/* sbdec.h - a small, plain-C door into FFmpeg's H.264 decoder for 360 programs.

   The program never includes FFmpeg's own headers (they need the c99-to-c89 build chain): it only uses these
   few functions, which live in sbdec.c, compiled together with FFmpeg into ffmpeg360.lib.

     sbdec_open(log, threads)   start the H.264 decoder; log = function for FFmpeg's messages (or NULL);
                                threads = how many pictures to decode at once (1 = no threads, up to 5:
                                the 360 has 6 hardware threads, 0 stays free for the program)
     sbdec_decode(data, size, &frame)
                                give it one packet (one picture's data, Annex-B: 00 00 01 start codes);
                                returns 1 when a finished picture is in frame, 0 when it needs more data,
                                below 0 on an error. data NULL / size 0 = "no more input: hand out what's left"
     sbdec_close()              free everything */
#ifndef SBDEC_H
#define SBDEC_H

typedef struct {
    int width, height;              /* picture size in pixels */
    const unsigned char *y, *u, *v; /* the three planes: brightness (full size), two colour planes (half size) */
    int y_stride, uv_stride;        /* bytes from one row to the next in each plane (can be more than width) */
    int pix_fmt;                    /* FFmpeg's pixel format number: 0 = YUV 4:2:0, the normal case */
} sbdec_frame;

#ifdef __cplusplus
extern "C" {
#endif

int sbdec_open(void (*log)(const char *line), int threads);
int sbdec_decode(const unsigned char *data, int size, sbdec_frame *out);
void sbdec_close(void);

#ifdef __cplusplus
}
#endif

#endif
