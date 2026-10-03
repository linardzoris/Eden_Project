// M4b: DXR 光追立方体（lib_6_3）
// 全局根签名：t0=TLAS, u0=输出纹理, b0=相机 CB
// 命中组局部根签名：t1=IB raw, t2=VB raw（通过 shader table 局部根参数绑定）

RaytracingAccelerationStructure scene : register(t0);
RWTexture2D<float4> rayOutput : register(u0);
ByteAddressBuffer ibuf : register(t1);
ByteAddressBuffer vbuf : register(t2);

cbuffer CamCB : register(b0)
{
	float4 camPos;
	float4 camRight;
	float4 camUp;
	float4 camFwd;
	float4 camParams;	// x=tanHalfFov, y=aspect, z=time
};

struct Payload
{
	float4 color;
};

[shader("raygeneration")]
void RayGen()
{
	uint2 idx = DispatchRaysIndex().xy;
	uint2 dim = DispatchRaysDimensions().xy;

	float2 ndc = ((float2)idx + 0.5) / (float2)dim * 2.0 - 1.0;
	ndc.y = -ndc.y;

	float tanHalf = camParams.x;
	float aspect = camParams.y;
	float3 dir = normalize(
		camFwd.xyz
		+ ndc.x * tanHalf * aspect * camRight.xyz
		+ ndc.y * tanHalf * camUp.xyz
	);

	RayDesc ray;
	ray.Origin = camPos.xyz;
	ray.Direction = dir;
	ray.TMin = 0.01;
	ray.TMax = 100.0;

	Payload p;
	p.color = float4(0.0, 0.0, 0.3, 1.0);
	TraceRay(scene, RAY_FLAG_NONE, 0xFF, 0, 0, 0, ray, p);
	rayOutput[idx] = p.color;
}

[shader("miss")]
void Miss(inout Payload p)
{
	p.color = float4(0.0, 0.0, 0.3, 1.0);
}

[shader("closesthit")]
void ClosestHit(inout Payload p, BuiltInTriangleIntersectionAttributes attr)
{
	uint prim = PrimitiveIndex();
	uint3 tri = ibuf.Load3(prim * 12);

	float u = attr.barycentrics.x;
	float v = attr.barycentrics.y;
	float w = 1.0 - u - v;

	// 顶点布局：pos(3f) + nrm(3f)，24B 步长
	float3 n0 = asfloat(vbuf.Load3(tri.x * 24 + 12));
	float3 n1 = asfloat(vbuf.Load3(tri.y * 24 + 12));
	float3 n2 = asfloat(vbuf.Load3(tri.z * 24 + 12));
	float3 n = normalize(n0 * w + n1 * u + n2 * v);

	float3 L = normalize(float3(0.5, 0.8, -0.6));
	float ndl = saturate(dot(n, L));
	float3 albedo = float3(0.75, 0.55, 0.95);
	p.color = float4(albedo * (0.15 + 0.85 * ndl), 1.0);
}
