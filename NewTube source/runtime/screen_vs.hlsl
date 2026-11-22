// screen_vs.hlsl - VERTEX shader for the pixel screen (runs on the GPU, once per corner of the screen).
// The 360 has no "just show this picture" mode: every drawing goes through two small GPU programs.
// This one passes each corner's position and picture coordinate straight through.
// build.py compiles it with fxc.exe into screen_vs.h (an array of numbers p360_screen.c hands to the GPU).

struct VS_IN  { float2 pos : POSITION;  float2 uv : TEXCOORD0; };
struct VS_OUT { float4 pos : POSITION;  float2 uv : TEXCOORD0; };

VS_OUT main(VS_IN v)
{
    VS_OUT o;
    o.pos = float4(v.pos, 0.0, 1.0);    // x, y from -1 to 1 (= the whole TV), no depth
    o.uv = v.uv;                        // which spot of the picture this corner shows (0 to 1)
    return o;
}
