/* sbplayer.c - see sbplayer.h. Compiled with FFmpeg's build chain (build_ffmpeg.py part "sbdec").

   One background thread ("feeder") does all the FFmpeg work:
     - reads packets from one or two MP4 files (through our own read/seek functions: plain fopen now, the network
       later), always topping up whatever runs low: the picture queue (max VQ_MAX) or the sound buffer (max 2 s)
     - video packets -> H.264 decoder (with its own worker threads) -> finished pictures into the queue
       (FFmpeg's reference-counted frames: nothing is copied)
     - audio packets -> AAC decoder -> converted to 16-bit, channels interleaved -> ring buffer
   The program's thread takes from the queue / ring. One mutex guards both. */
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include "libavformat/avformat.h"
#include "libavcodec/avcodec.h"
#include "libavutil/channel_layout.h"
#include "sbplayer.h"
#include "sbhttp.h"
#include "sbhls.h"

#define VQ_MAX 8                    /* finished pictures kept ready (covers frame threading's slow moments) */
#define PQ_MAX 1024                 /* compressed packets waiting per stream (see "waiting lists" below) */
#define RING_SECONDS 2
#define IO_BUF 65536

typedef struct {                    /* one opened file: on the hard drive, or an https:// address */
    FILE *file;
    sbhttp *http;
    sbhls *hls;                     /* a LIVE stream (playlist of MPEG-TS pieces, see sbhls.c) */
    AVIOContext *io;
    AVFormatContext *fmt;
    int vstream, astream;           /* stream numbers inside this file, -1 = none */
    int eof;
} Source;

static Source g_src[2];             /* [0] = video file (maybe with sound), [1] = separate sound file */
static int g_nsrc;
static AVCodecContext *g_vdec, *g_adec;
static int g_vsrc = -1, g_asrc = -1;            /* which Source carries video / sound */
static AVRational g_vtb, g_atb;                  /* their time units */
static void (*g_log)(const char *line);

static pthread_t g_thread;
static pthread_mutex_t g_lock;
static volatile int g_stop, g_feeder_done;

static AVFrame *g_vq[VQ_MAX + 4];               /* picture queue (a few extra: one packet can give 2 pictures) */
static double g_vq_pts[VQ_MAX + 4];
static int g_vq_n;
static AVFrame *g_current;                       /* the picture handed to the program last */
static int g_shown, g_dropped;

/* WAITING LISTS of compressed packets, one for pictures and one for sound (feeder thread only, no lock needed).
   In a file with both, picture and sound packets come interleaved: reading on because the SOUND runs low also
   brings picture packets. Those used to be decoded at once even with a full picture queue, and the extra pictures
   were thrown away (first player test: NXE showed 323 of ~1,206 pictures). Now they wait here, compressed (small),
   until their queue has room. */
typedef struct { AVPacket *p[PQ_MAX]; int head, n; } PacketList;
static PacketList g_vpk, g_apk;
static int g_lost;                                  /* pictures that still found no room (should stay 0) */

static int list_put(PacketList *l, AVPacket *pkt)
{
    AVPacket *copy;
    if (l->n >= PQ_MAX)
        return 0;
    copy = av_packet_alloc();
    if (!copy)
        return 0;
    av_packet_move_ref(copy, pkt);
    l->p[(l->head + l->n) % PQ_MAX] = copy;
    l->n++;
    return 1;
}

static AVPacket *list_get(PacketList *l)
{
    AVPacket *p;
    if (!l->n)
        return NULL;
    p = l->p[l->head];
    l->head = (l->head + 1) % PQ_MAX;
    l->n--;
    return p;
}

static void list_clear(PacketList *l)
{
    AVPacket *p;
    while ((p = list_get(l)) != NULL)
        av_packet_free(&p);
    l->head = 0;
}

static short *g_ring;                           /* sound: 16-bit, interleaved */
static int g_ring_frames, g_ring_read, g_ring_count, g_channels, g_rate;

/* SEEKING: the program asks (sbplayer_seek), the feeder does it at the top of its next round: both files jump to
   the key picture at or before the time, decoders + queues are emptied, then everything decoded before the exact
   time is quietly thrown away (g_skip_*) so playback starts right where asked. g_seeking stays 1 until the first
   picture (or, with no video, sound) from there is ready - the program shows a still picture meanwhile. */
static volatile int g_seek_req, g_seeking;
static double g_seek_to;
static double g_skip_v = -1, g_skip_a = -1;     /* feeder only */

/* LIVE streams: no seeking, no end time. Their picture times don't start at 0 (a TV-style clock that has been
   running for hours), so they're shifted to start at the sound's first moment (= where the program's clock starts).
   A "discontinuity" (the broadcaster restarted, an ad break) makes the times jump: the shift is adjusted so the
   pictures just carry on. */
static int g_live;
static double g_vzero, g_voff, g_vlast;
static int g_vlast_set;

static void say(const char *fmt, ...)
{
    char line[512];
    va_list args;
    if (!g_log)
        return;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line) - 1, fmt, args);
    va_end(args);
    line[sizeof(line) - 1] = 0;
    g_log(line);
}

/* ---- our own file access for FFmpeg ---- */
static int read_cb(void *opaque, uint8_t *buf, int size)
{
    size_t n = fread(buf, 1, size, (FILE *)opaque);
    return n > 0 ? (int)n : AVERROR_EOF;
}

static int64_t seek_cb(void *opaque, int64_t offset, int whence)
{
    FILE *f = (FILE *)opaque;
    if (whence == AVSEEK_SIZE) {                 /* FFmpeg asks for the file size */
        __int64 here = _ftelli64(f), size;      /* (64-bit positions: movie files can be over 2 GB) */
        _fseeki64(f, 0, SEEK_END);
        size = _ftelli64(f);
        _fseeki64(f, here, SEEK_SET);
        return size;
    }
    if (_fseeki64(f, offset, whence & ~AVSEEK_FORCE))
        return -1;
    return _ftelli64(f);
}

/* the same two functions for an https:// address (sbhttp.c: chunked Range requests over verified HTTPS) */
static int http_read_cb(void *opaque, uint8_t *buf, int size)
{
    int n = sbhttp_read((sbhttp *)opaque, buf, size);
    return n > 0 ? n : (n == 0 ? AVERROR_EOF : AVERROR(EIO));
}

static int64_t http_seek_cb(void *opaque, int64_t offset, int whence)
{
    sbhttp *h = (sbhttp *)opaque;
    if (whence == AVSEEK_SIZE)
        return sbhttp_size(h);
    whence &= ~AVSEEK_FORCE;
    if (whence == SEEK_CUR) offset += sbhttp_tell(h);
    else if (whence == SEEK_END) offset += sbhttp_size(h);
    return sbhttp_seek(h, offset);
}

/* a live stream: one endless byte stream (no seeking) */
static int hls_read_cb(void *opaque, uint8_t *buf, int size)
{
    int n = sbhls_read((sbhls *)opaque, buf, size);
    return n > 0 ? n : (n == 0 ? AVERROR_EOF : AVERROR(EIO));
}

static int open_source(Source *s, const char *path)
{
    unsigned char *buf;
    int r;
    memset(s, 0, sizeof(*s));
    s->vstream = s->astream = -1;
    buf = av_malloc(IO_BUF);
    if (!strncmp(path, "hls:", 4)) {             /* "hls:https://..." = a live stream's playlist */
        s->hls = sbhls_open(path + 4, g_log);
        if (!s->hls) { say("sbplayer: can't start the live stream"); return -1; }
        s->io = avio_alloc_context(buf, IO_BUF, 0, s->hls, hls_read_cb, NULL, NULL);
        if (s->io) s->io->seekable = 0;
    } else if (!strncmp(path, "https://", 8)) {
        s->http = sbhttp_open(path);
        if (!s->http) { say("sbplayer: can't open the stream %.60s...", path); return -1; }
        say("sbplayer: stream open, %.1f MB", sbhttp_size(s->http) / 1048576.0);
        s->io = avio_alloc_context(buf, IO_BUF, 0, s->http, http_read_cb, NULL, http_seek_cb);
    } else {
        s->file = fopen(path, "rb");
        if (!s->file) { say("sbplayer: can't open %s", path); return -1; }
        s->io = avio_alloc_context(buf, IO_BUF, 0, s->file, read_cb, NULL, seek_cb);
    }
    s->fmt = avformat_alloc_context();
    if (!s->io || !s->fmt) return AVERROR(ENOMEM);
    s->fmt->pb = s->io;
    if (s->hls)                                  /* MPEG-TS, said straight away (no guessing from the first bytes) */
        r = avformat_open_input(&s->fmt, "live.ts", av_find_input_format("mpegts"), NULL);
    else
        r = avformat_open_input(&s->fmt, s->http ? "stream.mp4" : path, NULL, NULL);   /* (name: only a label) */
    if (r < 0) { say("sbplayer: %s is not a readable media file (%d)", path, r); return r; }
    r = avformat_find_stream_info(s->fmt, NULL);
    if (r < 0) { say("sbplayer: no stream info in %s (%d)", path, r); return r; }
    s->vstream = av_find_best_stream(s->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    s->astream = av_find_best_stream(s->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    return 0;
}

static void close_source(Source *s)
{
    if (s->fmt)
        avformat_close_input(&s->fmt);          /* (frees the context; our AVIOContext stays ours) */
    if (s->io) {
        av_freep(&s->io->buffer);
        av_freep(&s->io);
    }
    if (s->file)
        fclose(s->file);
    if (s->http) {
        long long bytes;
        int requests, connections;
        sbhttp_stats(s->http, &bytes, &requests, &connections);
        say("sbplayer: stream used %.1f MB in %d requests over %d connections", bytes / 1048576.0, requests,
            connections);
        sbhttp_close(s->http);
    }
    if (s->hls) {
        int pieces, waits, skipped;
        long long bytes;
        sbhls_stats(s->hls, &pieces, &bytes, &waits, &skipped);
        say("sbplayer: live stream used %d pieces, %.1f MB; reader waited %d times, %d pieces skipped", pieces,
            bytes / 1048576.0, waits, skipped);
        sbhls_close(s->hls);
    }
    memset(s, 0, sizeof(*s));
}

static AVCodecContext *open_decoder(AVStream *st, int threads)
{
    AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
    AVCodecContext *c;
    if (!codec) { say("sbplayer: no decoder for codec %d", st->codecpar->codec_id); return NULL; }
    c = avcodec_alloc_context3(codec);
    if (!c || avcodec_parameters_to_context(c, st->codecpar) < 0) return NULL;
    if (threads > 0) {
        c->thread_count = threads;
        c->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    }
    if (avcodec_open2(c, codec, NULL) < 0) { say("sbplayer: can't open the %s decoder", codec->name); return NULL; }
    return c;
}

/* ---- decoded sound -> 16-bit interleaved into the ring ---- */
static short to16(float f)
{
    int v = (int)(f * 32767.0f);
    return (short)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

static float smp(AVFrame *fr, int c, int i)      /* one sample of channel c, as -1..1 */
{
    switch (fr->format) {
    case AV_SAMPLE_FMT_FLTP: return ((float *)fr->extended_data[c])[i];
    case AV_SAMPLE_FMT_FLT:  return ((float *)fr->extended_data[0])[i * fr->channels + c];
    case AV_SAMPLE_FMT_S16P: return ((short *)fr->extended_data[c])[i] / 32768.0f;
    case AV_SAMPLE_FMT_S16:  return ((short *)fr->extended_data[0])[i * fr->channels + c] / 32768.0f;
    default: return 0;
    }
}

/* surround sound (5.1 movies) -> stereo: left = front left + 0.7 x (centre + left surround), right likewise. The
   centre channel holds the dialogue, so just taking the first two channels would lose the voices. LFE (bass) dropped */
static int downmix(AVFrame *fr, int i, short *out)
{
    uint64_t lay = fr->channel_layout ? fr->channel_layout : av_get_default_channel_layout(fr->channels);
    int fl = av_get_channel_layout_channel_index(lay, AV_CH_FRONT_LEFT);
    int fr_ = av_get_channel_layout_channel_index(lay, AV_CH_FRONT_RIGHT);
    int fc = av_get_channel_layout_channel_index(lay, AV_CH_FRONT_CENTER);
    int sl = av_get_channel_layout_channel_index(lay, AV_CH_SIDE_LEFT);
    int sr = av_get_channel_layout_channel_index(lay, AV_CH_SIDE_RIGHT);
    float l, r, c;
    if (sl < 0) sl = av_get_channel_layout_channel_index(lay, AV_CH_BACK_LEFT);
    if (sr < 0) sr = av_get_channel_layout_channel_index(lay, AV_CH_BACK_RIGHT);
    if (fl < 0 || fr_ < 0 || fl >= fr->channels || fr_ >= fr->channels) return 0;
    c = fc >= 0 && fc < fr->channels ? smp(fr, fc, i) * 0.707f : 0;
    l = smp(fr, fl, i) + c + (sl >= 0 && sl < fr->channels ? smp(fr, sl, i) * 0.707f : 0);
    r = smp(fr, fr_, i) + c + (sr >= 0 && sr < fr->channels ? smp(fr, sr, i) * 0.707f : 0);
    out[0] = to16(l * 0.6f);
    out[1] = to16(r * 0.6f);
    return 1;
}

static void push_audio(AVFrame *fr)
{
    int n = fr->nb_samples, ch = g_channels, i = 0, c, w;
    if (g_skip_a >= 0) {                        /* just after a seek: drop the sound before the asked time */
        int64_t t = fr->best_effort_timestamp != AV_NOPTS_VALUE ? fr->best_effort_timestamp : fr->pts;
        if (t != AV_NOPTS_VALUE && g_rate > 0) {
            double start = t * av_q2d(g_atb), end = start + (double)n / g_rate;
            if (end <= g_skip_a) return;
            if (start < g_skip_a) i = (int)((g_skip_a - start) * g_rate);
        }
        g_skip_a = -1;
    }
    pthread_mutex_lock(&g_lock);
    if (g_vsrc < 0) g_seeking = 0;              /* sound only: ready as soon as there's sound */
    for (; i < n && g_ring_count < g_ring_frames; i++) {
        w = (g_ring_read + g_ring_count) % g_ring_frames;
        if (fr->channels > 2 && ch == 2 && downmix(fr, i, &g_ring[w * 2])) { g_ring_count++; continue; }
        for (c = 0; c < ch; c++) {
            int src_c = c < fr->channels ? c : 0;          /* (mono file on stereo output: copy) */
            short s;
            switch (fr->format) {
            case AV_SAMPLE_FMT_FLTP: s = to16(((float *)fr->extended_data[src_c])[i]); break;
            case AV_SAMPLE_FMT_FLT:  s = to16(((float *)fr->extended_data[0])[i * fr->channels + src_c]); break;
            case AV_SAMPLE_FMT_S16P: s = ((short *)fr->extended_data[src_c])[i]; break;
            case AV_SAMPLE_FMT_S16:  s = ((short *)fr->extended_data[0])[i * fr->channels + src_c]; break;
            default: s = 0;
            }
            g_ring[w * ch + c] = s;
        }
        g_ring_count++;
    }
    pthread_mutex_unlock(&g_lock);
}

static void push_video(AVFrame *fr)
{
    AVFrame *copy = av_frame_alloc();
    int64_t t = fr->best_effort_timestamp != AV_NOPTS_VALUE ? fr->best_effort_timestamp : fr->pts;
    double secs;
    if (g_skip_v >= 0) {                        /* just after a seek: pictures before the asked time aren't shown */
        if (t != AV_NOPTS_VALUE && t * av_q2d(g_vtb) < g_skip_v - 0.02) { av_frame_free(&copy); return; }
        g_skip_v = -1;
    }
    if (!copy || av_frame_ref(copy, fr) < 0) { av_frame_free(&copy); return; }
    secs = t != AV_NOPTS_VALUE ? t * av_q2d(g_vtb) : 0.0;
    if (g_live) {
        secs = secs - g_vzero + g_voff;
        if (t == AV_NOPTS_VALUE && g_vlast_set)
            secs = g_vlast + 1.0 / 30;
        else if (g_vlast_set && (secs < g_vlast - 1.0 || secs > g_vlast + 5.0)) {
            say("sbplayer: live picture times jumped by %.1f s, carrying on", secs - g_vlast);
            g_voff += g_vlast + 1.0 / 30 - secs;
            secs = g_vlast + 1.0 / 30;
        }
        g_vlast = secs;
        g_vlast_set = 1;
    }
    pthread_mutex_lock(&g_lock);
    g_seeking = 0;
    if (g_vq_n < VQ_MAX + 4) {
        g_vq[g_vq_n] = copy;
        g_vq_pts[g_vq_n] = secs;
        g_vq_n++;
        copy = NULL;
    }
    if (copy)
        g_lost++;                               /* queue over-full: counted, so it shows in the log */
    pthread_mutex_unlock(&g_lock);
    av_frame_free(&copy);
}

/* send one packet (or NULL = "no more") to a decoder and collect everything it hands back */
static void decode(AVCodecContext *dec, AVPacket *pkt, int is_video, AVFrame *fr)
{
    if (avcodec_send_packet(dec, pkt) < 0 && pkt)
        return;
    while (avcodec_receive_frame(dec, fr) == 0) {
        if (is_video) push_video(fr); else push_audio(fr);
        av_frame_unref(fr);
    }
}

/* read one packet from file s and put it on the right waiting list (others are skipped). 0 = file finished */
static int read_into_lists(int s)
{
    AVPacket pkt;
    av_init_packet(&pkt);
    if (g_src[s].eof || av_read_frame(g_src[s].fmt, &pkt) < 0) {
        g_src[s].eof = 1;
        return 0;
    }
    if (s == g_vsrc && pkt.stream_index == g_src[s].vstream)
        list_put(&g_vpk, &pkt);
    else if (s == g_asrc && pkt.stream_index == g_src[s].astream)
        list_put(&g_apk, &pkt);
    av_packet_unref(&pkt);                       /* (no-op when moved onto a list) */
    return 1;
}

/* ---- the feeder thread ----
   Each round: whatever is low (picture queue / sound buffer) gets ONE packet decoded from its waiting list;
   an empty waiting list is refilled by reading the file that carries that stream. */
static void do_seek(double t)                   /* feeder thread */
{
    int i;
    for (i = 0; i < g_nsrc; i++) {
        Source *s = &g_src[i];
        int st = s->vstream >= 0 ? s->vstream : s->astream;
        if (st < 0) continue;
        if (av_seek_frame(s->fmt, st, (int64_t)(t / av_q2d(s->fmt->streams[st]->time_base)),
                          AVSEEK_FLAG_BACKWARD) < 0)
            say("sbplayer: seek to %.1f s failed in file %d", t, i);
        s->eof = 0;
    }
    list_clear(&g_vpk);
    list_clear(&g_apk);
    if (g_vdec) avcodec_flush_buffers(g_vdec);
    if (g_adec) avcodec_flush_buffers(g_adec);
    pthread_mutex_lock(&g_lock);
    for (i = 0; i < g_vq_n; i++) av_frame_free(&g_vq[i]);
    g_vq_n = 0;
    g_ring_count = g_ring_read = 0;
    pthread_mutex_unlock(&g_lock);
    g_skip_v = g_vsrc >= 0 ? t : -1;
    g_skip_a = g_asrc >= 0 ? t : -1;
}

static void *feeder(void *unused)
{
    AVFrame *fr = av_frame_alloc();
    int vdone = g_vsrc < 0, adone = g_asrc < 0;
    (void)unused;
    while (!g_stop) {
        int need_v, need_a, worked = 0, seek = 0;
        double seek_to = 0;
        AVPacket *p;
        pthread_mutex_lock(&g_lock);
        if (g_seek_req) { seek = 1; seek_to = g_seek_to; g_seek_req = 0; }
        pthread_mutex_unlock(&g_lock);
        if (seek) {
            do_seek(seek_to);
            vdone = g_vsrc < 0;
            adone = g_asrc < 0;
            g_feeder_done = 0;
        }
        if (vdone && adone) {                   /* both files finished: wait (a seek may still come) */
            pthread_mutex_lock(&g_lock);
            g_seeking = 0;
            pthread_mutex_unlock(&g_lock);
            g_feeder_done = 1;
            sb_sleep_ms(10);
            continue;
        }
        pthread_mutex_lock(&g_lock);
        need_v = !vdone && g_vq_n < VQ_MAX;
        need_a = !adone && g_ring_count < g_ring_frames - 8192;
        pthread_mutex_unlock(&g_lock);

        if (need_v) {
            if (g_vpk.n == 0 && !g_src[g_vsrc].eof && g_apk.n < PQ_MAX && read_into_lists(g_vsrc))
                worked = 1;                      /* (may bring a sound packet instead: it waits on its list) */
            if ((p = list_get(&g_vpk)) != NULL) {
                decode(g_vdec, p, 1, fr);
                av_packet_free(&p);
                worked = 1;
            } else if (g_src[g_vsrc].eof) {      /* file finished, list empty: empty the decoder */
                decode(g_vdec, NULL, 1, fr);
                vdone = 1;
                worked = 1;
            }
        }
        if (need_a) {
            if (g_apk.n == 0 && !g_src[g_asrc].eof && g_vpk.n < PQ_MAX && read_into_lists(g_asrc))
                worked = 1;
            if ((p = list_get(&g_apk)) != NULL) {
                decode(g_adec, p, 0, fr);
                av_packet_free(&p);
                worked = 1;
            } else if (g_src[g_asrc].eof) {
                decode(g_adec, NULL, 0, fr);
                adone = 1;
                worked = 1;
            }
        }
        if (!worked)
            sb_sleep_ms(2);                      /* everything topped up (or waiting): 2 ms (sb_pthread.c) */
    }
    av_frame_free(&fr);
    g_feeder_done = 1;
    return NULL;
}

void sbplayer_seek(double seconds)
{
    if (g_live)                                 /* a live stream can't jump */
        return;
    if (seconds < 0) seconds = 0;
    pthread_mutex_lock(&g_lock);
    g_seek_to = seconds;
    g_seek_req = 1;
    g_seeking = 1;                              /* from now on no old pictures / sound are handed out */
    pthread_mutex_unlock(&g_lock);
}

int sbplayer_seeking(void)
{
    return g_seeking;
}

void sbplayer_live_url(const char *url)
{
    if (!g_live || !g_src[0].hls || !url) return;
    sbhls_set_url(g_src[0].hls, !strncmp(url, "hls:", 4) ? url + 4 : url);
}

int sbplayer_open(const char *video_path, const char *audio_path, void (*log)(const char *line), int threads,
                  sbplayer_info *info)
{
    int r, i;
    memset(info, 0, sizeof(*info));
    g_log = log;
    avcodec_register_all();
    av_register_all();
    g_nsrc = 0;
    g_vsrc = g_asrc = -1;
    if ((r = open_source(&g_src[0], video_path)) < 0) return r;
    g_nsrc = 1;
    if (audio_path) {
        if ((r = open_source(&g_src[1], audio_path)) < 0) return r;
        g_nsrc = 2;
    }
    for (i = 0; i < g_nsrc; i++) {               /* video from the first file that has it, sound likewise */
        if (g_vsrc < 0 && g_src[i].vstream >= 0) g_vsrc = i;
        if (g_asrc < 0 && g_src[i].astream >= 0) g_asrc = i;
    }
    if (audio_path && g_src[1].astream >= 0)
        g_asrc = 1;                              /* a separate sound file wins */
    if (g_vsrc >= 0) {
        AVStream *st = g_src[g_vsrc].fmt->streams[g_src[g_vsrc].vstream];
        if (st->codecpar->codec_id != AV_CODEC_ID_H264) {
            say("sbplayer: the video isn't H.264 (codec %d)", st->codecpar->codec_id);
            return -10;
        }
        /* 10-bit pictures (some movie encodes) would come out as garbled colours: the screen takes 8-bit only */
        if (st->codecpar->format != AV_PIX_FMT_NONE && st->codecpar->format != AV_PIX_FMT_YUV420P &&
            st->codecpar->format != AV_PIX_FMT_YUVJ420P) {
            say("sbplayer: picture format %d isn't 8-bit 4:2:0", st->codecpar->format);
            return -12;
        }
        g_vdec = open_decoder(st, threads);
        if (!g_vdec) return -10;
        g_vtb = st->time_base;
        info->has_video = 1;
        info->width = st->codecpar->width;
        info->height = st->codecpar->height;
    }
    if (g_asrc >= 0) {
        AVStream *st = g_src[g_asrc].fmt->streams[g_src[g_asrc].astream];
        g_adec = open_decoder(st, 0);
        if (g_adec) {
            g_atb = st->time_base;
            g_channels = st->codecpar->channels > 2 ? 2 : (st->codecpar->channels < 1 ? 1 : st->codecpar->channels);
            info->has_audio = 1;
            info->sample_rate = st->codecpar->sample_rate;
            g_rate = st->codecpar->sample_rate;
            info->channels = g_channels;
            g_ring_frames = st->codecpar->sample_rate * RING_SECONDS;
            g_ring = (short *)av_malloc(g_ring_frames * g_channels * sizeof(short));
            g_ring_read = g_ring_count = 0;
        } else
            g_asrc = -1;
    }
    g_live = g_src[0].hls != NULL;
    g_voff = g_vlast = 0;
    g_vlast_set = 0;
    g_vzero = 0;
    if (g_live) {                                /* picture times count from the sound's first moment */
        AVStream *ast = g_asrc >= 0 ? g_src[g_asrc].fmt->streams[g_src[g_asrc].astream] : NULL;
        if (ast && ast->start_time != AV_NOPTS_VALUE)
            g_vzero = ast->start_time * av_q2d(ast->time_base);
        else if (g_src[0].fmt->start_time != AV_NOPTS_VALUE)
            g_vzero = g_src[0].fmt->start_time / (double)AV_TIME_BASE;
        info->live = 1;
    } else if (g_src[0].fmt->duration > 0)
        info->duration = g_src[0].fmt->duration / (double)AV_TIME_BASE;
    g_vq_n = 0;
    g_current = NULL;
    g_shown = g_dropped = g_lost = 0;
    g_stop = g_feeder_done = 0;
    g_seek_req = g_seeking = 0;
    g_skip_v = g_skip_a = -1;
    pthread_mutex_init(&g_lock, NULL);
    if (pthread_create(&g_thread, NULL, feeder, NULL))
        return -11;
    say("sbplayer: video %s (%dx%d, %d threads), sound %s (%d Hz, %d ch), %.1f s%s",
        info->has_video ? "yes" : "no", info->width, info->height, threads,
        info->has_audio ? "yes" : "no", info->sample_rate, info->channels, info->duration,
        g_live ? " (LIVE)" : "");
    return 0;
}

int sbplayer_audio(short *out, int max_frames)
{
    int n, i, ch = g_channels;
    if (!g_ring)
        return 0;
    pthread_mutex_lock(&g_lock);
    n = g_seeking ? 0 : (g_ring_count < max_frames ? g_ring_count : max_frames);
    for (i = 0; i < n; i++) {
        memcpy(out + i * ch, g_ring + g_ring_read * ch, ch * sizeof(short));
        g_ring_read = (g_ring_read + 1) % g_ring_frames;
    }
    g_ring_count -= n;
    pthread_mutex_unlock(&g_lock);
    return n;
}

/* the queue isn't strictly in time order with B-frames? FFmpeg hands frames out in DISPLAY order, so it is */
int sbplayer_video(double now, sbdec_frame *out, double *pts)
{
    AVFrame *pick = NULL;
    double pick_pts = 0;
    int i, n;
    pthread_mutex_lock(&g_lock);
    /* every picture whose time has come: the newest one is shown, older ones are dropped (too late) */
    for (n = 0; !g_seeking && n < g_vq_n && g_vq_pts[n] <= now; n++) {
        if (pick) { av_frame_free(&pick); g_dropped++; }
        pick = g_vq[n];
        pick_pts = g_vq_pts[n];
    }
    for (i = n; i < g_vq_n; i++) {
        g_vq[i - n] = g_vq[i];
        g_vq_pts[i - n] = g_vq_pts[i];
    }
    g_vq_n -= n;
    pthread_mutex_unlock(&g_lock);
    if (!pick)
        return 0;
    av_frame_free(&g_current);                  /* the program is done with the previous picture */
    g_current = pick;
    g_shown++;
    out->width = pick->width;
    out->height = pick->height;
    out->y = pick->data[0];
    out->u = pick->data[1];
    out->v = pick->data[2];
    out->y_stride = pick->linesize[0];
    out->uv_stride = pick->linesize[1];
    out->pix_fmt = pick->format;
    if (pts) *pts = pick_pts;
    return 1;
}

int sbplayer_finished(void)
{
    int done;
    pthread_mutex_lock(&g_lock);
    done = g_feeder_done && !g_seeking && !g_seek_req && g_vq_n == 0 && g_ring_count == 0;
    pthread_mutex_unlock(&g_lock);
    return done;
}

void sbplayer_stats(int *shown, int *dropped)
{
    *shown = g_shown;
    *dropped = g_dropped;
}

void sbplayer_close(void)
{
    int i;
    g_stop = 1;
    for (i = 0; i < g_nsrc; i++)                /* a feeder waiting for the next live piece: let it go */
        sbhls_abort(g_src[i].hls);
    if (g_thread) {
        pthread_join(g_thread, NULL);
        g_thread = NULL;
    }
    say("sbplayer: %d pictures shown, %d dropped (too late), %d lost (no room - should be 0)",
        g_shown, g_dropped, g_lost);
    for (i = 0; i < g_vq_n; i++)
        av_frame_free(&g_vq[i]);
    g_vq_n = 0;
    list_clear(&g_vpk);
    list_clear(&g_apk);
    av_frame_free(&g_current);
    avcodec_free_context(&g_vdec);
    avcodec_free_context(&g_adec);
    for (i = 0; i < g_nsrc; i++)
        close_source(&g_src[i]);
    g_nsrc = 0;
    av_freep(&g_ring);
    g_ring_count = g_ring_read = 0;
    g_live = 0;
    pthread_mutex_destroy(&g_lock);
}
