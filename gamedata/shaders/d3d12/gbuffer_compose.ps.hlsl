// M4a: G-buffer 合成 pass
// 采样 t0(albedo) + t1(normal) 两张 G-buffer，简单 Lambert 光照输出 backbuffer
// 验证多 SRV 描述符表（2 个连续槽）

Texture2D gbufAlbedo : register(t0);
Texture2D gbufNormal : register(t1);
SamplerState smp : register(s0);

struct VSOut
{
	float4 pos : SV_POSITION;
	float2 uv  : TEXCOORD0;
};

VSOut VSMain(uint vid : SV_VertexID)
{
	VSOut o;
	o.pos = float4(
		(vid == 1) ? 3.0 : -1.0,
		(vid == 2) ? 3.0 : -1.0,
		0.0, 1.0
	);
	o.uv = float2(o.pos.x * 0.5 + 0.5, 0.5 - o.pos.y * 0.5);
	return o;
}

float4 PSMain(VSOut i) : SV_TARGET
{
	float4 albedo = gbufAlbedo.Sample(smp, i.uv);
	float3 nenc = gbufNormal.Sample(smp, i.uv).xyz;

	// 背景（法线 RT 清零，无几何）：直接输出 albedo
	if (dot(nenc, nenc) < 0.01)
		return albedo;

	float3 n = normalize(nenc * 2.0 - 1.0);
	float3 L = normalize(float3(0.5, 0.8, -0.6));
	float ndl = saturate(dot(n, L));
	float3 col = albedo.rgb * (0.15 + 0.85 * ndl);
	return float4(col, 1.0);
}
