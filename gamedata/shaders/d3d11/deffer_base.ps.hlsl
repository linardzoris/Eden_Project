#include "common.hlsli"
#include "sload.hlsli"

void main(p_bumped_new I, out IXrayGbufferPack O)
{
    IXrayMaterial M;
    M.Depth = I.position.z;
	
#ifdef USE_CLIP_NEAR_PLANE
	clip(I.hpos_curr.z - I.hpos_curr.w * 0.02f);
#endif

    M.Sun = I.tcdh.w;
    M.Hemi = I.tcdh.z;
    M.Point = I.position.xyz;

    SloadNew(I, M);

#ifdef USE_AREF
    float aref_alpha = M.Color.w;
    #ifdef USE_MIP_FADE
        // Mip fade: dissolve alpha-tested foliage as the base map's mip level
        // rises (small/distant canopy cards). More texels drop below the ref
        // value, cutting distant overdraw; x=start mip (fade begins), y=end
        // mip (fully gone). end <= start disables the effect.
        float2 bsz;
        s_base.GetDimensions(bsz.x, bsz.y);
        float2 dxc = ddx(I.tcdh.xy) * bsz;
        float2 dyc = ddy(I.tcdh.xy) * bsz;
        float mip = 0.5f * log2(max(dot(dxc, dxc), dot(dyc, dyc)));
        aref_alpha *= (tree_mip_fade.y > tree_mip_fade.x)
            ? saturate((tree_mip_fade.y - mip) / (tree_mip_fade.y - tree_mip_fade.x))
            : 1.0f;
    #endif
    #if defined(USE_HASHED_AREF) && !defined(DETAIL_SHADOW_PASS)
		    clip(aref_alpha - hashed_alpha_test(M.Point));
    #else
		    clip(aref_alpha - def_aref);
    #endif
    #ifdef USE_DXT1_HACK
	      M.Color.xyz *= rcp(max(0.0001f, M.Color.w));
    #endif
#endif

#if defined(USE_BUMP) || defined(USE_TDETAIL_BUMP)
    M.Normal = mul(float3x3(I.M1, I.M2, I.M3), M.Normal);
#else
	M.Normal = float3(I.M1.z, I.M2.z, I.M3.z);
#endif

    M.Normal = normalize(M.Normal);

#ifdef USE_LM_HEMI
    float4 lm = s_hemi.Sample(smp_rtlinear, I.tcdh.zw);

    M.Sun = get_sun(lm);
    M.Hemi = get_hemi(lm);
#endif

#ifdef USE_LEGACY_LIGHT
    #ifndef USE_PBR
		M.Metalness = L_material.w;
    #else
		M.Color.xyz *= M.AO;
		M.AO = 1.0f;
		float Specular = M.Metalness * dot(M.Color.xyz, LUMINANCE_VECTOR);
		M.Color.xyz = lerp(M.Color.xyz, 0.04f, M.Metalness);
		M.Metalness = 0.5f - M.Roughness * M.Roughness * 0.5f;
		M.Roughness = Specular;
    #endif
#endif

#ifdef IGNORE_SNOW_MASK
	M.SnowMask = 0.0f;
#endif

#ifdef USE_AREF
	#ifdef USE_TREEWAVE
		M.SSS = 1.0f;
	#endif
	M.SnowMask = 0.0f;
#endif

    O.Velocity = I.hpos_curr.xy / I.hpos_curr.w - I.hpos_old.xy / I.hpos_old.w;
    GbufferPack(O, M);
}

