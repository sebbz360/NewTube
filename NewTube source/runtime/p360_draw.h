/* p360_draw.h - 2D drawing for menus: coloured boxes, pictures, and TEXT, on a 1280 x 720 screen (pixels, 0,0 = top
   left). Call between p360_frame_begin and p360_frame_end (p360_video.h); things drawn later cover earlier ones.
   Colours are 0xAARRGGBB (AA = opacity: FF solid, 80 half see-through).

     p360_draw_init()                          once, after p360_video_init
     p360_rect(x, y, w, h, colour)             filled box
     p360_rect_v(x, y, w, h, top, bottom)      box shading from one colour at the top to another at the bottom
     p360_frame_rect(x, y, w, h, t, colour)    outline t pixels thick (inside the box)

     p360_image_create(w, h, pixels)           a picture from 0xAARRGGBB pixels (row by row); NULL = failed
     p360_image_draw(img, x, y, w, h, tint)    draw it stretched to that box (tint 0xFFFFFFFF = as it is)
     p360_image_free(img)

     p360_font_load("game:\\fonts\\x.sbf")    a letter sheet made by tools\make_font.py; NULL = failed
     p360_text(f, x, y, size, colour, text)    one line of UTF-8 text; (x, y) = top-left of the line; size = letter
                                               height in pixels (a capital is ~0.72 of it). Returns where it ended (x)
     p360_text_width(f, size, text)            how wide that line would be
     p360_text_line(f, size)                   the distance from one line to the next
     p360_text_wrap(f, x, y, w, size, max_lines, colour, text)
                                               word-wrapped into width w; at most max_lines lines (0 = no limit),
                                               the last one ends in "..." if the text didn't fit. Returns the lines used
     p360_text_fit(f, x, y, w, size, colour, text)
                                               one line, cut with "..." if wider than w */
#ifndef P360_DRAW_H
#define P360_DRAW_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct p360_image p360_image;
typedef struct p360_font p360_font;

int p360_draw_init(void);
/* safe area: p360_draw_safe(1) = everything drawn after it is shrunk into the inner 90 % of the screen (TVs may cut
   off the edges), so a layout can be designed for the whole 1280 x 720 and still fit. p360_draw_safe(0) = full
   screen again (for backgrounds that should reach the edges). p360_draw_transform = any scale + move */
void p360_draw_safe(int on);
void p360_draw_transform(float scale, float ox, float oy);
/* clip: only the part of later drawings inside this box shows (same coordinates as the drawings) */
void p360_clip(float x, float y, float w, float h);
void p360_clip_off(void);
void p360_rect(float x, float y, float w, float h, unsigned int argb);
void p360_rect_v(float x, float y, float w, float h, unsigned int top_argb, unsigned int bottom_argb);
void p360_frame_rect(float x, float y, float w, float h, float thickness, unsigned int argb);

p360_image *p360_image_create(int w, int h, const unsigned int *argb);
p360_image *p360_image_load(const char *path);      /* an .sbi file from tools\make_ui_art.py */
void p360_image_size(p360_image *img, int *w, int *h);
void p360_image_draw(p360_image *img, float x, float y, float w, float h, unsigned int tint);
/* only the part (sx, sy, sw, sh) of the picture, in its own pixels (e.g. one frame of a sheet of frames) */
void p360_image_draw_part(p360_image *img, float sx, float sy, float sw, float sh,
                          float x, float y, float w, float h, unsigned int tint);
void p360_image_free(p360_image *img);

p360_font *p360_font_load(const char *path);
float p360_text(p360_font *f, float x, float y, float size, unsigned int argb, const char *utf8);
float p360_text_width(p360_font *f, float size, const char *utf8);
float p360_text_line(p360_font *f, float size);
int p360_text_wrap(p360_font *f, float x, float y, float w, float size, int max_lines, unsigned int argb,
                   const char *utf8);
void p360_text_fit(p360_font *f, float x, float y, float w, float size, unsigned int argb, const char *utf8);

#ifdef __cplusplus
}
#endif

#endif
void p360_text_boost(float b);   /* larger text: 0 = off, 0.5 = small sizes grow halfway to 22 */
