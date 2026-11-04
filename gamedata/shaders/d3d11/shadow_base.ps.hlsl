#include "common.hlsli"

float4 main(p_shadow I) : SV_Target
{
#ifdef USE_AREF
    float4 C = s_base.Sample(smp_linear, I.tc0);
    float aref_alpha = C.w;
    #ifdef USE_MIP_FADE
        // Mip fade: match the main pass foliage dissolve (see deffer_base.ps).
        // x=start mip (fade begins), y=end mip (fully gone), end <= start = off.
        float2 bsz;
        s_base.GetDimensions(bsz.x, bsz.y);
        float2 dxc = ddx(I.tc0) * bsz;
        float2 dyc = ddy(I.tc0) * bsz;
        float mip = 0.5f * log2(max(dot(dxc, dxc), dot(dyc, dyc)));
        aref_alpha *= (tree_mip_fade.y > tree_mip_fade.x)
            ? saturate((tree_mip_fade.y - mip) / (tree_mip_fade.y - tree_mip_fade.x))
            : 1.0f;
    #endif
    clip(aref_alpha - def_aref);
#endif

    return 0;
}

