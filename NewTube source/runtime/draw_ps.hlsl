// draw_ps.hlsl - PIXEL shader for p360_draw.c. One program for everything; each corner says which job it is:
//   tex.w = 0  plain colour (boxes, lines)
//   tex.w = 1  picture (thumbnail, logo) tinted by the colour
//   tex.w = 2  text: the letter sheet holds a DISTANCE to the letter's edge (0.5 = edge). smoothstep turns it into
//              a sharp but smooth edge about one screen pixel wide (tex.z = half that width, worked out per size)

sampler tex0 : register(s0);

float4 main(float4 tex : TEXCOORD0, float4 col : COLOR0) : COLOR
{
    if (tex.w < 0.5)
        return col;
    float4 s = tex2D(tex0, tex.xy);
    if (tex.w < 1.5)
        return s * col;
    float a = smoothstep(0.5 - tex.z, 0.5 + tex.z, s.r);
    return float4(col.rgb, col.a * a);
}
