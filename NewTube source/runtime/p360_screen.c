/* p360_screen.c - the PIXEL SCREEN: a picture in memory that the CPU draws into, pixel by pixel, and the
   GPU shows stretched over the whole TV (that's how Doom draws: it never asks the GPU to draw shapes).

   How a frame works:
     p360_screen_begin()   gives you the picture's memory: one DWORD (0xAARRGGBB) per pixel
     ... you set pixels ...
     p360_screen_end()     hands the picture to the GPU: it draws it as one big rectangle over the whole
                           TV (two triangles), with the two little shader programs from screen_vs.hlsl /
                           screen_ps.hlsl, then shows the result

   There are TWO pictures, used in turn: while the GPU is still showing one, you draw into the other.
   A "fence" is a marker in the GPU's to-do list; waiting on it = waiting until the GPU got that far. */
#include "p360.h"
#include "screen_vs.h"      /* g_screen_vs: the vertex shader, compiled by build.py with fxc.exe */
#include "screen_ps.h"      /* g_screen_ps: the pixel shader */

#define N_PICTURES 2

static D3DDevice *g_dev;
static D3DTexture *g_picture[N_PICTURES];
static DWORD g_fence[N_PICTURES];       /* GPU marker after the last use of each picture (0 = never used) */
static int g_current;                   /* which picture we draw into now */
static D3DVertexShader *g_vs;
static D3DPixelShader *g_ps;
static D3DVertexDeclaration *g_layout;
static int g_width, g_height;

/* The 4 corners of the TV: position (-1..1, y up) and which corner of the picture goes there (0..1, y down) */
typedef struct { float x, y, u, v; } CORNER;
static const CORNER CORNERS[4] = {
    { -1.0f,  1.0f,   0.0f, 0.0f },     /* top left     */
    {  1.0f,  1.0f,   1.0f, 0.0f },     /* top right    */
    { -1.0f, -1.0f,   0.0f, 1.0f },     /* bottom left  */
    {  1.0f, -1.0f,   1.0f, 1.0f },     /* bottom right */
};
/* tells the GPU how a CORNER is laid out: 2 floats position at byte 0, 2 floats picture spot at byte 8 */
static const D3DVERTEXELEMENT9 CORNER_LAYOUT[] = {
    { 0, 0, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
    { 0, 8, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
    D3DDECL_END()
};

int p360_screen_init(int width, int height)
{
    Direct3D *d3d;
    D3DPRESENT_PARAMETERS pp;
    int i;

    d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) {
        p360_log("screen: Direct3DCreate9 failed");
        return 0;
    }
    ZeroMemory(&pp, sizeof(pp));
    pp.BackBufferWidth = 1280;
    pp.BackBufferHeight = 720;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferCount = 1;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
    if (FAILED(IDirect3D9_CreateDevice(d3d, 0, D3DDEVTYPE_HAL, NULL,
                                       D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &g_dev))) {
        p360_log("screen: CreateDevice failed");
        return 0;
    }
    /* LIN_ = "linear": pixels stored row after row, so the CPU can write them directly
       (normal 360 textures are stored in a shuffled "tiled" order the GPU prefers) */
    for (i = 0; i < N_PICTURES; i++) {
        if (FAILED(IDirect3DDevice9_CreateTexture(g_dev, width, height, 1, 0, D3DFMT_LIN_A8R8G8B8, 0,
                                                  &g_picture[i], NULL))) {
            p360_log("screen: CreateTexture failed");
            return 0;
        }
    }
    if (FAILED(IDirect3DDevice9_CreateVertexShader(g_dev, (const DWORD *)g_screen_vs, &g_vs)) ||
        FAILED(IDirect3DDevice9_CreatePixelShader(g_dev, (const DWORD *)g_screen_ps, &g_ps)) ||
        FAILED(IDirect3DDevice9_CreateVertexDeclaration(g_dev, CORNER_LAYOUT, &g_layout))) {
        p360_log("screen: shader set-up failed");
        return 0;
    }
    g_width = width;
    g_height = height;
    p360_log("screen: ready");
    return 1;
}

int p360_screen_width(void)  { return g_width; }
int p360_screen_height(void) { return g_height; }

DWORD *p360_screen_begin(int *pitch)
{
    D3DLOCKED_RECT locked;

    if (!g_dev)
        return NULL;
    /* the GPU may still be showing this picture from 2 frames ago: wait until it's done with it */
    if (g_fence[g_current])
        IDirect3DDevice9_BlockOnFence(g_dev, g_fence[g_current]);
    IDirect3DTexture9_LockRect(g_picture[g_current], 0, &locked, NULL, 0);
    /* Pitch = bytes from one row to the next (can be MORE than width * 4: rows are padded) */
    *pitch = locked.Pitch / 4;
    return (DWORD *)locked.pBits;
}

void p360_screen_end(void)
{
    D3DTexture *picture;

    if (!g_dev)
        return;
    picture = g_picture[g_current];
    IDirect3DTexture9_UnlockRect(picture, 0);

    IDirect3DDevice9_SetVertexDeclaration(g_dev, g_layout);
    IDirect3DDevice9_SetVertexShader(g_dev, g_vs);
    IDirect3DDevice9_SetPixelShader(g_dev, g_ps);
    IDirect3DDevice9_SetTexture(g_dev, 0, (D3DBaseTexture *)picture);
    /* POINT = sharp, blocky pixels when stretched (LINEAR would blur them) */
    IDirect3DDevice9_SetSamplerState(g_dev, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    IDirect3DDevice9_SetSamplerState(g_dev, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    IDirect3DDevice9_SetSamplerState(g_dev, 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    IDirect3DDevice9_SetSamplerState(g_dev, 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    IDirect3DDevice9_DrawPrimitiveUP(g_dev, D3DPT_TRIANGLESTRIP, 2, CORNERS, sizeof(CORNER));
    IDirect3DDevice9_SetTexture(g_dev, 0, NULL);
    IDirect3DDevice9_Present(g_dev, NULL, NULL, NULL, NULL);

    g_fence[g_current] = IDirect3DDevice9_InsertFence(g_dev);
    g_current = (g_current + 1) % N_PICTURES;
}
