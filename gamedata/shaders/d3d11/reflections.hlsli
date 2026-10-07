#ifndef reflections_h_2134124_inc
#define reflections_h_2134124_inc

// Screen Space Sky Reflections off
#define SKYBLED_FADE
#define USE_BASE_HUD_REFLECTIONS

#define USE_VASYAN_CUTOFF

// #define VSLR_SLOW_BREAK
// #define SSLR_SLOW_BREAK

uniform float4 scaled_screen_res;

float get_depth_fast(float2 tc)
{
    float P = s_position.SampleLevel(smp_rtlinear, tc, 0).x;
    return depth_unpack.x * rcp(P - depth_unpack.y);
}

float3 gbuf_unpack_position(float2 uv)
{
    float depth = get_depth_fast(uv);
    uv = uv * 2.0f - 1.0f;
    return float3(uv * pos_decompression_params.xy, 1.0f) * depth;
}

float3 gbuf_unpack_position(float2 uv, float depth)
{
    uv = uv * 2.0f - 1.0f;
    return float3(uv * pos_decompression_params.xy, 1.0f) * depth;
}

float2 gbuf_unpack_uv(float3 position)
{
    position.xy *= rcp(pos_decompression_params.xy * position.z);
    return saturate(position.xy * 0.5 + 0.5);
}

#define SSLR_STEPS 30
#define MAX_FIND_STEP 3

// P0 optimization: exponential marching for the world-space SSR ray.
// Base step is calibrated against SSLR_STEPS so the 30-step budget still covers
// the whole ray: sum(i=0..N-1) scale^i = (scale^N - 1) / (scale - 1)
// SSLR_TRACE_BASE = (SSLR_TRACE_SCALE - 1) / (SSLR_TRACE_SCALE^SSLR_STEPS - 1)
#define SSLR_TRACE_SCALE 1.20f
#define SSLR_TRACE_BASE  0.0008454f

// P2: distance-graded march budgets for the world path (sslr_params.w =
// r4_sslr_max_dist). Same calibration, N = 22 / 14, so the ray still covers its
// whole screen-space length with fewer (proportionally larger) steps.
#define SSLR_STEPS_MID   22
#define SSLR_STEPS_FAR   14
#define SSLR_TRACE_BASE_MID 0.0036866f
#define SSLR_TRACE_BASE_FAR 0.0168780f

// P3: low-precision budget for low-Fresnel pixels. They used to skip the trace
// entirely, which left a hard-edged bright disc (env fallback vs. traced scene
// reflection) around the camera on flat reflective surfaces. Tracing them keeps
// the content continuous at ~1/4 of the full march cost.
#define SSLR_STEPS_LOW   8
#define SSLR_TRACE_BASE_LOW 0.0606090f

// P2: a world ray whose whole folded screen-space length is below this (~1% of
// the screen) can not produce a meaningful hit - skip it without any fetch.
#define SSLR_TRACE_MIN_LEN 0.005f

// P0: linear-depth relative thickness gate run BEFORE binary refinement.
// If one march step lands this far (in relative linear depth) past the sampled
// surface, the crossing is treated as a silhouette edge: refinement taps are
// skipped and the ray keeps marching instead of terminating on a false hit.
#define SSLR_THICKNESS 0.25f

// Cheap per-ray [0,1] random value. Replaces the per-step frac(sin()) hash,
// which cost ~4 transcendental sin()s per march iteration.
float RayJitter01(uint seed)
{
	seed ^= seed >> 13;
	seed *= 1597334677u;
	return float((seed >> 8) & 0xffffu) * rcp(65535.0f);
}

float BinaryRefinement(inout float3 EndProj, float3 Reflect)
{
	float HitDepth = 0.0f;
	
	[unroll(MAX_FIND_STEP)]
	for(int i = 0; i < MAX_FIND_STEP; ++i)
	{
		HitDepth = s_env.SampleLevel(smp_nofilter, EndProj.xyz, 0).w;
		HitDepth *= HitDepth;
		
		Reflect *= 0.5f;
		EndProj += dot(EndProj, EndProj) > HitDepth ? -Reflect : Reflect;
	}
	
	HitDepth = s_env.SampleLevel(smp_nofilter, EndProj.xyz, 0).w;
	HitDepth *= HitDepth;
	
	return HitDepth;
}

float BinaryRefinementHUD(inout float3 EndProj, float3 Reflect)
{
	float HitDepth = 0.0f;
	
	[unroll(MAX_FIND_STEP)]
	for(int i = 0; i < MAX_FIND_STEP; ++i)
	{
		HitDepth = s_position.SampleLevel(smp_nofilter, EndProj.xy, 0).x;
		
		Reflect *= 0.5f;
		EndProj += EndProj.z > HitDepth ? -Reflect : Reflect;
	}
	
	HitDepth = s_position.SampleLevel(smp_nofilter, EndProj.xy, 0).x;
	
	return HitDepth;
}

float4 FastViewReflections(float3 Point, float3 Reflect)
{
	float3 SamplePoint = Reflect;
	
	float SampleHitPointLen = 0;
	float Step = rcp(SSLR_STEPS + 1) * 0.01f;
	float L = 0.011f;
	
	float RadiusS = fog_params.z * fog_params.z;
	float DistanceS = dot(Point, Point);
	
	float DirectionS = dot(Reflect, Reflect);
	
	if(DistanceS >= RadiusS) {
		return float4(SamplePoint, 0.0f);
	}
	
	Step *= fog_params.z - length(Point); //sqrt((RadiusS - DistanceS) * rcp(DirectionS));
	
	float Fade = 0;
	float Delta = 0.0f;

	uint jitterSeed = asuint(Point.x) * 1973u ^ asuint(Point.y) * 9277u ^ asuint(Point.z) * 123u ^ uint(m_taa_jitter.w) * 26699u;
	float JitterAmt = lerp(0.8f, 1.2f, RayJitter01(jitterSeed));

	[loop]
	for(uint i = 0; i < SSLR_STEPS; ++i)
	{
		float JStep = Step * JitterAmt;
		L += JStep;
		
		Step *= 1.25f;
		
		SamplePoint.xyz = Point.xyz + Reflect * L;
		
		SampleHitPointLen = s_env.SampleLevel(smp_nofilter, SamplePoint.xyz, 0).w;
		SampleHitPointLen *= SampleHitPointLen;
		
		Delta = dot(SamplePoint, SamplePoint) - SampleHitPointLen;
		
		if (Delta > 0 /*&& Delta <= JStep * 0.8f*/)
		{
			float3 JReflect = Reflect * JStep * 0.5f;
			SamplePoint.xyz -= JReflect;
			
			SampleHitPointLen = BinaryRefinement(SamplePoint.xyz, JReflect);
			Delta = dot(SamplePoint.xyz, SamplePoint.xyz) - SampleHitPointLen;
			Fade = abs(Delta) / max(dot(SamplePoint.xyz, SamplePoint.xyz), SampleHitPointLen) < 0.1f;

#ifdef VSLR_SLOW_BREAK
			if(Fade)
#endif
			break;
		}
	}
	
	SamplePoint = normalize(SamplePoint) * sqrt(SampleHitPointLen);
	return float4(SamplePoint, Fade);
}

float4 FastViewReflectionsSSR(float3 Point, float3 Reflect, bool is_hud, bool low_fresnel)
{
	float4 StartProj, EndProj;
	float3 ReflectBase = Reflect;
	
	bool Fade = false;

	if(is_hud) {
		StartProj = mul(m_P_hud, float4(Point, 1.0f)); StartProj.xyz /= StartProj.w;
		EndProj = mul(m_P_hud, float4(Point + Reflect * Point.z, 1.0f)); EndProj.xyz /= EndProj.w;
		
		StartProj.z *= 0.02f;
		EndProj.z *= 0.02f;
	} else {
		StartProj = mul(m_P, float4(Point, 1.0f)); StartProj.xyz /= StartProj.w;
		EndProj = mul(m_P, float4(Point + Reflect * Point.z, 1.0f)); EndProj.xyz /= EndProj.w;
	}
	
	Reflect = EndProj.xyz - StartProj.xyz;
	
	StartProj.xy = StartProj.xy * float2(0.5f, -0.5f) + 0.5f;
	Reflect.xy = Reflect.xy * float2(0.5f, -0.5f);
	
	Reflect.xyz = normalize(Reflect.xyz);
	float RayLen = GetMaxDirLength(StartProj.xyz, rcp(Reflect));

	// P2: distance-graded step budget (world path only; HUD keeps its fixed short
	// schedule). sslr_params.w (r4_sslr_max_dist) scales both the cut-off and the
	// budget, so one knob controls the far-field cost.
	uint NumSteps = SSLR_STEPS;
	float TraceBase = SSLR_TRACE_BASE;
	[branch]
	if(!is_hud && sslr_params.w > 0.0f)
	{
		float ViewDist = length(Point);
		if(ViewDist >= sslr_params.w * 0.75f)
		{
			NumSteps = SSLR_STEPS_FAR;
			TraceBase = SSLR_TRACE_BASE_FAR;
		}
		else if(ViewDist >= sslr_params.w * 0.4f)
		{
			NumSteps = SSLR_STEPS_MID;
			TraceBase = SSLR_TRACE_BASE_MID;
		}
	}

	// P3: low-Fresnel pixels contribute very little (F0 ~ 0.03 downstream), so a
	// coarse schedule is enough - but they must still trace, otherwise their env
	// fallback stands out as a hard-edged disc against the traced surroundings.
	if(!is_hud && low_fresnel && NumSteps > SSLR_STEPS_LOW)
	{
		NumSteps = SSLR_STEPS_LOW;
		TraceBase = SSLR_TRACE_BASE_LOW;
	}

	// HUD keeps its original near-linear short schedule.
	// The world path uses exponential steps: denser near the pixel (where a linear
	// NDC schedule undersamples), same total ray length, binary refinement still
	// recovers the exact hit after a depth crossing.
	float Step = rcp(NumSteps + 1) * RayLen * (is_hud ? 0.2f : (TraceBase * (NumSteps + 1)));
	float StepScale = is_hud ? 1.095f : SSLR_TRACE_SCALE;
	float L = is_hud ? 0.001f : 0.0f;

	// P2: world rays that leave the screen within a couple of texels can not hit
	// anything - return the miss without entering the march at all.
	bool CanTrace = is_hud || RayLen > SSLR_TRACE_MIN_LEN;

	// One jitter value per ray, animated by the frame index. Temporal accumulation
	// in sslr_temporal converges the per-frame offsets.
	uint2 jitterPixel = uint2(StartProj.xy * scaled_screen_res.xy);
	uint jitterSeed = jitterPixel.x * 1973u ^ jitterPixel.y * 9277u ^ uint(m_taa_jitter.w) * 26699u;
	float JitterAmt = lerp(0.8f, 1.2f, RayJitter01(jitterSeed));

	[loop]
	for(uint i = 0; i < NumSteps && CanTrace; ++i)
	{
		float JStep = Step * JitterAmt;
		L += JStep;
		
		Step *= StepScale;
		
		EndProj.xyz = StartProj.xyz + Reflect * L;

		// P2: a straight ray in screen space never comes back once it left the
		// screen - stop instead of sampling clamped edge texels for the rest of
		// the (fixed) step budget.
		[branch]
		if(!is_hud && (any(EndProj.xy < 0.0f) || any(EndProj.xy > 1.0f)))
			break;
		
		float HitDepth = s_position.SampleLevel(smp_nofilter, EndProj.xy, 0).x;		
		float Delta = EndProj.z - HitDepth;
		
		if (Delta > 0 && (is_hud || HitDepth > 0.02f))
		{
			// P0: cheap linear-depth thickness pre-test (one rcp pair, no texture
			// taps). Silhouette overshoots fail the gate and fall through to the
			// next march step instead of paying 3+1 refinement taps on a miss.
			float linHit = rcp(max(1.0f - HitDepth, 0.00001f));
			float linRay = rcp(max(1.0f - EndProj.z, 0.00001f));
			bool thicknessOK = is_hud || abs(linHit - linRay) * rcp(max(linHit, linRay)) < SSLR_THICKNESS;

			[branch]
			if(thicknessOK)
			{
				float3 JReflect = Reflect * JStep * 0.5f;
				EndProj.xyz -= JReflect;
				
				HitDepth = BinaryRefinementHUD(EndProj.xyz, JReflect);
				
				float2 depthL = rcp(max(1.0f - HitDepth, 0.00001f));
				float2 depthR = rcp(max(1.0f - EndProj.z, 0.00001f));
				
				EndProj.z = HitDepth;
				
			 	Fade = is_hud || abs(depthL - depthR) * rcp(max(depthL, depthR)) < 0.01f;
				
#ifdef SSLR_SLOW_BREAK
				if(Fade)
#endif
				break;
			}
		}
	}
	
	if(is_hud) {
		Fade = Fade && EndProj.z < 0.02f;
	
#ifdef USE_BASE_HUD_REFLECTIONS
		if(!Fade && ReflectBase.z > 0.0f) {
			EndProj = mul(m_P, float4(ReflectBase, 1.0f)); EndProj.xyz /= EndProj.w;
			EndProj.xy = EndProj.xy * float2(0.5f, -0.5f) + 0.5f;
			EndProj.xy = saturate(EndProj.xy);
			
			EndProj.z = s_position.SampleLevel(smp_nofilter, EndProj.xy, 0).x;
			Fade = GetBorderAtten(EndProj.xy, 0.001f) > 0.0f;
			Fade = Fade && EndProj.z > 0.02f && EndProj.z < 1.0f;
			
			EndProj.z *= 0.0002f;
		}
#endif
	} else {
		Fade = Fade && EndProj.z < 1.0f && EndProj.z > 0.02f;
	}
	
	float3 ReflPoint = GbufferGetPointRealUnjitter(EndProj.xy, EndProj.z);
	return float4(ReflPoint, Fade);
}

float4 ScreenSpaceLocalReflections(float3 Point, float3 Reflect)
{
    float2 ReflUV = 0.0;
    float3 HitPos, TestPos;
    float L = 0.025f, DeltaL = 0.0f;

    float Fade = saturate(dot(Reflect, normalize(Point)) * 4.0f);

    if (Fade < 0.001f)
    {
        return 0.0f;
    }

    [unroll(15)]
    for (int i = 0; i < 15; i++)
    {
        TestPos = Point + Reflect * L;
        ReflUV = gbuf_unpack_uv(TestPos);
        HitPos = gbuf_unpack_position(ReflUV);
        if (all(min(min(1.f - ReflUV.x, ReflUV.x), min(1.f - ReflUV.y, ReflUV.y))))
        {
            L = length(Point - HitPos);
        }
        else
        {
            return 0.0f;
        }
    }

    DeltaL = length(HitPos) - length(Point);
    Fade *= step(-0.4f, DeltaL);

    float Attention = GetBorderAtten(ReflUV, 0.125f);
    ReflUV -= s_velocity.SampleLevel(smp_rtlinear, ReflUV, 0).xy * float2(0.5f, -0.5f);
    Fade *= min(Attention, GetBorderAtten(ReflUV, 0.125f));

#ifdef SKYBLED_FADE
    float Fog = saturate(length(HitPos) * fog_params.w + fog_params.x);
    Fade *= 1.f - Fog * Fog;
#endif

    float3 Color = s_image.SampleLevel(smp_rtlinear, ReflUV, 0).xyz;
    return float4(Color, Fade);
}

float4 calc_reflections(float2 pos2d, float zpos, float3 vreflect)
{
    float3 Point = zpos * float3(pos2d * pos_decompression_params.zw - pos_decompression_params.xy, 1.0f);
    return ScreenSpaceLocalReflections(Point, mul((float3x3)m_V, vreflect));
}
#endif

