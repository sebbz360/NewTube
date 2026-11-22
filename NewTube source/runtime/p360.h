#ifndef P360_H
#define P360_H

#include <xtl.h>

/* C++ files (like 02_video's main.cpp) call these C functions too: extern "C" tells the C++ compiler
   "these were compiled as plain C, use their plain names" (C++ otherwise expects decorated names) */
#ifdef __cplusplus
extern "C" {
#endif

void p360_log(const char *text);
void p360_log_close(void);

WORD p360_pad_pressed(int pad);
DWORD p360_ticks(void);

/* ---- pixel screen (p360_screen.c): a width x height picture, stretched over the whole TV ---- */
int p360_screen_init(int width, int height);   /* 1 = ready, 0 = failed (see the log) */
int p360_screen_width(void);
int p360_screen_height(void);
DWORD *p360_screen_begin(int *pitch);          /* the pixels of this frame; *pitch = pixels per row */
void p360_screen_end(void);                    /* show them on the TV */

/* a colour from red, green, blue (each 0-255): P360_RGB(255, 0, 0) = red */
#define P360_RGB(r, g, b) (0xFF000000 | ((r) << 16) | ((g) << 8) | (b))

#ifdef __cplusplus
}
#endif

#endif