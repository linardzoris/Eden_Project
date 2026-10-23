#pragma once

// Shared host/device structures for the OptiX light baker.
// Plain C layout: must compile identically under MSVC (host) and nvcc (device).

// mirrors R_light.h light types
#define GPU_LT_DIRECT		0
#define GPU_LT_POINT		1
#define GPU_LT_SECONDARY	2

// mirrors light_point.h LP_* flags
#define GPU_LP_DONT_RGB		(1 << 1)
#define GPU_LP_DONT_SUN		(1 << 2)
#define GPU_LP_DONT_HEMI	(1 << 3)

struct GpuLight
{
	float diffuse[3];
	float position[3];
	float direction[3];
	float range;
	float range2;
	float falloff;
	float attenuation0;
	float attenuation1;
	float attenuation2;
	float energy;
	int type;
};

struct GpuTextureDesc
{
	const unsigned int* pixels;		// ARGB u32 texels, row-major
	int width;
	int height;
};

struct GpuFaceData
{
	float tc[6];			// base texture channel UVs, 3 x float2 (matches face getTC0())
	int texIdx;				// GpuTextureDesc index, -1 for faces that never sample
	unsigned int opaque;	// 1 = blocks rays outright
};

struct GpuBakePoint
{
	float P[3];				// world-space shading point
	float N[3];				// world-space shading normal
	int skipFace;			// global face id to ignore, -1 = none
};

struct GpuBakeColor
{
	float rgb[3];
	float sun;
	float hemi;
};

// OptiX launch parameter block (matches the device __constant__ "params");
// handle is an OptixTraversableHandle stored as u64 to keep this header CUDA-free
struct GpuLaunchParams
{
	unsigned long long handle;		// OptixTraversableHandle (single GAS)
	const GpuBakePoint* points;
	GpuBakeColor* out;
	int pointCount;
	const GpuLight* lightsRgb;		int lightsRgbCount;
	const GpuLight* lightsSun;		int lightsSunCount;
	const GpuLight* lightsHemi;		int lightsHemiCount;
	const GpuTextureDesc* textures;	int textureCount;
	const GpuFaceData* faces;
	float energyMax;
	int glLinear;
	unsigned int flags;
};
