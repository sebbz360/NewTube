// video_ps.hlsl - PIXEL shader for p360_video.c: turns a video picture's three planes into screen colours, on the GPU.
//
// Video decoders give YUV 4:2:0: Y = brightness for every pixel, U and V = colour for every 2x2 block. Each plane is
// a one-byte-per-pixel texture (D3DFMT_LIN_L8). The GPU runs this once per screen pixel: it looks up Y, U, V at that
// spot (the colour planes are half size, so their texture is simply stretched - the GPU blends between neighbours),
// then applies the standard TV formula (BT.601, "video range": black = 16, white = 235).

sampler planeY : register(s0);
sampler planeU : register(s1);
sampler planeV : register(s2);

float4 main(float2 uv : TEXCOORD0) : COLOR
{
    float y = (tex2D(planeY, uv).r - 16.0 / 255.0) * 1.164;
    float u = tex2D(planeU, uv).r - 0.5;
    float v = tex2D(planeV, uv).r - 0.5;
    return float4(saturate(y + 1.596 * v),
                  saturate(y - 0.391 * u - 0.813 * v),
                  saturate(y + 2.018 * u),
                  1.0);
}
