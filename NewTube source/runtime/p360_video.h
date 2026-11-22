/* p360_video.h - the graphics start-up, the FRAME (one TV picture), and VIDEO pictures (YUV 4:2:0, the format video
   decoders give) drawn by the GPU (colours converted by video_ps.hlsl).

   Simple use (a player that only shows video):
     p360_video_init(vsync)      start the graphics; vsync 1 = wait for the TV (smooth playback), 0 = don't (speed tests)
     p360_video_show(...)        put one picture on the whole TV: its size, the three planes (Y full size, U and V half
                                 size) and the bytes per row of each (Y; U and V share one). Black bars keep its shape.
     p360_video_redraw()         show the last picture again (keeps the TV fed while waiting for the next one)

   Building a screen from parts (menus with video, boxes, pictures, text: see p360_draw.h):
     p360_frame_begin(colour)    start a new TV picture, filled with one colour (0xAARRGGBB)
     p360_video_upload(...)      hand over a new video picture (same arguments as show) without drawing it
     p360_video_draw(x,y,w,h)    draw the latest video picture into that rectangle (pixels, 1280 x 720 screen),
                                 keeping its shape; returns 0 if there is no picture yet
     ... p360_draw_* calls ...
     p360_frame_end()            show the finished picture on the TV */
#ifndef P360_VIDEO_H
#define P360_VIDEO_H

#ifdef __cplusplus
extern "C" {
#endif

int p360_video_init(int vsync);
void p360_video_show(int width, int height, const unsigned char *y, const unsigned char *u, const unsigned char *v,
                     int y_stride, int uv_stride);
void p360_video_redraw(void);

void p360_frame_begin(unsigned int clear_argb);
void p360_frame_end(void);
int p360_video_upload(int width, int height, const unsigned char *y, const unsigned char *u, const unsigned char *v,
                      int y_stride, int uv_stride);
int p360_video_draw(float x, float y, float w, float h);
void *p360_device(void);        /* the D3D device (for p360_draw.c) */

#ifdef __cplusplus
}
#endif

#endif
int p360_letterboxed(void);   /* 1 = a 4:3 TV: everything is drawn letterboxed */
int p360_display_sd(void);    /* 1 = a standard-definition TV picture (480i / 480p) */
