// OptiX device programs for the GPU light baker.
// Semantics mirror the CPU path exactly:
//   - anyhit alpha-energy accumulation  == EmbreeRayTrace.cpp FilterRaytraceTransparent / CalculateEnergy
//   - opaque/skip handling              == FilterRayTraceOpaque
//   - per-light attenuation math        == xrDeflectorLight.cpp LightPoint
#include <optix_device.h>

#include "OptiXBakerShared.h"

// minimal float3 helpers (CUDA headers provide no dot()/operators for float3)
static __forceinline__ __device__ float3 vadd(const float3& a, const float3& b) { return make_float3(a.x + b.x, a.y + b.y, a.z + b.z); }
static __forceinline__ __device__ float3 vsub(const float3& a, const float3& b) { return make_float3(a.x - b.x, a.y - b.y, a.z - b.z); }
static __forceinline__ __device__ float3 vscale(const float3& a, float s) { return make_float3(a.x * s, a.y * s, a.z * s); }
static __forceinline__ __device__ float3 vneg(const float3& a) { return make_float3(-a.x, -a.y, -a.z); }
static __forceinline__ __device__ float vdot(const float3& a, const float3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

extern "C" {
	__constant__ GpuLaunchParams params;
}

static __forceinline__ __device__ float trace_occlusion(const float3& origin, const float3& dir, float range, int skipFace)
{
	// payload: [0] = transmission energy (float as uint), [1] = skip face id
	unsigned int p0 = __float_as_uint(1.0f);
	unsigned int p1 = (unsigned int)skipFace;
	optixTrace(reinterpret_cast<OptixTraversableHandle>(params.handle),
		origin, dir,
		0.1f,						// tmin, matches the Embree path
		range, 0.0f,
		OptixVisibilityMask(0xFF),
		OPTIX_RAY_FLAG_NONE,
		0, 1, 0,					// SBT offsets: ray type 0
		p0, p1);
	return __uint_as_float(p0);
}

// texture alpha-energy factor, mirrors the CPU CalculateEnergy
static __forceinline__ __device__ float texel_opacity(const GpuFaceData& F, float u, float v)
{
	const GpuTextureDesc& T = params.textures[F.texIdx];
	const float w0 = 1.0f - u - v;		// barycentric (1-u-v, u, v), same as CPU
	const float tu = F.tc[0] * w0 + F.tc[2] * u + F.tc[4] * v;
	const float tv = F.tc[1] * w0 + F.tc[3] * u + F.tc[5] * v;
	int U = (int)floorf(tu * (float)T.width + 0.5f);
	int V = (int)floorf(tv * (float)T.height + 0.5f);
	U %= T.width;	if (U < 0) U += T.width;
	V %= T.height;	if (V < 0) V += T.height;
	const unsigned int pixel = T.pixels[V * T.width + U];
	const float alpha = (float)((pixel >> 24) & 0xFF) / 255.0f;
	return 1.0f - alpha * alpha;
}

extern "C" __global__ void __anyhit__occlusion()
{
	const float energy = __uint_as_float(optixGetPayload_0());
	const int skipFace = (int)optixGetPayload_1();

	const unsigned int primID = optixGetPrimitiveIndex();
	if ((int)primID == skipFace)
	{
		optixIgnoreIntersection();
		return;
	}

	const GpuFaceData& F = params.faces[primID];
	if (F.opaque)
	{
		optixSetPayload_0(__float_as_uint(0.0f));
		optixTerminateRay();
		return;
	}

	float result = energy;
	if (F.texIdx >= 0)
	{
		const float2 bary = optixGetTriangleBarycentrics();
		result *= texel_opacity(F, bary.x, bary.y);
		optixSetPayload_0(__float_as_uint(result));
		if (result < params.energyMax)
		{
			optixTerminateRay();
			return;
		}
	}
	// ray survives the hit: fall through to keep traversal going
}

extern "C" __global__ void __closesthit__noop()
{
	// occlusion query: the payload carries the result
}

extern "C" __global__ void __miss__occlusion()
{
	// payload energy stays 1.0 - nothing to do
}

// one light contribution; mirrors LightPoint's per-light logic in xrDeflectorLight.cpp
static __forceinline__ __device__ void process_light(const GpuLight& L, const float3& P, const float3& Pnew,
	const float3& N, int skipFace, int channel, GpuBakeColor& out)
{
	float att = 0.0f;
	const float3 Lpos = make_float3(L.position[0], L.position[1], L.position[2]);
	const float3 Ldir0 = make_float3(L.direction[0], L.direction[1], L.direction[2]);

	if (L.type == GPU_LT_DIRECT)
	{
		const float3 Ldir = vneg(Ldir0);
		const float D = vdot(Ldir, N);
		if (D <= 0.0f)
			return;
		const float trace = trace_occlusion(Pnew, Ldir, 1000.0f, skipFace);
		att = (channel != 0) ? L.energy * trace : D * L.energy * trace;
	}
	else if (L.type == GPU_LT_POINT)
	{
		const float3 delta = vsub(P, Lpos);
		const float sqD = vdot(delta, delta);
		if (sqD > L.range2 || sqD < 1e-12f)
			return;
		const float3 Ldir = vscale(delta, 1.0f / sqrtf(sqD));
		const float D = vdot(Ldir, N);
		if (D <= 0.0f)
			return;
		const float R = sqrtf(sqD);
		const float trace = trace_occlusion(Pnew, Ldir, R, skipFace);
		const float scale = D * L.energy * trace;
		if (channel != 0)
			att = scale / (L.attenuation0 + L.attenuation1 * R + L.attenuation2 * sqD);
		else if (params.glLinear)
			att = scale * (1.0f - R / L.range);
		else
			att = scale * (1.0f / (L.attenuation0 + L.attenuation1 * R + L.attenuation2 * sqD) - R * L.falloff);
	}
	else // GPU_LT_SECONDARY
	{
		const float3 delta = vsub(P, Lpos);
		const float sqD = vdot(delta, delta);
		if (sqD > L.range2 || sqD < 1e-12f)
			return;
		const float3 Ldir = vscale(delta, 1.0f / sqrtf(sqD));
		float D = vdot(Ldir, N);
		if (D <= 0.0f)
			return;
		D *= -vdot(Ldir, Ldir0);
		if (D <= 0.0f)
			return;
		// the CPU jitters the light position but never uses the jittered value
		// for this trace - skip it here as well
		const float R = sqrtf(sqD);
		const float trace = trace_occlusion(Pnew, Ldir, R, skipFace);
		att = powf(D, 0.125f) * L.energy * trace * (1.0f - R / L.range);
	}

	if (channel == 0)
	{
		out.rgb[0] += att * L.diffuse[0];
		out.rgb[1] += att * L.diffuse[1];
		out.rgb[2] += att * L.diffuse[2];
	}
	else if (channel == 1)
	{
		out.sun += att;
	}
	else
	{
		out.hemi += att;
	}
}

extern "C" __global__ void __raygen__bake()
{
	const int idx = blockIdx.x * blockDim.x + threadIdx.x;
	const int stride = gridDim.x * blockDim.x;

	for (int p = idx; p < params.pointCount; p += stride)
	{
		const GpuBakePoint bp = params.points[p];
		const float3 P = make_float3(bp.P[0], bp.P[1], bp.P[2]);
		const float3 N = make_float3(bp.N[0], bp.N[1], bp.N[2]);
		const float3 Pnew = vadd(P, vscale(N, 0.01f));

		GpuBakeColor out;
		out.rgb[0] = out.rgb[1] = out.rgb[2] = 0.0f;
		out.sun = 0.0f;
		out.hemi = 0.0f;

		if ((params.flags & GPU_LP_DONT_RGB) == 0)
			for (int i = 0; i < params.lightsRgbCount; ++i)
				process_light(params.lightsRgb[i], P, Pnew, N, bp.skipFace, 0, out);

		if ((params.flags & GPU_LP_DONT_SUN) == 0)
			for (int i = 0; i < params.lightsSunCount; ++i)
				process_light(params.lightsSun[i], P, Pnew, N, bp.skipFace, 1, out);

		if ((params.flags & GPU_LP_DONT_HEMI) == 0)
			for (int i = 0; i < params.lightsHemiCount; ++i)
				process_light(params.lightsHemi[i], P, Pnew, N, bp.skipFace, 2, out);

		params.out[p] = out;
	}
}
