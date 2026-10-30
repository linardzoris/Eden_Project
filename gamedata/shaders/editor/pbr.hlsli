#ifndef PBR_H
#define PBR_H

// Analytic metal/roughness BRDF for the editor pipeline (ps_3_0 only).
// The runtime renderer evaluates the same response through the precomputed
// material LUT (s_material) and the prefiltered sky cubes; the editor has no
// LUT path for PBR materials, so the model is evaluated directly here.
// All directions are normalized; N, L and V share the same space.

float pbr_d_ggx(float NdotH, float Roughness)
{
    float Alpha = max(Roughness * Roughness, 1.0e-4f);
    float AlphaTwo = Alpha * Alpha;
    float Divider = NdotH * NdotH * (AlphaTwo - 1.0f) + 1.0f;
    return AlphaTwo * rcp(Divider * Divider);
}

float pbr_g_smith(float NdotL, float NdotV, float Roughness)
{
    float R = Roughness + 1.0f;
    float K = R * R * 0.125f;
    float InvK = 1.0f - K;
    return 0.25f * rcp(K + NdotL * InvK) * rcp(K + NdotV * InvK);
}

float3 pbr_f_schlick(float3 F0, float HdotV)
{
    float X = 1.0f - HdotV;
    float X5 = X * X * X * X * X;
    return F0 + (1.0f - F0) * X5;
}

// Direct light response. Radiance must already contain the light color and
// distance attenuation; L points from the surface to the light, V from the
// surface to the eye.
float3 pbr_direct(float3 Radiance, float3 L, float3 N, float3 V,
                  float3 Color, float Metalness, float Roughness, float3 F0 = 0.04f)
{
    float3 H = normalize(L + V);

    float NdotL = max(0.0f, dot(N, L));
    float NdotH = max(0.0f, dot(N, H));
    float NdotV = max(0.0f, dot(N, V));
    float HdotV = max(0.0f, dot(H, V));

    float D = pbr_d_ggx(NdotH, Roughness);
    float G = pbr_g_smith(NdotL, NdotV, Roughness);
    float3 F = pbr_f_schlick(lerp(F0, Color, Metalness), HdotV);

    // Same split as the runtime DirectLight(): the Fresnel term is applied by
    // the lerp below, so the specular lobe is only D*G.
    float3 Specular = D * G;
    float3 Diffuse = Color * (1.0f - Metalness);

    return Radiance * NdotL * lerp(Diffuse, Specular, F);
}

// Ambient (image based) response. DiffuseIrradiance is the irradiance arriving
// at the surface, SpecularIrradiance the prefiltered radiance along the
// reflection vector.
float3 pbr_ambient(float3 DiffuseIrradiance, float3 SpecularIrradiance, float NdotV,
                   float3 Color, float Metalness, float3 F0 = 0.04f)
{
    DiffuseIrradiance *= (1.0f - Metalness) * Color;

    float3 F = pbr_f_schlick(lerp(F0, Color, Metalness), NdotV);
    return lerp(DiffuseIrradiance, SpecularIrradiance, F);
}

#endif
