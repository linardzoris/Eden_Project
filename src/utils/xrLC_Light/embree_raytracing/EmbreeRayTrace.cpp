#include "stdafx.h"
 
#include "EmbreeRayTrace.h"
#include "../../xrCore/Collision/xrCDB.h"

#include "xrLC_GlobalData.h"
#include "xrFace.h"
#include "xrDeflector.h"
#include "light_point.h"
#include "R_light.h"

#include <../xrForms/CompilersUI.h>
extern CompilersMode gCompilerMode;

#include "../gpu_baker/GpuBaker.h"
#include "../gpu_baker/OptiXBakerHost.h"

// Важные параметры
// INTIALIZE GEOMETRY, SCENE QUALITY TYPE
// Инициализация Основных Фишек Embree

// INTEL DATA STRUCTURE
RTCSceneFlags scene_flags = RTC_SCENE_FLAG_NONE;
// Build quality HIGH: scene commits in ~20ms while the compiler issues hundreds
// of millions of shadow rays against it, so traversal speed wins over build speed.
RTCBuildQuality scene_quality = RTC_BUILD_QUALITY_HIGH;

RTCDevice device	= 0;
RTCScene IntelScene = 0;
 
RTCGeometry IntelGeometryNormal = 0;
RTCGeometry IntelGeometryMuModels = 0;

RTCGeometry IntelGeometryTransp = 0;
RTCGeometry IntelGeometryMuModelsTransp = 0;

EmbreeData EmbreeMain;
 
// Сильно ускоряет Но не нужно сильно завышать вообще 0.01f желаетельно 
// Влияет на яркость на выходе (если близко к 0 будет занулятся)
// можно и 0.10f Было раньше так
float EmbreeEnergyMAX = 0.01f;

// Shadow ray start offset (tnear) in meters, on top of LightPoint's 1cm normal
// offset. 0.1 tunnelled straight through walls/ceilings thinner than ~11cm
// (thin-wall light leaks); 0.01 keeps 2cm total clearance for float precision,
// which matches the legacy OPCODE path that started rays right at the origin.
#define EMBREE_TNEAR 0.01f

struct RayQueryContext
{
	RTCRayQueryContext context;		// must stay first (filters cast the pointer back)

	Face* skip = 0;
	// Per-lane running transmission energy: index 0 for scalar (N==1) queries,
	// one slot per lane for packet queries. skip is uniform for the whole query.
	float energy[16] = { 0 };
};

// Fetch a single lane's hit data; scalar (N==1) queries are AoS RTCHit, packets are SoA RTCHit8
static __forceinline void GetLaneHit(const RTCFilterFunctionNArguments* args, u32 lane, unsigned& geomID, unsigned& primID, float& u, float& v)
{
	if (args->N == 1)
	{
		RTCHit* h = (RTCHit*)args->hit;
		geomID = h->geomID;
		primID = h->primID;
		u = h->u;
		v = h->v;
	}
	else
	{
		RTCHit8* h = (RTCHit8*)args->hit;
		geomID = h->geomID[lane];
		primID = h->primID[lane];
		u = h->u[lane];
		v = h->v[lane];
	}
}

bool CalculateEnergy(Face* F, float& energy, float u, float v)
{
	// Перемещаем начало луча немного дальше пересечения
	b_material& M = inlc_global_data()->materials()[F->dwMaterial];
	b_texture& T = inlc_global_data()->textures()[M.surfidx];

	// calc UV
	Fvector2* cuv = F->getTC0();
	Fvector2	uv;
	uv.x = cuv[0].x * (1.0f - u - v) + cuv[1].x * u + cuv[2].x * v;
	uv.y = cuv[0].y * (1.0f - u - v) + cuv[1].y * u + cuv[2].y * v;
	int U = iFloor(uv.x * float(T.dwWidth) + .5f);
	int V = iFloor(uv.y * float(T.dwHeight) + .5f);
	U %= T.dwWidth;		if (U < 0) U += T.dwWidth;
	V %= T.dwHeight;	if (V < 0) V += T.dwHeight;

	u32* raw = static_cast<u32*>(T.pSurface);
	u32 pixel = raw[V * T.dwWidth + U];
	u32 pixel_a = color_get_A(pixel);

	float opac = 1.f - _sqr(float(pixel_a) / 255.f);

	// Дополнение Контекста
	energy *= opac;
	if (energy < EmbreeEnergyMAX)
		return false;

	return true;
}

void FilterRayTraceOpaque(const struct RTCFilterFunctionNArguments* args)
{
	RayQueryContext* ctxt = (RayQueryContext*)args->context;

	for (u32 lane = 0; lane < args->N; lane++)
	{
		if (args->valid[lane] == 0)
			continue;

		unsigned geomID, primID; float u, v;
		GetLaneHit(args, lane, geomID, primID, u, v);

		Face* F = geomID == 0 ? EmbreeMain.static_geom.dummy[primID] : geomID == 1 ? EmbreeMain.murefs_geom.dummy[primID] : nullptr;
		if (F == ctxt->skip)
		{
			args->valid[lane] = 0; continue;
		}
		ctxt->energy[lane] = 0;
		args->valid[lane] = -1; // Приехали
	}
}

void FilterRaytraceTransparent(const struct RTCFilterFunctionNArguments* args)
{
	RayQueryContext* ctxt = (RayQueryContext*)args->context;

	for (u32 lane = 0; lane < args->N; lane++)
	{
		if (args->valid[lane] == 0)
			continue;

		unsigned geomID, primID; float u, v;
		GetLaneHit(args, lane, geomID, primID, u, v);

		// Собрать все
		Face* F = nullptr;

		if (geomID == 2)
			F = EmbreeMain.static_geom_transp.dummy[primID];
		else
			F = EmbreeMain.murefs_geom_transp.dummy[primID];

		if (F != ctxt->skip && !CalculateEnergy(F, ctxt->energy[lane], u, v))
		{
			ctxt->energy[lane] = 0;
			continue;	// energy dead - lane stays accepted (occluded)
		}

		args->valid[lane] = 0;	// continue traversal
	}
}

float EmbreeData::RaytraceEmbreeProcess(R_Light& L, Fvector& P, Fvector& N, float range, void* skip)
{
	// Структура для RayTracing
	RayQueryContext data_hits;
	data_hits.skip = (Face*)skip;
	data_hits.energy[0] = 1.0f;

	RTCRay ray;
	SetRay1(ray, P, N, EMBREE_TNEAR, range);

	RTCOccludedArguments args;
	rtcInitOccludedArguments(&args);

	RTCRayQueryContext context;
	rtcInitRayQueryContext(&context);

	// SET CONTEXT
	data_hits.context = context;
	args.context = &data_hits.context;
	rtcOccluded1(IntelScene, &ray, &args);

	return data_hits.energy[0];
}

void EmbreeData::RaytraceEmbreeProcess8(Fvector& P, Fvector* dirs, float* ranges, u32 count, void* skip, float* out_energy)
{
	RayQueryContext data_hits;
	data_hits.skip = (Face*)skip;

	// Embree loads the packet valid mask and ray data with aligned AVX moves:
	// both must be 32-byte aligned or rtcOccluded8 throws "mask not aligned"
	alignas(32) RTCRay8 ray8;
	alignas(32) int valid[8];

	for (u32 i = 0; i < 8; i++)
	{
		if (i < count)
		{
			valid[i] = -1;
			data_hits.energy[i] = 1.0f;
			SetRay1(ray8, i, P, dirs[i], EMBREE_TNEAR, ranges[i]);
		}
		else
		{
			valid[i] = 0;
			data_hits.energy[i] = 0.0f;
			SetRay1(ray8, i, P, dirs[0], 0.0f, -1.0f);	// parked lane, never traversed
		}
	}

	RTCOccludedArguments args;
	rtcInitOccludedArguments(&args);

	RTCRayQueryContext context;
	rtcInitRayQueryContext(&context);

	// SET CONTEXT
	data_hits.context = context;
	args.context = &data_hits.context;
	rtcOccluded8(valid, IntelScene, &ray8, &args);

	for (u32 i = 0; i < count; i++)
		out_energy[i] = data_hits.energy[i];
}
 
// LOADING GEOMETRY
 
size_t GetMemory()
{
	size_t used, free, reserved;
	vminfo(&free, &reserved, &used);
	return used;
}

 
#include <xrMU_Model.h>
#include <xrMU_Model_Reference.h>

void LoadGeomBuffer(RTCGeometry& geom, RTCBuildQuality& quality, bool FilterTransp, TriangleContainer& geom_buffer)
{
 	geom = rtcNewGeometry(device, RTC_GEOMETRY_TYPE_TRIANGLE);
	rtcSetGeometryBuildQuality(geom, quality);

	if (FilterTransp)
		rtcSetGeometryOccludedFilterFunction(geom, &FilterRaytraceTransparent);
	else 
		rtcSetGeometryOccludedFilterFunction(geom, &FilterRayTraceOpaque);

	rtcSetSharedGeometryBuffer(geom, RTC_BUFFER_TYPE_VERTEX, 0, RTC_FORMAT_FLOAT3, geom_buffer.vertex().data(), 0, sizeof(VertexEmbree), geom_buffer.vertex().size());
	rtcSetSharedGeometryBuffer(geom, RTC_BUFFER_TYPE_INDEX, 0, RTC_FORMAT_UINT3, geom_buffer.faces().data(), 0, sizeof(TriEmbree), geom_buffer.faces().size());
	geom_buffer.hashTable.clear();

	rtcCommitGeometry(geom);
}

void EmbreeData::InitializeGeometry(size_t& geom_static_mem, size_t& geom_murefs_mem)
{
 	// Конструктор модели
 	EmbreeData::GetGlobalData(geom_static_mem, geom_murefs_mem);
	CTimer t; t.Start();
   	LoadGeomBuffer(IntelGeometryNormal, scene_quality, false, static_geom);
	LoadGeomBuffer(IntelGeometryMuModels, scene_quality, false, murefs_geom);
  	LoadGeomBuffer(IntelGeometryTransp, scene_quality, true, static_geom_transp);
	LoadGeomBuffer(IntelGeometryMuModelsTransp, scene_quality, true, murefs_geom_transp);
	Status("[Embree] Loading Buffers: [%u ms]", t.GetElapsed_ms());

//	Msg("Static MODELS Transp : %u, Opacue: %u", static_geom_transp.faces_v.size(), static_geom.faces_v.size());
//	Msg("MU MODELS Transp : %u, Opacue: %u", murefs_geom_transp.faces_v.size(), murefs_geom.faces_v.size());
}

size_t EmbreeData::AttachGeometrys(bool addMU)
{
	RemoveGeometry(false);
	CTimer t;
	t.Start();

	IntelScene = rtcNewScene(device);
	rtcSetSceneFlags(IntelScene, scene_flags);
 
 	isAttached = true;
  	rtcAttachGeometryByID(IntelScene, IntelGeometryNormal, 0);
	rtcAttachGeometryByID(IntelScene, IntelGeometryTransp, 2); 
	rtcAttachGeometryByID(IntelScene, IntelGeometryMuModels, 1);
	rtcAttachGeometryByID(IntelScene, IntelGeometryMuModelsTransp, 3);

	size_t start = GetMemory();
	rtcCommitScene(IntelScene);
	BVH_size = GetMemory() - start;
	 
	AditionalData("ST: %umb | MU: %umb | BVH: %u mb", Static_size / 1024 / 1024, MU_size / 1024 / 1024, BVH_size / 1024 / 1024);

	Status("[Embree] Attach Geoms : [%u ms]",t.GetElapsed_ms());

	return (GetMemory() - start);
}

void EmbreeData::RemoveGeometry(bool isDealloc)
{
 	if (isDealloc)
	{
 		rtcReleaseScene(IntelScene);
  		static_geom.ClearAll();
		static_geom_transp.ClearAll();
		murefs_geom.ClearAll();
		murefs_geom_transp.ClearAll();
		 
		BVH_size = 0;
		Static_size = 0;
		MU_size = 0;
	}
	else
	{
		rtcReleaseScene(IntelScene);
		BVH_size = 0;
	}
 	
	IntelScene = 0;
}

void errors_embree(void* userPtr, enum RTCError code, const char* str)
{
 	R_ASSERT2(false, str);
}

 
void EmbreeData::IntializeDevice()
{
	bool avx_test = CPU::ID.hasFeature(CPUFeature::AVX2);
	bool sse = CPU::ID.hasFeature(CPUFeature::SSE);

	const char* config = "";
	if (avx_test)
		config = "threads=16,isa=avx,verbose=0";
	else if (sse)
		config = "threads=16,isa=sse4.2,verbose=0";
	else
		config = "threads=16,isa=sse2,verbose=0";

	device = rtcNewDevice(config);

	rtcSetDeviceProperty(device, RTC_DEVICE_PROPERTY_NATIVE_RAY16_SUPPORTED, 0);
 	rtcSetDeviceErrorFunction(device, &errors_embree, NULL);

	string128 state;
	sprintf(state, "- Intilized Intel Embree %s - %s", RTC_VERSION_STRING, avx_test ? "avx" : sse ? "sse" : "default");
	Status(state);

	GetEmbreeDeviceProperty("RTC_DEVICE_PROPERTY_RAY_MASK_SUPPORTED", device, RTC_DEVICE_PROPERTY_RAY_MASK_SUPPORTED);
	GetEmbreeDeviceProperty("RTC_DEVICE_PROPERTY_BACKFACE_CULLING_ENABLED", device, RTC_DEVICE_PROPERTY_BACKFACE_CULLING_ENABLED);
	GetEmbreeDeviceProperty("RTC_DEVICE_PROPERTY_NATIVE_RAY4_SUPPORTED", device, RTC_DEVICE_PROPERTY_NATIVE_RAY4_SUPPORTED);

	GetEmbreeDeviceProperty("RTC_DEVICE_PROPERTY_NATIVE_RAY8_SUPPORTED", device, RTC_DEVICE_PROPERTY_NATIVE_RAY8_SUPPORTED);
	GetEmbreeDeviceProperty("RTC_DEVICE_PROPERTY_NATIVE_RAY16_SUPPORTED", device, RTC_DEVICE_PROPERTY_NATIVE_RAY16_SUPPORTED);
	GetEmbreeDeviceProperty("RTC_DEVICE_PROPERTY_IGNORE_INVALID_RAYS_ENABLED", device, RTC_DEVICE_PROPERTY_IGNORE_INVALID_RAYS_ENABLED);

	GetEmbreeDeviceProperty("RTC_DEVICE_PROPERTY_TASKING_SYSTEM", device, RTC_DEVICE_PROPERTY_TASKING_SYSTEM);

	// GPU baker probe: log the CUDA device (validates the GPU toolchain end-to-end)
	{
		char gpu_name[128] = { 0 };
		int sms = 0;
		if (gpu_baker_probe(gpu_name, sizeof(gpu_name), &sms))
			Status("- GPU baker probe: %s (%d SMs)", gpu_name, sms);
		else
			Status("- GPU baker probe: no CUDA device available");
	}
}

void EmbreeData::IntelEmbereLOAD()
{
	if (!isInitialized)
	{
		IntializeDevice();
		isInitialized = true;
	}

 	Msg("- Intel Embree Loading| Memory: %u mb", u32(GetMemory()/1024/1024) );
	if (gCompilerMode.EmbreeBVHCompact)
		scene_flags = scene_flags | RTC_SCENE_FLAG_COMPACT;
	if (gCompilerMode.EmbreeBVHRobust)
		scene_flags = scene_flags | RTC_SCENE_FLAG_ROBUST;
 	 
	IntelScene = rtcNewScene(device);
	rtcSetSceneFlags(IntelScene, scene_flags);

	// LOADING NORMAL GEOM
	// size_t geom_memory, refs_memory;
 	InitializeGeometry(Static_size, MU_size);
	AttachGeometrys(true);

	// GPU baker: OptiX context/pipeline once, then rebuild the global traceable
	// scene from the fresh face capture
	if (gCompilerMode.GPUBaking)
	{
		if (gpu_optix_init())
			gpu_optix_rebuild_global_scene();
	}

	//size_t BVH = AttachGeometrys(true);
 	//AditionalData("ST: %umb | MU: %umb | BVH: %u mb", geom_memory / 1024 / 1024, refs_memory / 1024 / 1024, BVH / 1024 / 1024);
}

void EmbreeData::IntelEmbereUNLOAD()
{
 	Msg("* Intel Embree Releasing Start| Memory: %u mb", u32(GetMemory() / 1024 / 1024));
	RemoveGeometry(true);
	gpu_optix_shutdown();
  	Msg("* Intel Embree Releasing End| Memory: %u mb", u32(GetMemory() / 1024 / 1024));
}
