// M3b: 全屏拷贝 pass（最小依赖，不引入 common.hlsli）
// 采样 t0 纹理输出到 backbuffer，验证 SRV 描述符表 + 采样器 + 资源屏障

Texture2D tex : register(t0);
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
	return tex.Sample(smp, i.uv);
}
