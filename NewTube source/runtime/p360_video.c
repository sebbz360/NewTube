/* p360_video.c - see p360_video.h. (Started as 03_ffmpeg's yuvscreen.c.)

   The CPU only copies the three planes (Y, U, V) into three textures; the GPU stretches them over a rectangle
   (keeping the picture's shape) and converts the colours with video_ps.hlsl.
   Two sets of textures, used in turn, so the CPU never writes into a texture the GPU is still drawing from:
   after each frame a "fence" marks the set it used, and an upload into that set waits for the fence first. */
#include <xtl.h>
#include <string.h>
#include <stdio.h>
#include "p360.h"
#include "p360_video.h"
/* the Runtime's vertex shader (position + texture spot). Its generated header DEFINES the array g_screen_vs, and
   p360_screen.c includes it too: renamed here so a program doesn't contain the same name twice */
#define g_screen_vs g_video_vs
#include "screen_vs.h"
#undef g_screen_vs
#include "video_ps.h"
static void letterbox_check(void);

#define SETS 2

static D3DDevice *g_dev;
static D3DVertexShader *g_vs;
static D3DPixelShader *g_ps;
static D3DVertexDeclaration *g_layout;
static D3DTexture *g_tex[SETS][3];      /* [set][plane: 0 Y, 1 U, 2 V] */
static DWORD g_fence[SETS];
static int g_set, g_last = -1, g_w, g_h, g_tw, g_th;   /* g_last = set holding the newest picture */
static int g_used[SETS];                /* drawn from in the frame being built */

typedef struct { float x, y, u, v; } CORNER;
static const D3DVERTEXELEMENT9 LAYOUT[] = {
    { 0, 0, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
    { 0, 8, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
    D3DDECL_END()
};

int p360_video_init(int vsync)
{
    Direct3D *d3d = Direct3DCreate9(D3D_SDK_VERSION);
    D3DPRESENT_PARAMETERS pp;
    if (!d3d)
        return 0;
    ZeroMemory(&pp, sizeof(pp));
    pp.BackBufferWidth = 1280;
    pp.BackBufferHeight = 720;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferCount = 1;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.PresentationInterval = vsync ? D3DPRESENT_INTERVAL_ONE : D3DPRESENT_INTERVAL_IMMEDIATE;
    if (FAILED(IDirect3D9_CreateDevice(d3d, 0, D3DDEVTYPE_HAL, NULL, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &g_dev)))
        return 0;
    if (FAILED(IDirect3DDevice9_CreateVertexShader(g_dev, (const DWORD *)g_video_vs, &g_vs)) ||
        FAILED(IDirect3DDevice9_CreatePixelShader(g_dev, (const DWORD *)g_video_ps, &g_ps)) ||
        FAILED(IDirect3DDevice9_CreateVertexDeclaration(g_dev, LAYOUT, &g_layout)))
        return 0;
    letterbox_check();
    return 1;
}

void *p360_device(void) { return g_dev; }

/* 4:3 TVs (CRTs, "Standard" in the Xbox's display settings): the 1280 x 720 picture would be squeezed into 4:3, so
   everything is drawn LETTERBOXED: squashed to the middle 3/4 of the height (black bars above and below), which the
   TV's 4:3 stretch turns back into the right shape. p360_draw.c and p360_video_draw apply it to every position. */
float p360_lb_ky = 1.0f, p360_lb_oy = 0.0f;
int p360_letterboxed(void) { return p360_lb_ky != 1.0f; }
static int g_sd;                                    /* 1 = a standard-definition picture (480i / 480p) */
int p360_display_sd(void) { return g_sd; }
static void letterbox_check(void)
{
    XVIDEO_MODE vm;
    ZeroMemory(&vm, sizeof(vm));
    XGetVideoMode(&vm);
    if (!vm.fIsWideScreen) { p360_lb_ky = 0.75f; p360_lb_oy = 90.0f; }
    g_sd = !vm.fIsHiDef;
    {
        char line[128];
        sprintf(line, "display: %u x %u, %s, %s%s", vm.dwDisplayWidth, vm.dwDisplayHeight, vm.fIsHiDef ? "HD" : "SD",
                vm.fIsWideScreen ? "widescreen" : "4:3", vm.fIsWideScreen ? "" : " -> letterboxed");
        p360_log(line);
    }
}

static void free_textures(void)
{
    int s, p;
    for (s = 0; s < SETS; s++) {
        if (g_fence[s])
            IDirect3DDevice9_BlockOnFence(g_dev, g_fence[s]);
        g_fence[s] = 0;
        for (p = 0; p < 3; p++)
            if (g_tex[s][p]) { IDirect3DTexture9_Release(g_tex[s][p]); g_tex[s][p] = NULL; }
    }
    g_last = -1;
}

/* textures rounded up to multiples of 64 x 32 (so the half-size colour planes are whole multiples of 32 too,
   and one set of texture spots fits all three planes) */
static int make_textures(int w, int h)
{
    int s, p;
    free_textures();
    g_w = w; g_h = h;
    g_tw = (w + 63) & ~63;
    g_th = (h + 31) & ~31;
    for (s = 0; s < SETS; s++)
        for (p = 0; p < 3; p++) {
            int tw = p ? g_tw / 2 : g_tw, th = p ? g_th / 2 : g_th;
            if (FAILED(IDirect3DDevice9_CreateTexture(g_dev, tw, th, 1, 0, D3DFMT_LIN_L8, 0, &g_tex[s][p], NULL)))
                return 0;
        }
    return 1;
}

static void copy_plane(D3DTexture *t, const unsigned char *src, int stride, int w, int h)
{
    D3DLOCKED_RECT lr;
    int y;
    IDirect3DTexture9_LockRect(t, 0, &lr, NULL, 0);
    for (y = 0; y < h; y++)
        memcpy((unsigned char *)lr.pBits + y * lr.Pitch, src + y * stride, w);
    IDirect3DTexture9_UnlockRect(t, 0);
}

void p360_frame_begin(unsigned int clear_argb)
{
    if (!g_dev) return;
    if (p360_letterboxed()) {                       /* black bars, the colour only inside the picture */
        D3DRECT r;
        r.x1 = 0; r.y1 = (LONG)p360_lb_oy; r.x2 = 1280; r.y2 = (LONG)(p360_lb_oy + 720 * p360_lb_ky);
        IDirect3DDevice9_Clear(g_dev, 0, NULL, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0);
        IDirect3DDevice9_Clear(g_dev, 1, &r, D3DCLEAR_TARGET, clear_argb, 1.0f, 0);
        return;
    }
    IDirect3DDevice9_Clear(g_dev, 0, NULL, D3DCLEAR_TARGET, clear_argb, 1.0f, 0);
}

void p360_frame_end(void)
{
    DWORD fence;
    int s;
    if (!g_dev) return;
    IDirect3DDevice9_Present(g_dev, NULL, NULL, NULL, NULL);
    fence = IDirect3DDevice9_InsertFence(g_dev);
    for (s = 0; s < SETS; s++)
        if (g_used[s]) { g_fence[s] = fence; g_used[s] = 0; }
}

int p360_video_upload(int w, int h, const unsigned char *y, const unsigned char *u, const unsigned char *v,
                      int y_stride, int uv_stride)
{
    if (!g_dev || !y)
        return 0;
    if ((w != g_w || h != g_h) && !make_textures(w, h)) {
        p360_log("p360_video: texture creation failed");
        return 0;
    }
    if (g_fence[g_set])                             /* GPU still drawing from this set? wait */
        IDirect3DDevice9_BlockOnFence(g_dev, g_fence[g_set]);
    copy_plane(g_tex[g_set][0], y, y_stride, w, h);
    copy_plane(g_tex[g_set][1], u, uv_stride, w / 2, h / 2);
    copy_plane(g_tex[g_set][2], v, uv_stride, w / 2, h / 2);
    g_last = g_set;
    g_set = (g_set + 1) % SETS;
    return 1;
}

int p360_video_draw(float rx, float ry, float rw, float rh)
{
    CORNER c[4];
    float pw, ph, x0, y0, x1, y1, u1, v1;
    int p, set = g_last;

    if (!g_dev || set < 0)
        return 0;
    /* fit the picture's shape into the rectangle (bars on the sides or top/bottom stay as they are) */
    pw = rw;
    ph = rw * (float)g_h / (float)g_w;
    if (ph > rh) { ph = rh; pw = rh * (float)g_w / (float)g_h; }
    x0 = rx + (rw - pw) * 0.5f;  y0 = ry + (rh - ph) * 0.5f;
    x1 = x0 + pw;                y1 = y0 + ph;
    y0 = y0 * p360_lb_ky + p360_lb_oy;  y1 = y1 * p360_lb_ky + p360_lb_oy;   /* (4:3 TV: letterboxed) */
    /* screen pixels -> the GPU's -1..1 (y up) */
    x0 = x0 / 640.0f - 1.0f;  x1 = x1 / 640.0f - 1.0f;
    y0 = 1.0f - y0 / 360.0f;  y1 = 1.0f - y1 / 360.0f;
    u1 = (float)g_w / (float)g_tw;                  /* the picture's part of the (bigger) texture */
    v1 = (float)g_h / (float)g_th;
    c[0].x = x0; c[0].y = y0; c[0].u = 0;  c[0].v = 0;
    c[1].x = x1; c[1].y = y0; c[1].u = u1; c[1].v = 0;
    c[2].x = x0; c[2].y = y1; c[2].u = 0;  c[2].v = v1;
    c[3].x = x1; c[3].y = y1; c[3].u = u1; c[3].v = v1;

    IDirect3DDevice9_SetRenderState(g_dev, D3DRS_ALPHABLENDENABLE, FALSE);
    IDirect3DDevice9_SetVertexDeclaration(g_dev, g_layout);
    IDirect3DDevice9_SetVertexShader(g_dev, g_vs);
    IDirect3DDevice9_SetPixelShader(g_dev, g_ps);
    for (p = 0; p < 3; p++) {
        IDirect3DDevice9_SetTexture(g_dev, p, (D3DBaseTexture *)g_tex[set][p]);
        IDirect3DDevice9_SetSamplerState(g_dev, p, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);   /* smooth scaling */
        IDirect3DDevice9_SetSamplerState(g_dev, p, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        IDirect3DDevice9_SetSamplerState(g_dev, p, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        IDirect3DDevice9_SetSamplerState(g_dev, p, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    }
    IDirect3DDevice9_DrawPrimitiveUP(g_dev, D3DPT_TRIANGLESTRIP, 2, c, sizeof(CORNER));
    for (p = 0; p < 3; p++)
        IDirect3DDevice9_SetTexture(g_dev, p, NULL);
    g_used[set] = 1;
    return 1;
}

void p360_video_show(int w, int h, const unsigned char *y, const unsigned char *u, const unsigned char *v,
                     int y_stride, int uv_stride)
{
    if (!p360_video_upload(w, h, y, u, v, y_stride, uv_stride))
        return;
    p360_frame_begin(0xFF000000);
    p360_video_draw(0, 0, 1280, 720);
    p360_frame_end();
}

void p360_video_redraw(void)
{
    p360_frame_begin(0xFF000000);
    p360_video_draw(0, 0, 1280, 720);               /* nothing shown yet: just black */
    p360_frame_end();
}
