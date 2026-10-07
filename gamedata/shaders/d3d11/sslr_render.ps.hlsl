#include "common.hlsli"
#include "reflections.hlsli"
#include "metalic_roughness_ambient.hlsli"

struct PSInput
{
    float4 hpos : SV_POSITION;
    float2 texcoord : TEXCOORD0;
};

Texture3D s_blue_noise;

// TODO: Это можно упростить потом
float3 TangentToWorld(in float3 N, in float3 H)
{
    float3 UpVector = abs(N.y) < 0.999f ? float3(0.0, 1.0, 0.0) : float3(0.0, 0.0, -1.0);
    float3 T = normalize(cross(UpVector, N));
    float3 B = cross(N, T);
				 
    return normalize(T * H.x + B * H.y + N * H.z);
}

// Brian Karis, Epic Games "Real Shading in Unreal Engine 4"
float4 ImportanceSampleGGX(float3 N, float2 Xi, float Roughness)
{
    float m = Roughness * Roughness;
    float m2 = m * m;
		
    float Phi = 2 * PI * Xi.x;
				 
    float CosTheta = sqrt((1.0 - Xi.y) * rcp(1.0 + (m2 - 1.0) * Xi.y));
    float SinTheta = sqrt(abs(1.0 - CosTheta * CosTheta));
				 
    float3 H;
    H.x = SinTheta * cos(Phi);
    H.y = SinTheta * sin(Phi);
    H.z = CosTheta;
		
    float d = (CosTheta * m2 - CosTheta) * CosTheta + 1.0f;
    float D = m2 * rcp(d * d);
    float pdf = D * CosTheta;

	H = TangentToWorld(N, H);
	
    return float4(H, pdf);
}

void main(PSInput I, out float4 Point : SV_Target0, out float4 Final : SV_Target1)
{
    IXrayGbuffer O;
    // P1: half-res pass; I.hpos indexes half-res pixels, unpack full-res Gbuffer by UV
    GbufferUnpack(I.texcoord.xy, O);
	
	if(O.Depth >= 1.0f)
	{
		Point = 0.0f;
		Final = 0.0f;
		
		return;
	}
	
	float3 ReflectPoint = GbufferGetPointRealUnjitter(I.texcoord.xy, O.Depth);
	float3 ViewVec = normalize(ReflectPoint);
	
	float2 Jitter = s_blue_noise[uint3(uint2(I.hpos.xy) % 128, uint(m_taa_jitter.w) % 32)].xy;
	
	Jitter.x = Jitter.x * 0.5f + 0.5f;
	Jitter.y *= 0.7f;
	
	float3 BaseReflect = normalize(reflect(ViewVec, O.Normal.xyz));
	
	float4 H = ImportanceSampleGGX(mul(m_invV, O.Normal.xyz), Jitter, O.Roughness);
	H.xyz = mul(m_V, H.xyz);
	
	float3 Reflection = reflect(ViewVec, H.xyz);
	
	if (dot(Reflection, O.Normal) < 0.0f)
    {
        Reflection = normalize(Reflection + O.Normal);
    }
	
	float3 StartPoint = ReflectPoint * 0.996f;
	Point.xyz = StartPoint + Reflection * fog_params.z;	
	
	bool isNotHUD = O.Depth >= 0.02f;

	// P0 optimization: world pixels with near-perfectly rough material keep
	// skipping both screen-space traces - their env/hemisphere fallback is
	// visually equivalent and this avoids the dependent-depth march entirely.
	// P3: low-Fresnel pixels no longer skip. Skipping them made the near-nadir
	// region of flat reflective surfaces (an iso-incidence disc centred on the
	// camera) output only the env fallback, which stands out as a hard-edged
	// bright disc against the traced surroundings. They now use a low-precision
	// trace (content stays continuous, cost is ~1/4 of the full march).
	// HUD reflections are always traced (short, cheap schedule).
	float TraceFade = 0.0f;
	float DistFade = 1.0f;
	bool LowFresnel = false;
	if(isNotHUD)
	{
		float NdotV = max(0.0f, -dot(O.Normal, ViewVec));
		float F0 = saturate(O.F0 + O.Metalness);
		float OneMinusNV = 1.0f - NdotV;
		float OneMinusNV2 = OneMinusNV * OneMinusNV;
		float FresnelPeak = F0 + (1.0f - F0) * OneMinusNV2 * OneMinusNV2 * OneMinusNV;

		TraceFade = (O.Roughness > 0.9f) ? 0.0f : 1.0f;
		LowFresnel = FresnelPeak < 0.04f;

		// P2: distance cut-off (r4_sslr_max_dist, 0 = unlimited). World pixels past
		// the cut-off fall back to the env/hemisphere reflection - they are
		// fog-dominated there and the trace is pure cost. The last 20% of the range
		// blends into the fallback so the boundary does not pop.
		float maxDist = sslr_params.w;
		if(maxDist > 0.0f)
		{
			DistFade = saturate((maxDist - O.ViewDist) * rcp(maxDist * 0.2f));
			TraceFade = O.ViewDist < maxDist ? TraceFade : 0.0f;
		}

		StartPoint += O.Normal * 0.15f;
		
#ifdef USE_OFFSCREEN_REFLECTIONS
		// P3: low-Fresnel pixels keep their old behaviour here (no VSLR march) -
		// only the fallback direction differs, and they never ran it before.
		[branch]
		if(TraceFade > 0.0f && !LowFresnel)
		{
			float4 VSLR = FastViewReflections(StartPoint, Reflection);
			Point.xyz = lerp(Point.xyz, VSLR.xyz, VSLR.w);
		}
#endif
	} 
	else
	{
		Point.xyz = Reflection.xyz * s_env.SampleLevel(smp_linear, Point.xyz, 0.0f).w;
	}
	
	float4 SSLR = 0.0f;
	float2 PrevSpecularUV = 0.5f;

	[branch]
	if(!isNotHUD || TraceFade > 0.0f)
		SSLR = FastViewReflectionsSSR(StartPoint, Reflection, !isNotHUD, LowFresnel);

	Final = 0.0f;
	[branch]
	if(SSLR.w > 0.0f)
	{
		float4 EndProj = mul(O.Depth < 0.02f ? m_P_hud : m_P, float4(SSLR.xyz, 1.0f));
		EndProj.xy = EndProj.xy * rcp(EndProj.w) * float2(0.5f, -0.5f) + 0.5f;
		
		float2 Velocity = s_velocity.Sample(smp_rtlinear, EndProj.xy).xy * float2(0.5f, -0.5f);
		PrevSpecularUV = saturate(EndProj.xy - Velocity.xy);
		
		Final = s_image.Sample(smp_rtlinear, PrevSpecularUV.xy);
	}
	
#ifdef USE_OFFSCREEN_REFLECTIONS
	O.Hemi = saturate(O.Hemi * 3.0f);
#endif
	
	float4 Hemi = CompureSpecularIrradance(Reflection.xyz, O.Hemi, 0.0f).xyzz;
	[branch]
	if(SSLR.w > 0.0f)
		SSLR.w *= GetBorderAtten(PrevSpecularUV) * DistFade;
	
#ifdef USE_OFFSCREEN_REFLECTIONS
	// VSLR fallback: sample the cubemap at the roughness-appropriate mip
	// (matches CompureSpecularIrradance's 8*Roughness LOD), so miss pixels on
	// rough surfaces get a properly blurred environment reflection instead of
	// aliased mip-0 noise.
	float3 Color = s_env.SampleLevel(smp_linear, Point.xyz, 8.0f * O.Roughness);
	Color.xyz *= rcp(1.00001f - Color.xyz);
	Color.xyz *= vslr_params.x; // r4_vslr_intensity
#else
	float3 Color = Hemi.xyz;
#endif
	
	Final.xyz = lerp(Color.xyz, Final.xyz, SSLR.w);
	Point.xyz = lerp(Point.xyz, SSLR.xyz, SSLR.w);
	Final.xyz = PopGamma(Final.xyz);
	
	Hemi.w = max(length(Point.xyz), length(StartPoint.xyz) + length(Point.xyz - StartPoint.xyz));
	Hemi.w = saturate(Hemi.w * fog_params.w + fog_params.x);
	
	Final.xyz = lerp(Final.xyz, Hemi.xyz, Hemi.w);
	
	Point.w = rcp(max(0.000001f, H.w));
	Final.xyz *= sslr_params.z; // r4_sslr_intensity
	Final.xyz *= rcp(1.0f + Final.xyz);
	
	Final.w = isNotHUD;
}

