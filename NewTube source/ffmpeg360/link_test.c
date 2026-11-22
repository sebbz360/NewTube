/* link_test.c - does everything we build link into a 360 program? (built by link_test.py, not run)
   Uses the same calls the player will: the MP4 reader with our own read function, the H.264 and AAC decoders. */
#include "libavformat/avformat.h"
#include "libavcodec/avcodec.h"
#include "sbimage.h"

static int read_cb(void *opaque, uint8_t *buf, int size) { (void)opaque; (void)buf; (void)size; return AVERROR_EOF; }
static int64_t seek_cb(void *opaque, int64_t off, int whence) { (void)opaque; (void)off; (void)whence; return -1; }

int sb_ffmpeg_link_test(void)
{
    AVCodec *h264, *aac;
    AVCodecContext *ctx;
    AVFrame *frame;
    AVPacket pkt;
    AVFormatContext *fmt;
    AVIOContext *io;
    unsigned char *iobuf;

    avcodec_register_all();
    av_register_all();
    h264 = avcodec_find_decoder(AV_CODEC_ID_H264);
    aac = avcodec_find_decoder(AV_CODEC_ID_AAC);
    if (!h264 || !aac)
        return -1;
    ctx = avcodec_alloc_context3(h264);
    if (!ctx || avcodec_open2(ctx, h264, NULL) < 0)
        return -2;
    frame = av_frame_alloc();
    av_init_packet(&pkt);
    pkt.data = NULL;
    pkt.size = 0;
    avcodec_send_packet(ctx, &pkt);
    avcodec_receive_frame(ctx, frame);

    iobuf = av_malloc(32768);
    io = avio_alloc_context(iobuf, 32768, 0, NULL, read_cb, NULL, seek_cb);
    fmt = avformat_alloc_context();
    fmt->pb = io;
    if (avformat_open_input(&fmt, NULL, NULL, NULL) >= 0) {
        avformat_find_stream_info(fmt, NULL);
        while (av_read_frame(fmt, &pkt) >= 0)
            av_packet_unref(&pkt);
        avformat_close_input(&fmt);
    }
    av_frame_free(&frame);
    avcodec_free_context(&ctx);
    {   /* the JPEG door (thumbnails) */
        int w, h;
        static const unsigned char not_a_jpeg[4] = { 0xFF, 0xD8, 0xFF, 0xD9 };
        sbimage_free(sbimage_decode_jpeg(not_a_jpeg, 4, &w, &h));
    }
    return 0;
}

void __cdecl main(void)
{
    sb_ffmpeg_link_test();
}
