/* sbimage.c - see sbimage.h. JPEG = brightness + colour planes (YUV, "full range": black 0, white 255), usually the
   colour planes at half size. This turns them into 0xAARRGGBB pixels with the standard JPEG (BT.601) formula. */
#include <stdlib.h>
#include <string.h>
#include "libavcodec/avcodec.h"
#include "libavutil/pixdesc.h"
#include "sbimage.h"

static unsigned char clamp255(int v) { return (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v); }

unsigned int *sbimage_decode_jpeg(const unsigned char *data, int size, int *w_out, int *h_out)
{
    AVCodec *codec;
    AVCodecContext *ctx = NULL;
    AVPacket *pkt = NULL;
    AVFrame *fr = NULL;
    unsigned char *copy = NULL;
    unsigned int *out = NULL;
    const AVPixFmtDescriptor *d;
    int x, y, w, h, sx, sy, gray;

    avcodec_register_all();
    codec = avcodec_find_decoder(AV_CODEC_ID_MJPEG);
    if (!codec || size <= 0) return NULL;
    ctx = avcodec_alloc_context3(codec);
    pkt = av_packet_alloc();
    fr = av_frame_alloc();
    copy = (unsigned char *)av_malloc(size + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!ctx || !pkt || !fr || !copy) goto done;
    memcpy(copy, data, size);
    memset(copy + size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    ctx->thread_count = 1;
    if (avcodec_open2(ctx, codec, NULL) < 0) goto done;
    pkt->data = copy;
    pkt->size = size;
    if (avcodec_send_packet(ctx, pkt) < 0 || avcodec_receive_frame(ctx, fr) < 0) goto done;

    w = fr->width;
    h = fr->height;
    d = av_pix_fmt_desc_get((enum AVPixelFormat)fr->format);
    if (!d || w <= 0 || h <= 0 || w > 4096 || h > 4096) goto done;
    gray = d->nb_components == 1;
    if (!gray && !(d->flags & AV_PIX_FMT_FLAG_PLANAR)) goto done;   /* only the planar YUV kinds JPEG gives */
    sx = d->log2_chroma_w;
    sy = d->log2_chroma_h;
    out = (unsigned int *)malloc((size_t)w * h * 4);
    if (!out) goto done;
    for (y = 0; y < h; y++) {
        const unsigned char *yp = fr->data[0] + y * fr->linesize[0];
        const unsigned char *up = gray ? NULL : fr->data[1] + (y >> sy) * fr->linesize[1];
        const unsigned char *vp = gray ? NULL : fr->data[2] + (y >> sy) * fr->linesize[2];
        unsigned int *o = out + y * w;
        for (x = 0; x < w; x++) {
            int Y = yp[x];
            if (gray) {
                o[x] = 0xFF000000u | (Y << 16) | (Y << 8) | Y;
            } else {
                int U = up[x >> sx] - 128, V = vp[x >> sx] - 128;
                /* full-range BT.601 in 1/1024 steps: R = Y + 1.402 V, G = Y - 0.344 U - 0.714 V, B = Y + 1.772 U */
                int r = Y + ((1436 * V) >> 10);
                int g = Y - ((352 * U + 731 * V) >> 10);
                int b = Y + ((1815 * U) >> 10);
                o[x] = 0xFF000000u | (clamp255(r) << 16) | (clamp255(g) << 8) | clamp255(b);
            }
        }
    }
    *w_out = w;
    *h_out = h;
done:
    av_frame_free(&fr);
    av_packet_free(&pkt);
    avcodec_free_context(&ctx);
    av_free(copy);
    return out;
}

void sbimage_free(void *p) { free(p); }
