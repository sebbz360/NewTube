/* p360_draw.c - see p360_draw.h.

   Everything is drawn as rectangles made of two triangles; each corner carries: screen position, a texture spot,
   a "mode" (0 colour, 1 picture, 2 text) and a colour (draw_vs.hlsl / draw_ps.hlsl). Each call draws at once
   (no saving up): a menu has a few hundred calls per frame, which the 360 handles easily. */
#include <xtl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "p360.h"
#include "p360_video.h"
#include "p360_draw.h"
#include "draw_vs.h"
#include "draw_ps.h"

typedef struct { float x, y; float u, v, smooth, mode; float r, g, b, a; } VTX;

static const D3DVERTEXELEMENT9 LAYOUT[] = {
    { 0, 0,  D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
    { 0, 8,  D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
    { 0, 24, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0 },
    D3DDECL_END()
};

struct p360_image { D3DTexture *tex; int w, h; };

typedef struct { unsigned int cp; unsigned short x, y, w, h; float xoff, yoff, adv; } GLYPH;
struct p360_font {
    D3DTexture *tex;
    int base, spread, sheet_w, sheet_h, count;
    float ascent, descent, line;
    GLYPH *g;
    short ascii[256];                   /* code < 256 -> glyph index (-1 = none) */
    signed char kern[96][96];           /* ASCII pairs, in 1/16 pixel at the base size */
};

static D3DDevice *g_dev;
static D3DVertexShader *g_vs;
static D3DPixelShader *g_ps;
static D3DVertexDeclaration *g_layout;
static D3DTexture *g_white;             /* 1x1 white: bound when nothing else is (plain colour mode) */

#define MAX_QUADS 512
static VTX g_v[MAX_QUADS * 6];

static unsigned int be32(const unsigned char *p);

/* safe area: positions are multiplied by g_scale and moved by (g_ox, g_oy) before drawing */
static float g_scale = 1.0f, g_ox, g_oy;
extern float p360_lb_ky, p360_lb_oy;           /* 4:3 TV letterbox (p360_video.c): y * ky + oy last */

void p360_draw_transform(float scale, float ox, float oy)
{
    g_scale = scale;
    g_ox = ox;
    g_oy = oy;
}

void p360_clip(float x, float y, float w, float h)
{
    RECT r;
    if (!g_dev) return;
    r.left = (LONG)(x * g_scale + g_ox);
    r.top = (LONG)((y * g_scale + g_oy) * p360_lb_ky + p360_lb_oy);
    r.right = (LONG)((x + w) * g_scale + g_ox + 0.999f);
    r.bottom = (LONG)(((y + h) * g_scale + g_oy) * p360_lb_ky + p360_lb_oy + 0.999f);
    IDirect3DDevice9_SetScissorRect(g_dev, &r);
    IDirect3DDevice9_SetRenderState(g_dev, D3DRS_SCISSORTESTENABLE, TRUE);
}

void p360_clip_off(void)
{
    if (g_dev) IDirect3DDevice9_SetRenderState(g_dev, D3DRS_SCISSORTESTENABLE, FALSE);
}

void p360_draw_safe(int on)
{
    if (on) p360_draw_transform(0.9f, 64.0f, 36.0f);          /* the inner 90 %: Microsoft's "title safe" area */
    else p360_draw_transform(1.0f, 0, 0);
}

int p360_draw_init(void)
{
    D3DLOCKED_RECT lr;
    g_dev = (D3DDevice *)p360_device();
    if (!g_dev) return 0;
    if (FAILED(IDirect3DDevice9_CreateVertexShader(g_dev, (const DWORD *)g_draw_vs, &g_vs)) ||
        FAILED(IDirect3DDevice9_CreatePixelShader(g_dev, (const DWORD *)g_draw_ps, &g_ps)) ||
        FAILED(IDirect3DDevice9_CreateVertexDeclaration(g_dev, LAYOUT, &g_layout)) ||
        FAILED(IDirect3DDevice9_CreateTexture(g_dev, 32, 32, 1, 0, D3DFMT_LIN_A8R8G8B8, 0, &g_white, NULL)))
        return 0;
    IDirect3DTexture9_LockRect(g_white, 0, &lr, NULL, 0);
    memset(lr.pBits, 0xFF, lr.Pitch * 32);
    IDirect3DTexture9_UnlockRect(g_white, 0);
    return 1;
}

static void unpack(unsigned int c, VTX *v)
{
    v->a = ((c >> 24) & 255) / 255.0f;
    v->r = ((c >> 16) & 255) / 255.0f;
    v->g = ((c >> 8) & 255) / 255.0f;
    v->b = (c & 255) / 255.0f;
}

/* one quad = 6 corners (two triangles). Colours: top pair and bottom pair */
static VTX *quad(VTX *v, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1,
                 float smooth, float mode, unsigned int ctop, unsigned int cbot)
{
    VTX t, b;
    x0 = x0 * g_scale + g_ox;  x1 = x1 * g_scale + g_ox;
    y0 = (y0 * g_scale + g_oy) * p360_lb_ky + p360_lb_oy;  y1 = (y1 * g_scale + g_oy) * p360_lb_ky + p360_lb_oy;
    t.smooth = b.smooth = smooth;
    t.mode = b.mode = mode;
    unpack(ctop, &t);
    unpack(cbot, &b);
    v[0] = t; v[0].x = x0; v[0].y = y0; v[0].u = u0; v[0].v = v0;
    v[1] = t; v[1].x = x1; v[1].y = y0; v[1].u = u1; v[1].v = v0;
    v[2] = b; v[2].x = x0; v[2].y = y1; v[2].u = u0; v[2].v = v1;
    v[3] = v[1];
    v[4] = b; v[4].x = x1; v[4].y = y1; v[4].u = u1; v[4].v = v1;
    v[5] = v[2];
    return v + 6;
}

static void submit(D3DTexture *tex, int quads)
{
    if (!g_dev || quads <= 0) return;
    IDirect3DDevice9_SetRenderState(g_dev, D3DRS_HALFPIXELOFFSET, TRUE);   /* pixel centres at .5: crisp 1px lines */
    IDirect3DDevice9_SetRenderState(g_dev, D3DRS_ALPHABLENDENABLE, TRUE);
    IDirect3DDevice9_SetRenderState(g_dev, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    IDirect3DDevice9_SetRenderState(g_dev, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    IDirect3DDevice9_SetRenderState(g_dev, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice9_SetVertexDeclaration(g_dev, g_layout);
    IDirect3DDevice9_SetVertexShader(g_dev, g_vs);
    IDirect3DDevice9_SetPixelShader(g_dev, g_ps);
    IDirect3DDevice9_SetTexture(g_dev, 0, (D3DBaseTexture *)(tex ? tex : g_white));
    IDirect3DDevice9_SetSamplerState(g_dev, 0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    IDirect3DDevice9_SetSamplerState(g_dev, 0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    IDirect3DDevice9_SetSamplerState(g_dev, 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    IDirect3DDevice9_SetSamplerState(g_dev, 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    IDirect3DDevice9_DrawPrimitiveUP(g_dev, D3DPT_TRIANGLELIST, quads * 2, g_v, sizeof(VTX));
    IDirect3DDevice9_SetTexture(g_dev, 0, NULL);
    IDirect3DDevice9_SetRenderState(g_dev, D3DRS_HALFPIXELOFFSET, FALSE);
}

/* ---- boxes ---- */

void p360_rect_v(float x, float y, float w, float h, unsigned int top, unsigned int bottom)
{
    quad(g_v, x, y, x + w, y + h, 0, 0, 1, 1, 0, 0, top, bottom);
    submit(NULL, 1);
}

void p360_rect(float x, float y, float w, float h, unsigned int argb) { p360_rect_v(x, y, w, h, argb, argb); }

void p360_frame_rect(float x, float y, float w, float h, float t, unsigned int c)
{
    VTX *v = g_v;
    v = quad(v, x, y, x + w, y + t, 0, 0, 1, 1, 0, 0, c, c);                    /* top */
    v = quad(v, x, y + h - t, x + w, y + h, 0, 0, 1, 1, 0, 0, c, c);            /* bottom */
    v = quad(v, x, y + t, x + t, y + h - t, 0, 0, 1, 1, 0, 0, c, c);            /* left */
    quad(v, x + w - t, y + t, x + w, y + h - t, 0, 0, 1, 1, 0, 0, c, c);        /* right */
    submit(NULL, 4);
}

/* ---- pictures ---- */

p360_image *p360_image_create(int w, int h, const unsigned int *argb)
{
    p360_image *img;
    D3DLOCKED_RECT lr;
    int y, tw = (w + 31) & ~31, th = (h + 31) & ~31;    /* 360 linear textures: sizes in steps of 32 */
    if (!g_dev || w <= 0 || h <= 0) return NULL;
    img = (p360_image *)calloc(1, sizeof(*img));
    if (!img) return NULL;
    if (FAILED(IDirect3DDevice9_CreateTexture(g_dev, tw, th, 1, 0, D3DFMT_LIN_A8R8G8B8, 0, &img->tex, NULL))) {
        free(img);
        return NULL;
    }
    img->w = w;
    img->h = h;
    IDirect3DTexture9_LockRect(img->tex, 0, &lr, NULL, 0);
    for (y = 0; y < h; y++)
        memcpy((unsigned char *)lr.pBits + y * lr.Pitch, argb + y * w, w * 4);
    IDirect3DTexture9_UnlockRect(img->tex, 0);
    return img;
}

/* a picture file made by tools\make_ui_art.py: "SBI1", u32 width, u32 height (big-endian), then 0xAARRGGBB pixels */
p360_image *p360_image_load(const char *path)
{
    FILE *fp = fopen(path, "rb");
    unsigned char hdr[12];
    unsigned int *px;
    p360_image *img = NULL;
    int w, h;
    if (!fp) { p360_log("p360_image_load: can't open a picture file"); return NULL; }
    if (fread(hdr, 1, 12, fp) == 12 && !memcmp(hdr, "SBI1", 4)) {
        w = (int)be32(hdr + 4);
        h = (int)be32(hdr + 8);
        if (w > 0 && h > 0 && w <= 4096 && h <= 4096 && (px = (unsigned int *)malloc(w * h * 4)) != NULL) {
            if (fread(px, 4, w * h, fp) == (size_t)(w * h))   /* the 360 is big-endian: the words read as written */
                img = p360_image_create(w, h, px);
            free(px);
        }
    }
    fclose(fp);
    return img;
}

void p360_image_size(p360_image *img, int *w, int *h)
{
    *w = img ? img->w : 0;
    *h = img ? img->h : 0;
}

void p360_image_draw(p360_image *img, float x, float y, float w, float h, unsigned int tint)
{
    float u1, v1;
    if (!img) return;
    u1 = (float)img->w / (float)((img->w + 31) & ~31);
    v1 = (float)img->h / (float)((img->h + 31) & ~31);
    quad(g_v, x, y, x + w, y + h, 0, 0, u1, v1, 0, 1, tint, tint);
    submit(img->tex, 1);
}

void p360_image_draw_part(p360_image *img, float sx, float sy, float sw, float sh,
                          float x, float y, float w, float h, unsigned int tint)
{
    float tw, th;
    if (!img) return;
    tw = (float)((img->w + 31) & ~31);
    th = (float)((img->h + 31) & ~31);
    quad(g_v, x, y, x + w, y + h, sx / tw, sy / th, (sx + sw) / tw, (sy + sh) / th, 0, 1, tint, tint);
    submit(img->tex, 1);
}

void p360_image_free(p360_image *img)
{
    if (!img) return;
    /* the GPU may still be drawing it this frame: wait until it is idle */
    IDirect3DDevice9_BlockUntilIdle(g_dev);
    IDirect3DTexture9_Release(img->tex);
    free(img);
}

/* ---- fonts ---- */

static unsigned int be32(const unsigned char *p) { return (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
static int be16(const unsigned char *p) { return (p[0] << 8) | p[1]; }
static int sbe16(const unsigned char *p) { return (short)((p[0] << 8) | p[1]); }

p360_font *p360_font_load(const char *path)
{
    FILE *fp = fopen(path, "rb");
    unsigned char *d, *p;
    long n;
    p360_font *f;
    D3DLOCKED_RECT lr;
    int i, y, nkern;
    if (!fp) { p360_log("p360_font_load: can't open the font file"); return NULL; }
    fseek(fp, 0, SEEK_END); n = ftell(fp); fseek(fp, 0, SEEK_SET);
    d = (unsigned char *)malloc(n);
    if (!d) { fclose(fp); return NULL; }
    fread(d, 1, n, fp);
    fclose(fp);
    if (n < 24 || memcmp(d, "SBF1", 4)) { free(d); p360_log("p360_font_load: not an SBF1 font"); return NULL; }
    f = (p360_font *)calloc(1, sizeof(*f));
    f->base = be16(d + 4);
    f->spread = be16(d + 6);
    f->ascent = sbe16(d + 8) / 16.0f;
    f->descent = sbe16(d + 10) / 16.0f;
    f->line = sbe16(d + 12) / 16.0f;
    f->sheet_w = be16(d + 14);
    f->sheet_h = be16(d + 16);
    f->count = be16(d + 18);
    nkern = be16(d + 20);
    f->g = (GLYPH *)calloc(f->count, sizeof(GLYPH));
    memset(f->ascii, 0xFF, sizeof(f->ascii));
    p = d + 22;
    for (i = 0; i < f->count; i++, p += 18) {
        GLYPH *g = &f->g[i];
        g->cp = be32(p);
        g->x = (unsigned short)be16(p + 4);  g->y = (unsigned short)be16(p + 6);
        g->w = (unsigned short)be16(p + 8);  g->h = (unsigned short)be16(p + 10);
        g->xoff = sbe16(p + 12) / 16.0f;
        g->yoff = sbe16(p + 14) / 16.0f;
        g->adv = sbe16(p + 16) / 16.0f;
        if (g->cp < 256) f->ascii[g->cp] = (short)i;
    }
    for (i = 0; i < nkern; i++, p += 6) {
        int a = be16(p), b = be16(p + 2), k = sbe16(p + 4);
        if (a >= 32 && a < 128 && b >= 32 && b < 128)
            f->kern[a - 32][b - 32] = (signed char)(k < -127 ? -127 : k > 127 ? 127 : k);
    }
    if (FAILED(IDirect3DDevice9_CreateTexture(g_dev, f->sheet_w, f->sheet_h, 1, 0, D3DFMT_LIN_L8, 0, &f->tex, NULL))) {
        free(d); free(f->g); free(f);
        p360_log("p360_font_load: texture creation failed");
        return NULL;
    }
    IDirect3DTexture9_LockRect(f->tex, 0, &lr, NULL, 0);
    for (y = 0; y < f->sheet_h; y++)
        memcpy((unsigned char *)lr.pBits + y * lr.Pitch, p + y * f->sheet_w, f->sheet_w);
    IDirect3DTexture9_UnlockRect(f->tex, 0);
    free(d);
    return f;
}

/* next character of a UTF-8 string (bad bytes come out as '?') */
static unsigned int next_cp(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    unsigned int c = *p++;
    int extra = 0;
    if (c >= 0xF0 && c < 0xF8) { c &= 0x07; extra = 3; }
    else if (c >= 0xE0) { c &= 0x0F; extra = 2; }
    else if (c >= 0xC0) { c &= 0x1F; extra = 1; }
    else if (c >= 0x80) c = '?';
    while (extra-- > 0) {
        if ((*p & 0xC0) != 0x80) { c = '?'; break; }
        c = (c << 6) | (*p++ & 0x3F);
    }
    *s = (const char *)p;
    return c;
}

/* emoji and invisible joiners: left out entirely (a "?" for each would litter titles) */
static int skip_cp(unsigned int cp)
{
    return cp >= 0x1F000 || (cp >= 0x2600 && cp <= 0x27BF) || (cp >= 0xFE00 && cp <= 0xFE0F) || cp == 0x200D ||
           cp == 0x200B || (cp >= 0x2190 && cp <= 0x21FF && cp != 0x2192) || (cp >= 0x2B00 && cp <= 0x2BFF) ||
           (cp >= 0xE000 && cp <= 0xF8FF);
}

static const GLYPH *find(p360_font *f, unsigned int cp)
{
    int lo = 0, hi = f->count - 1;
    if (skip_cp(cp)) return NULL;
    if (cp < 256) {
        int i = f->ascii[cp];
        return i >= 0 ? &f->g[i] : (f->ascii['?'] >= 0 ? &f->g[f->ascii['?']] : NULL);
    }
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (f->g[mid].cp == cp) return &f->g[mid];
        if (f->g[mid].cp < cp) lo = mid + 1; else hi = mid - 1;
    }
    return f->ascii['?'] >= 0 ? &f->g[f->ascii['?']] : NULL;     /* not in the font */
}

static float kern(p360_font *f, unsigned int a, unsigned int b)
{
    if (a >= 32 && a < 128 && b >= 32 && b < 128) return f->kern[a - 32][b - 32] / 16.0f;
    return 0;
}

/* draw (or with draw = 0 only measure) up to n bytes of text */
static float run(p360_font *f, float x, float y, float size, unsigned int c, const char *s, int n, int draw)
{
    float scale = size / f->base, baseline = y + f->ascent * scale;
    float smooth = 0.5f * ((float)f->base / (size * g_scale)) / (2.0f * f->spread);
    const char *end = s + n;
    unsigned int prev = 0;
    VTX *v = g_v;
    int quads = 0;
    if (smooth > 0.25f) smooth = 0.25f;
    while (s < end && *s) {
        unsigned int cp = next_cp(&s);
        const GLYPH *g = find(f, cp);
        if (!g) continue;
        x += kern(f, prev, cp) * scale;
        if (draw && g->w) {
            float x0 = x + g->xoff * scale, y0 = baseline + g->yoff * scale;
            v = quad(v, x0, y0, x0 + g->w * scale, y0 + g->h * scale,
                     (float)g->x / f->sheet_w, (float)g->y / f->sheet_h,
                     (float)(g->x + g->w) / f->sheet_w, (float)(g->y + g->h) / f->sheet_h, smooth, 2, c, c);
            if (++quads == MAX_QUADS) { submit(f->tex, quads); quads = 0; v = g_v; }
        }
        x += g->adv * scale;
        prev = cp;
    }
    if (draw) submit(f->tex, quads);
    return x;
}

/* LARGER TEXT (a setting, mainly for standard-definition / CRT TVs): small sizes grow towards 22, big ones stay.
   Every text function (drawing, measuring, line height) uses the same grown size, so layouts stay consistent. */
static float g_text_boost;                      /* 0 = off, 0.5 = halfway to 22 (15 -> 18.5, 13 -> 17.5) */
void p360_text_boost(float b) { g_text_boost = b; }
static float grow(float size) { return g_text_boost > 0 && size < 22 ? size + (22 - size) * g_text_boost : size; }

float p360_text(p360_font *f, float x, float y, float size, unsigned int c, const char *s)
{
    size = grow(size);
    if (!f || !s) return x;
    return run(f, x, y, size, c, s, (int)strlen(s), 1);
}

float p360_text_width(p360_font *f, float size, const char *s)
{
    size = grow(size);
    if (!f || !s) return 0;
    return run(f, 0, 0, size, 0, s, (int)strlen(s), 0);
}

float p360_text_line(p360_font *f, float size) { size = grow(size); return f ? f->line * size / f->base : size; }

static const char *ELLIPSIS = "\xE2\x80\xA6";   /* ... as one character (U+2026) */

/* draw n bytes, cut to width w with "..." at the end if needed */
static void fit_run(p360_font *f, float x, float y, float w, float size, unsigned int c, const char *s, int n,
                    int force_dots)
{
    float dots = run(f, 0, 0, size, 0, ELLIPSIS, 3, 0);
    const char *p = s, *last_ok = s;
    if (!force_dots && run(f, 0, 0, size, 0, s, n, 0) <= w) {
        run(f, x, y, size, c, s, n, 1);
        return;
    }
    while (p < s + n && *p) {                                   /* longest start that leaves room for "..." */
        const char *q = p;
        next_cp(&q);
        if (run(f, 0, 0, size, 0, s, (int)(q - s), 0) + dots > w) break;
        p = q;
        last_ok = p;
    }
    while (last_ok > s && last_ok[-1] == ' ') last_ok--;      /* no space before the dots */
    x = run(f, x, y, size, c, s, (int)(last_ok - s), 1);
    run(f, x, y, size, c, ELLIPSIS, 3, 1);
}

void p360_text_fit(p360_font *f, float x, float y, float w, float size, unsigned int c, const char *s)
{
    size = grow(size);
    if (!f || !s) return;
    fit_run(f, x, y, w, size, c, s, (int)strlen(s), 0);
}

int p360_text_wrap(p360_font *f, float x, float y, float w, float size, int max_lines, unsigned int c, const char *s)
{
    float lh;
    int lines = 0;
    if (!f || !s) return 0;
    size = grow(size);                                          /* (once: the helpers below take it as it is) */
    lh = f->line * size / f->base;
    while (*s) {
        const char *p = s, *brk = NULL;
        while (*s == ' ') s++;                                  /* lines don't start with spaces */
        p = s;
        /* find the furthest word end that still fits */
        for (;;) {
            const char *q = p;
            while (*q && *q != ' ' && *q != '\n') next_cp(&q);
            if (run(f, 0, 0, size, 0, s, (int)(q - s), 0) > w) break;
            brk = q;
            if (!*q || *q == '\n') break;
            p = q + 1;
        }
        if (!brk) {                                             /* one word wider than the line: cut it anywhere */
            const char *q = s;
            brk = s;
            while (*q && *q != '\n') {
                const char *r = q;
                next_cp(&r);
                if (run(f, 0, 0, size, 0, s, (int)(r - s), 0) > w) break;
                q = r;
                brk = q;
            }
            if (brk == s) next_cp(&brk);                        /* at least one character */
        }
        lines++;
        if (max_lines && lines == max_lines && *brk && !(*brk == '\n' && !brk[1])) {
            fit_run(f, x, y, w, size, c, s, (int)strcspn(s, "\n"), 1);  /* last line: the rest, cut with "..." */
            return lines;
        }
        run(f, x, y, size, c, s, (int)(brk - s), 1);
        y += lh;
        s = brk;
        if (*s == '\n') s++;
    }
    return lines;
}
