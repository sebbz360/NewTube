/* sbdec.c - see sbdec.h. Compiled with FFmpeg's build chain (build_ffmpeg.py part "sbdec"). */
#include <string.h>
#include <stdio.h>
#include "libavcodec/avcodec.h"
#include "libavutil/frame.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"
#include "sbdec.h"

static AVCodecContext *g_ctx;
static AVFrame *g_frame;
static AVPacket g_pkt;
static unsigned char *g_buf;        /* packet copy with FFmpeg's required zero padding after it */
static int g_buf_size;
static void (*g_log)(const char *line);

/* FFmpeg's messages ("missing SPS", "error while decoding MB ...") go to the program's log */
static void log_callback(void *avcl, int level, const char *fmt, va_list vl)
{
    char line[512];
    (void)avcl;
    if (!g_log || level > AV_LOG_INFO)
        return;
    vsnprintf(line, sizeof(line) - 1, fmt, vl);
    line[sizeof(line) - 1] = 0;
    {   /* FFmpeg's lines end in \n; the log adds its own */
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = 0;
    }
    g_log(line);
}

int sbdec_open(void (*log)(const char *line), int threads)
{
    AVCodec *codec;
    int r;

    g_log = log;
    av_log_set_callback(log_callback);
    av_log_set_level(AV_LOG_INFO);
    avcodec_register_all();
    codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec)
        return -1;
    g_ctx = avcodec_alloc_context3(codec);
    if (!g_ctx)
        return -2;
    /* FFmpeg can't count the 360's cores itself, so the number is given. FRAME threading = several pictures
       decoded at the same time (each thread one picture, waiting only for the parts of earlier pictures it
       needs); SLICE = one picture split into parts, when the video was made with several slices */
    if (threads < 1) threads = 1;
    if (threads > 5) threads = 5;
    g_ctx->thread_count = threads;
    g_ctx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    r = avcodec_open2(g_ctx, codec, NULL);
    if (r < 0)
        return r;
    g_frame = av_frame_alloc();
    av_init_packet(&g_pkt);
    return 0;
}

int sbdec_decode(const unsigned char *data, int size, sbdec_frame *out)
{
    int r;

    if (!g_ctx)
        return -1;
    if (data && size > 0) {
        if (size + AV_INPUT_BUFFER_PADDING_SIZE > g_buf_size) {
            av_free(g_buf);
            g_buf_size = size + AV_INPUT_BUFFER_PADDING_SIZE;
            g_buf = av_malloc(g_buf_size);
            if (!g_buf)
                return AVERROR(ENOMEM);
        }
        memcpy(g_buf, data, size);
        memset(g_buf + size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
        g_pkt.data = g_buf;
        g_pkt.size = size;
    } else {
        g_pkt.data = NULL;          /* "drain": no more input */
        g_pkt.size = 0;
    }
    r = avcodec_send_packet(g_ctx, &g_pkt);
    if (r < 0 && r != AVERROR(EAGAIN) && r != AVERROR_EOF)
        return r;
    r = avcodec_receive_frame(g_ctx, g_frame);
    if (r == AVERROR(EAGAIN) || r == AVERROR_EOF)
        return 0;
    if (r < 0)
        return r;
    out->width = g_frame->width;
    out->height = g_frame->height;
    out->y = g_frame->data[0];
    out->u = g_frame->data[1];
    out->v = g_frame->data[2];
    out->y_stride = g_frame->linesize[0];
    out->uv_stride = g_frame->linesize[1];
    out->pix_fmt = g_frame->format;
    return 1;
}

void sbdec_close(void)
{
    av_frame_free(&g_frame);
    avcodec_free_context(&g_ctx);
    av_free(g_buf);
    g_buf = NULL;
    g_buf_size = 0;
}
