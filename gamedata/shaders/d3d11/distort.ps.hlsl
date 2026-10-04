#include "common.hlsli"

struct v2p
{
    float2 tc : TEXCOORD0; // base & distort
};

Texture2D s_distort;

// Pixel  [R5 diag] R = s_base luminance, G/B = s_distort.rg (nominal ~0.498 gray)
float4 main(v2p I) : SV_Target
{
    float3 base = s_base.Sample(smp_rtlinear, I.tc).xyz;
    float2 dist = s_distort.Sample(smp_rtlinear, I.tc).xy;
    float l = dot(base, float3(0.3, 0.59, 0.11));
    return float4(l, dist.x, dist.y, 1.0);
}
