#include <stdio.h>
#include "p360.h"

static FILE *g_log;

/* ---- 1. A tiny log: every line goes to game:\hello_log.txt ---------------------------
   "game:" is the folder the .xex was started from (like the gMod module's game:\gmod\...). */
void p360_log(const char *text)
{
    if (!g_log)
        g_log = fopen("game:\\log.txt", "w");
    if (g_log) {
        fputs(text, g_log);
        fputs("\r\n", g_log);
        fflush(g_log);              /* write it out NOW, in case we crash right after */
    }
}

void p360_log_close(void)
{
    if (g_log) {
        fclose(g_log);
        g_log = NULL;
    }
}

/* controller */
static WORD g_last_buttons[4];

WORD p360_pad_pressed(int pad)
{
    XINPUT_STATE state;
    WORD pressed;
     
    if (pad < 0 || pad > 3)
        return  0;
    if (XInputGetState(pad, &state) != ERROR_SUCCESS) {
        g_last_buttons[pad] = 0; /*no pad plugged in: nothing pressed*/
        return 0;
    }
    pressed = state.Gamepad.wButtons & ~g_last_buttons[pad];
    g_last_buttons[pad] = state.Gamepad.wButtons;
    return pressed;
}

/*time*/
DWORD p360_ticks(void)
{
    return GetTickCount();
}