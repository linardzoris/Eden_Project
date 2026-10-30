#include "common.hlsli"

#if defined(USE_OFFSCREEN_REFLECTIONS) && !defined(USE_SSLR_REFLECTIONS)
#define USE_VIEW_REFLECTIONS
#endif

#include "metalic_roughness_light.hlsli"
#include "metalic_roughness_ambient.hlsli"
#include "reflections.hlsli"

Texture2D<float> s_occ;

// Debug view mode: 0 = disabled (normal combine), 1+ = output the selected
// G-buffer channel instead of the final color (picked in the GraphicDebug
// ImGui window, passed from CRenderTarget::phase_combine).
uniform float4 combine_debug_params; // x = mode

struct _input
{
    float4 tc0 : TEXCOORD0;
    float2 tcJ : TEXCOORD1;
    float4 pos2d : SV_POSITION;
};

float4 main(_input I) : SV_Target
{
    IXrayGbuffer O;
    GbufferUnpack(I.tc0.xy, I.pos2d.xy, O);
    float3 Light = s_accumulator.Load(int3(I.pos2d.xy, 0)).xyz;

#ifdef USE_R2_STATIC_SUN
    Light += O.SSS * DirectLight(Ldynamic_color, Ldynamic_dir.xyz, O.Normal, O.View.xyz, O.Color, O.Metalness, O.Roughness, O.F0) * O.ParallaxShadow;
#endif

    float Occ = O.AO * s_occ.SampleLevel(smp_rtlinear, I.tc0.xy, 0.0f).x;

#ifndef USE_LEGACY_LIGHT
	#ifdef USE_SSLR_REFLECTIONS
		float3 SpecularIrradance = s_refl.SampleLevel(smp_rtlinear, I.tc0, 0.0).xyz;
		SpecularIrradance *= rcp(1.00001f - SpecularIrradance);
	#else
		float3 SpecularIrradance = CompureSpecularIrradance(reflect(O.View, O.Normal), O.Hemi, O.Roughness);
	#endif

	float3 DiffuseIrradance = CompureDiffuseIrradance(O.Normal, O.Hemi) + L_ambient.xyz;	
    float3 Ambient = AmbientLighting(DiffuseIrradance, SpecularIrradance, max(0.0, dot(O.Normal, -O.View.xyz)), O.Color, O.Metalness, O.Roughness, O.F0);
#else
    float3 Ambient = AmbientLighting(O.View, O.Normal, O.Color, O.Metalness, O.Roughness, O.Hemi, O.F0);
#endif

    float3 Color = Occ * Ambient + Light;

    float Fog = PushGamma(saturate(O.ViewDist * fog_params.w + fog_params.x));
    Color = lerp(Color, PushGamma(fog_color.xyz), Fog);

#ifdef USE_LEGACY_LIGHT
	Fog *= Fog;
#endif

    // G-buffer debug view: replaces the final color with the selected channel.
    // Alpha is zeroed so the INVSRCALPHA/SRCALPHA blend passes the debug value
    // through unmodified.
    if (combine_debug_params.x > 0.5f)
    {
        int Mode = (int)(combine_debug_params.x + 0.5f);
        float3 Debug = 0.0f;

        switch (Mode)
        {
        case 1:  Debug = O.Hemi.xxx;             break; // hemispheric amount
        case 2:  Debug = O.Normal * 0.5f + 0.5f; break; // world-space normal
        case 3:  Debug = O.Color;                break; // albedo
        case 4:  Debug = O.Metalness.xxx;        break; // metalness
        case 5:  Debug = O.Roughness.xxx;        break; // roughness
        case 6:  Debug = O.AO.xxx;               break; // ambient occlusion
        case 7:  Debug = O.Depth.xxx;            break; // hardware depth
        case 8:  Debug = O.SSS.xxx;              break; // static sun (SSS)
        case 9:  Debug = O.View * 0.5f + 0.5f;   break; // view direction
        case 10: Debug = frac(O.Point * 0.1f);   break; // world position
        case 11: Debug = Light;                  break; // accumulator light
        case 12: Debug = Ambient;                break; // ambient term
        case 13: Debug = Occ * Ambient;          break; // occluded ambient
        case 14: Debug = O.ParallaxShadow.xxx;   break; // POM self-shadow
        case 15: Debug = O.SnowMask.xxx;         break; // snow mask
        }

        return float4(Debug, 0.0f);
    }

    return float4(Color, Fog);
}

