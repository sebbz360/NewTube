/* sbimage.h - decode a JPEG picture (e.g. a YouTube thumbnail) with FFmpeg's JPEG decoder.

     sbimage_decode_jpeg(data, size, &w, &h)  -> malloc'd pixels, 0xAARRGGBB row by row (w * h), or NULL.
                                                 Free with sbimage_free. Thread-safe: each call has its own decoder. */
#ifndef SBIMAGE_H
#define SBIMAGE_H

#ifdef __cplusplus
extern "C" {
#endif

unsigned int *sbimage_decode_jpeg(const unsigned char *data, int size, int *w, int *h);
void sbimage_free(void *p);

#ifdef __cplusplus
}
#endif

#endif
