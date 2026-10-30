#include "common.hlsli"
#include "lmodel.hlsli"
#include "pbr.hlsli"

//////////////////////////////////////////////////////////////////////////////////////////
// This is the basic primitive used by convex, volumetric lights
// for example spot-lights, one face of the omni lights, etc.
//////////////////////////////////////////////////////////////////////////////////////////
// following options are available to configure compilation:
// USE_LMAP
// USE_LMAPXFORM
// USE_SHADOW
//////////////////////////////////////////////////////////////////////////////////////////

uniform float4x4 m_shadow;

uniform sampler s_position;
uniform sampler s_normal;

uniform float4 Ldynamic_color;
uniform float4 Ldynamic_pos;
uniform float4 Ldynamic_dir;

float4 main(float4 tc : TEXCOORD0) : COLOR
{
    float4 _P = tex2Dproj(s_position, tc);

	if(_P.z < 0.001f)
		return 0.0f;

    float4 _C = tex2Dproj(s_base, tc);
    float4 _N = tex2Dproj(s_normal, tc);

    float3 P = _P.xyz;
	float3 N = normalize(_N.xyz);

    float4 P4 = float4(P, 1.0f);
    float4 PS = mul(m_shadow, P4);

#ifdef USE_LMAP
    float4 Lightmap = tex2Dlod(s_lmap, float4(PS.xy / PS.w, 0.0f, 0.0f));
#endif

    // PBR pixels are flagged by the normal target alpha (-1 - metalness, see
    // deffer_base.ps.hlsl); everything else keeps the legacy light model.
    if(_N.w < -0.5f)
    {
        float3 L2P = P - Ldynamic_pos.xyz;
        float Att = saturate(1.0f - dot(L2P, L2P) * Ldynamic_pos.w);

        float3 Light = -normalize(L2P); // surface -> light
        float3 View = -normalize(P); // surface -> eye
        float3 Radiance = Ldynamic_color.xyz * Att;

#ifdef USE_LMAP
        Radiance *= Lightmap.xyz;
#endif

        return float4(pbr_direct(Radiance, Light, N, View, _C.xyz, -1.0f - _N.w, _C.w), 0.0f);
    }

    // ----- light-model
    float4 light = plight_local(_P.w, _P, float4(N, _N.w), Ldynamic_pos, Ldynamic_pos.w);

    // ----- shadow
#ifdef USE_LMAP
    light *= Lightmap;
#endif

	float4 color = Ldynamic_color * light;
	color.xyz = color.xyz * _C.xyz + color.w * _C.w;

    return color;
}

