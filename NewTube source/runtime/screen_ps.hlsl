// screen_ps.hlsl - PIXEL shader for the pixel screen (runs on the GPU, once per TV pixel).
// It looks up the colour of our picture at that spot and outputs it.

sampler picture : register(s0);

float4 main(float2 uv : TEXCOORD0) : COLOR
{
    return tex2D(picture, uv);
}
