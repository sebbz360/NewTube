// draw_vs.hlsl - VERTEX shader for p360_draw.c (boxes, pictures, text). Positions arrive in SCREEN PIXELS
// (0,0 = top left, 1280 x 720); the GPU wants -1..1 with y pointing up, so this converts. The rest (texture spot,
// drawing mode, colour) is passed on to draw_ps.hlsl unchanged.

struct VS_IN  { float2 pos : POSITION;  float4 tex : TEXCOORD0;  float4 col : COLOR0; };
struct VS_OUT { float4 pos : POSITION;  float4 tex : TEXCOORD0;  float4 col : COLOR0; };

VS_OUT main(VS_IN v)
{
    VS_OUT o;
    o.pos = float4(v.pos.x / 640.0 - 1.0, 1.0 - v.pos.y / 360.0, 0.0, 1.0);
    o.tex = v.tex;
    o.col = v.col;
    return o;
}
