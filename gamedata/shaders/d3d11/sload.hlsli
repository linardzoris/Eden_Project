#ifndef SLOAD_H
#define SLOAD_H
#include "common.hlsli"

static const float fParallaxStartFade = 8.0f;
static const float fParallaxStopFade = 12.0f;
Texture2D s_snow;

#ifndef PARALLAX_HEIGHT
	#ifdef USE_PBR
		#define PARALLAX_HEIGHT 0.02
	#else
		#define PARALLAX_HEIGHT 0.01
	#endif
#endif

// Fine parallax settings (r4_parallax_min_layers / r4_parallax_max_layers /
// r4_parallax_relief_steps / r4_pom_shadow_steps). x/y = march layer count at
// nadir/grazing, z = binary refinement steps, w = POM shadow march steps.
// Clamped at the read sites so the worst-case cost stays bounded.
float4 parallax_settings;

#ifndef DYNAMIC_SNOW_COLOR
#define DYNAMIC_SNOW_COLOR float3(0.75f, 0.75f, 0.75f)
#endif

void UpdateTC(inout p_bumped_new I, inout float2 texCoord, Texture2D heightMap, uint idx, out float parallaxShadow)
{
	parallaxShadow = 1.0f;

	float3x3 TBN = float3x3(I.M1, I.M2, I.M3);
	float3 viewDir = mul(transpose(TBN), -I.position.xyz);

	viewDir = normalize(viewDir);

	float2 currTexCoord = texCoord;
	float height = heightMap.Sample(smp_base, currTexCoord)[idx];
	texCoord += viewDir.xy * PARALLAX_HEIGHT * (height - 0.5f);

#ifdef ALLOW_STEEPPARALLAX
    if (I.position.z < fParallaxStopFade)
    {
		const float minLayers = max(parallax_settings.x, 1.0f);
		const float maxLayers = max(parallax_settings.y, minLayers);
		uint reliefSteps = clamp(uint(parallax_settings.z + 0.5f), 1, 8);

		float numLayers = lerp(maxLayers, minLayers, abs(viewDir.z));
		float layerDepth = rcp(numLayers);

		float2 texcoordDelta = viewDir.xy * layerDepth * PARALLAX_HEIGHT;

		float currDepthMapVal = 1.0f - height;
		float currLayerDepth = 0.5f;

#ifdef USE_PBR
		currLayerDepth = 0.0f;
#endif

		[loop] while(currLayerDepth < currDepthMapVal)
		{
			currLayerDepth += layerDepth;
			currTexCoord -= texcoordDelta;
			currDepthMapVal = 1.0f - heightMap.SampleLevel(smp_base, currTexCoord, 0.0f)[idx];
		}

		texcoordDelta *= 0.5;
		layerDepth *= 0.5;

		currTexCoord += texcoordDelta;
		currLayerDepth -= layerDepth;

		[loop]
		for(uint i = 0; i < reliefSteps; ++i)
		{
			currDepthMapVal = 1.0f - heightMap.SampleLevel(smp_base, currTexCoord, 0.0f)[idx];

			texcoordDelta *= 0.5f;
			layerDepth *= 0.5f;

			if(currDepthMapVal > currLayerDepth)
			{
				currTexCoord -= texcoordDelta;
				currLayerDepth += layerDepth;
			}
			else
			{
				currTexCoord += texcoordDelta;
				currLayerDepth -= layerDepth;
			}
		}

		// Approximate soft self-shadowing (Tatarchuk, POM): march from the
		// surface intersection toward the sun and let the heightfield block
		// the ray. Occlusion scales with how deep the ray passes below the
		// surface; the strongest occluder wins. Skipped when the sun is below
		// the surface plane (NdotL kills the direct light there anyway).
		float3 lightDirT = mul(transpose(TBN), -mul((float3x3)m_V, L_sun_dir_w.xyz));	// toward the sun, tangent space
		if (lightDirT.z > 0.05f && currLayerDepth > 0.0f)
		{
			const uint shadowSteps = clamp(uint(parallax_settings.w + 0.5f), 1, 32);
			const float shadowSoftness = 8.0f;	// full occlusion at ~12% height penetration

			float depth = min(currLayerDepth, 1.0f);
			float stepDepth = depth / shadowSteps;
			float2 uvStep = lightDirT.xy * stepDepth * PARALLAX_HEIGHT;
			float2 shadowUV = currTexCoord;

			float occlusion = 0.0f;

			[loop]
			for (uint s = 0; s < shadowSteps; ++s)
			{
				depth -= stepDepth;
				if (depth <= 0.0f)
					break;	// the ray escaped above the heightfield top

				shadowUV += uvStep;
				float surfaceDepth = 1.0f - heightMap.SampleLevel(smp_base, shadowUV, 0.0f)[idx];
				occlusion = max(occlusion, saturate((depth - surfaceDepth) * shadowSoftness));
			}

			parallaxShadow = 1.0f - occlusion;
		}

        float fParallaxFade = smoothstep(fParallaxStartFade, fParallaxStopFade, I.position.z);
		texCoord = lerp(currTexCoord, texCoord, fParallaxFade);
		parallaxShadow = lerp(parallaxShadow, 1.0f, fParallaxFade);
    }
#endif
}

void SloadNew(inout p_bumped_new I, inout IXrayMaterial M)
{
	float parallaxShadow = 1.0f;

#if defined(USE_STEEPPARALLAX) && defined(USE_HIGH_QUALITY)
    #ifdef USE_PBR
		UpdateTC(I, I.tcdh.xy, s_bump, 0, parallaxShadow);
    #else
		UpdateTC(I, I.tcdh.xy, s_bumpX, 3, parallaxShadow);
	#endif
#endif

    M.Color = s_base.Sample(smp_base, I.tcdh.xy);
	M.SnowMask = smoothstep(0.7f, 0.8f, I.snow_mask);

#ifdef USE_BUMP
    float4 Bump = s_bump.Sample(smp_base, I.tcdh.xy);
    float4 BumpX = s_bumpX.Sample(smp_base, I.tcdh.xy);

    #ifdef USE_PBR
		M.Normal.xy = Bump.wy * 2.0 - 1.0;
		M.Normal.z = sqrt(1.0f - saturate(dot(M.Normal.xy, M.Normal.xy)));

		M.Metalness = BumpX.x;
		M.Roughness = BumpX.y;

		M.SSS = BumpX.z;
		M.AO = BumpX.w;
    #else
		M.Normal = Bump.wzy + BumpX.xyz - 1.0f;

		M.Metalness = 0.0f;
		M.Roughness = Bump.x;

#ifdef USE_LEGACY_LIGHT
		M.Roughness *= M.Roughness;
#endif

		M.SSS = 0.0;
		M.AO = 1.0;
    #endif
#else
	
    M.Normal = float3(0.0f, 0.0f, 1.0f);

    M.Roughness = def_gloss;
    M.Metalness = 0.0f;

    M.SSS = 0.0f;
    M.AO = 1.0f;

    #ifdef USE_PBR
    M.Roughness = 1.0f - M.Roughness;
    #endif
#endif

#ifdef USE_TDETAIL
    float2 tcdbump = I.tcdh.xy * dt_params.xy;
    float4 Detail = s_detail.Sample(smp_base, tcdbump);
    M.Color.xyz *= Detail.xyz * 2.0f;

    #ifndef USE_PBR
		M.Roughness *= Detail.w * 2.0f;
		#ifdef USE_TDETAIL_BUMP
			float4 DetailBump = s_detailBump.Sample(smp_base, tcdbump);
			float4 DetailBumpX = s_detailBumpX.Sample(smp_base, tcdbump);
			M.Normal += DetailBump.wzy + DetailBumpX.xyz - 1.0f;
		#endif
    #else
        #ifdef USE_TDETAIL_BUMP
			float4 DetailBump = s_detailBump.Sample(smp_base, tcdbump);
			float4 DetailBumpX = s_detailBumpX.Sample(smp_base, tcdbump);

			float3 DetailNormal = DetailBump.wyy - 128.0f / 255.0f;
			DetailNormal.z = sqrt(1.0f - dot(DetailNormal.xy, DetailNormal.xy));

			M.Normal += DetailNormal;

			M.Metalness *= DetailBumpX.x * 2.0f;
			M.Roughness *= DetailBumpX.y * 2.0f;

			M.SSS *= DetailBumpX.z;
			M.AO *= DetailBumpX.w;
        #else
			M.Roughness *= Detail.w * 2.0f;
        #endif
    #endif
#endif

#ifdef USE_SNOW_TEXTURE
	float4 Snow = s_snow.Sample(smp_base, I.tcdh.xy);
    Snow.y *= smoothstep(0.2, 0.3, hemi_cube_pos_faces.y);

	M.Color.xyz = lerp(M.Color.xyz, DYNAMIC_SNOW_COLOR, Snow.y);
    M.Roughness = lerp(M.Roughness, Snow.x, Snow.y);
    M.Metalness = lerp(M.Metalness, Snow.z, Snow.y);
#endif

#ifndef USE_PBR
	#ifndef USE_TRUE_NORMAL_MAP
		M.Normal.z *= 0.5f;
	#endif
    // Aprox GSC material to PBS
    #ifndef USE_LEGACY_LIGHT
		M.Roughness = 1.0f - M.Roughness;
		M.Roughness = 0.1f + 0.9f * M.Roughness * M.Roughness;
    #endif
#else
	#ifndef USE_DX_NORMAL_MAP
		M.Normal.y *= -1.0f;
	#endif
    M.Roughness = max(0.02f, M.Roughness);
#endif

	// POM self-shadowing travels via M.ParallaxShadow (packed into the G-buffer's
	// Material.w) so ONLY the sun accumulation consumes it - ambient and dynamic
	// lights stay untouched, so indoor pixels get no fake sun shadow.
	M.ParallaxShadow = parallaxShadow;
}


#endif
