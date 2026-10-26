// OptiX light baker host implementation. See OptiXBakerHost.h for the API.
#include "stdafx.h"

#ifdef XR_GPU_BAKER_HAS_OPTIX

#include "OptiXBakerHost.h"
#include "OptiXBakerPTX.h"		// generated: embedded device PTX

#include <optix.h>
#include <optix_stubs.h>
#include <optix_function_table_definition.h>
#include <cuda_runtime.h>

#include "xrLC_GlobalData.h"
#include "xrFace.h"
#include "base_lighting.h"
#include "../embree_raytracing/EmbreeRayTrace.h"
#include "../../xrCore/Collision/xrCDB.h"

#include <mutex>
#include <unordered_map>
#include <vector>

extern float EmbreeEnergyMAX;

namespace
{
	struct SceneImpl
	{
		void* verts = nullptr;			size_t vertBytes = 0;
		void* indices = nullptr;		size_t indexBytes = 0;
		void* faces = nullptr;			size_t faceBytes = 0;
		void* gasOutput = nullptr;		size_t gasOutBytes = 0;
		OptixTraversableHandle handle = 0;
		float tmin = 0.1f;
		float energyMax = 0.01f;
	};

	bool g_ready = false;
	OptixDeviceContext g_ctx = nullptr;
	OptixModule g_module = nullptr;
	OptixPipeline g_pipeline = nullptr;
	OptixProgramGroup g_pgRaygen = nullptr;
	OptixProgramGroup g_pgMiss = nullptr;
	OptixProgramGroup g_pgHit = nullptr;
	OptixShaderBindingTable g_sbt = {};
	void* g_sbtRaygen = nullptr;
	void* g_sbtMiss = nullptr;
	void* g_sbtHit = nullptr;
	void* g_dParams = nullptr;

	void* g_dTextures = nullptr;
	std::vector<void*> g_texturePixels;
	int g_textureCount = 0;

	void* g_dLightsRgb = nullptr;	int g_lightsRgbCount = 0;
	void* g_dLightsSun = nullptr;	int g_lightsSunCount = 0;
	void* g_dLightsHemi = nullptr;	int g_lightsHemiCount = 0;
	int g_glLinear = 0;

	GpuBakeScene* g_globalScene = nullptr;
	std::unordered_map<base_Face*, int> g_faceIds;
	std::mutex g_faceIdsMutex;

	bool cuda_check(cudaError_t e, const char* what)
	{
		if (e != cudaSuccess)
		{
			clMsg("[GpuBaker] CUDA error at %s: %s", what, cudaGetErrorString(e));
			return false;
		}
		return true;
	}

	void optix_log(unsigned int level, const char* tag, const char* message, void*)
	{
		clMsg("[OptiX][%u][%s] %s", level, tag, message);
	}

	void fill_face_data(base_Face* BF, GpuFaceData& d)
	{
		d.tc[0] = d.tc[1] = d.tc[2] = d.tc[3] = d.tc[4] = d.tc[5] = 0.0f;
		d.texIdx = -1;
		d.opaque = 1;
		if (!BF)
			return;

		b_material& M = inlc_global_data()->materials()[BF->dwMaterial];
		b_texture& T = inlc_global_data()->textures()[M.surfidx];
		Fvector2* cuv = BF->getTC0();
		if (cuv)
		{
			d.tc[0] = cuv[0].x;	d.tc[1] = cuv[0].y;
			d.tc[2] = cuv[1].x;	d.tc[3] = cuv[1].y;
			d.tc[4] = cuv[2].x;	d.tc[5] = cuv[2].y;
		}
		// faces without a usable alpha texture block the ray (matches the CPU path)
		if (!BF->flags.bOpaque && T.pSurface)
		{
			d.opaque = 0;
			d.texIdx = (int)M.surfidx;
		}
	}

	bool upload_lights()
	{
		base_lighting& L = inlc_global_data()->L_static();
		auto upload = [](const xr_vector<R_Light>& src, void** dst, int* count) -> bool
		{
			*count = (int)src.size();
			if (!*count)
				return true;
			std::vector<GpuLight> tmp(*count);
			for (int i = 0; i < *count; i++)
			{
				const R_Light& s = src[i];
				GpuLight& d = tmp[i];
				d.diffuse[0] = s.diffuse.x;	d.diffuse[1] = s.diffuse.y;	d.diffuse[2] = s.diffuse.z;
				d.position[0] = s.position.x; d.position[1] = s.position.y; d.position[2] = s.position.z;
				d.direction[0] = s.direction.x; d.direction[1] = s.direction.y; d.direction[2] = s.direction.z;
				d.range = s.range;
				d.range2 = s.range2;
				d.falloff = s.falloff;
				d.attenuation0 = s.attenuation0;
				d.attenuation1 = s.attenuation1;
				d.attenuation2 = s.attenuation2;
				d.energy = s.energy;
				d.type = s.type;
			}
			if (!cuda_check(cudaMalloc(dst, sizeof(GpuLight) * *count), "lights malloc"))
				return false;
			return cuda_check(cudaMemcpy(*dst, tmp.data(), sizeof(GpuLight) * *count, cudaMemcpyHostToDevice), "lights upload");
		};
		return upload(L.rgb, &g_dLightsRgb, &g_lightsRgbCount)
			&& upload(L.sun, &g_dLightsSun, &g_lightsSunCount)
			&& upload(L.hemi, &g_dLightsHemi, &g_lightsHemiCount);
	}

	bool upload_textures()
	{
		const xr_vector<b_BuildTexture>& texs = inlc_global_data()->textures();
		g_textureCount = (int)texs.size();
		std::vector<GpuTextureDesc> descs(g_textureCount);
		g_texturePixels.resize(g_textureCount, nullptr);
		for (int i = 0; i < g_textureCount; i++)
		{
			const b_texture& T = texs[i];
			descs[i].pixels = nullptr;
			descs[i].width = (int)T.dwWidth;
			descs[i].height = (int)T.dwHeight;
			if (T.pSurface)
			{
				const size_t bytes = (size_t)T.dwWidth * T.dwHeight * sizeof(u32);
				if (!cuda_check(cudaMalloc(&g_texturePixels[i], bytes), "texture malloc"))
					return false;
				if (!cuda_check(cudaMemcpy(g_texturePixels[i], T.pSurface, bytes, cudaMemcpyHostToDevice), "texture upload"))
					return false;
				descs[i].pixels = (const unsigned int*)g_texturePixels[i];
			}
		}
		if (!cuda_check(cudaMalloc(&g_dTextures, sizeof(GpuTextureDesc) * g_textureCount), "texture desc malloc"))
			return false;
		return cuda_check(cudaMemcpy(g_dTextures, descs.data(), sizeof(GpuTextureDesc) * g_textureCount, cudaMemcpyHostToDevice), "texture desc upload");
	}
}

bool gpu_optix_init()
{
	if (g_ready)
		return true;

	if (!cuda_check(cudaFree(0), "cuda context"))
		return false;

	if (optixInit() != OPTIX_SUCCESS)
	{
		clMsg("[GpuBaker] optixInit failed (driver too old for OptiX 8?)");
		return false;
	}

	OptixDeviceContextOptions ctxOpts = {};
	ctxOpts.logCallbackFunction = &optix_log;
	ctxOpts.logCallbackLevel = 2;
	if (optixDeviceContextCreate(nullptr, &ctxOpts, &g_ctx) != OPTIX_SUCCESS)
	{
		clMsg("[GpuBaker] optixDeviceContextCreate failed");
		return false;
	}

	OptixModuleCompileOptions mco = {};
	mco.maxRegisterCount = OPTIX_COMPILE_DEFAULT_MAX_REGISTER_COUNT;
	mco.optLevel = OPTIX_COMPILE_OPTIMIZATION_DEFAULT;
	mco.debugLevel = OPTIX_COMPILE_DEBUG_LEVEL_NONE;

	OptixPipelineCompileOptions pco = {};
	pco.usesMotionBlur = false;
	pco.traversableGraphFlags = OPTIX_TRAVERSABLE_GRAPH_FLAG_ALLOW_SINGLE_GAS;
	pco.numPayloadValues = 2;
	pco.numAttributeValues = 2;
	pco.exceptionFlags = OPTIX_EXCEPTION_FLAG_NONE;
	pco.pipelineLaunchParamsVariableName = "params";

	char log[4096];
	size_t logSize = sizeof(log);
	if (optixModuleCreate(g_ctx, &mco, &pco, (const char*)OptiXBakerPTX, OptiXBakerPTX_size, log, &logSize, &g_module) != OPTIX_SUCCESS)
	{
		clMsg("[GpuBaker] module create failed: %s", logSize ? log : "<no log>");
		return false;
	}

	OptixProgramGroupOptions pgo = {};
	OptixProgramGroupDesc pgd[3] = {};
	pgd[0].kind = OPTIX_PROGRAM_GROUP_KIND_RAYGEN;
	pgd[0].raygen.module = g_module;
	pgd[0].raygen.entryFunctionName = "__raygen__bake";
	pgd[1].kind = OPTIX_PROGRAM_GROUP_KIND_MISS;
	pgd[1].miss.module = g_module;
	pgd[1].miss.entryFunctionName = "__miss__occlusion";
	pgd[2].kind = OPTIX_PROGRAM_GROUP_KIND_HITGROUP;
	pgd[2].hitgroup.moduleCH = g_module;
	pgd[2].hitgroup.entryFunctionNameCH = "__closesthit__noop";
	pgd[2].hitgroup.moduleAH = g_module;
	pgd[2].hitgroup.entryFunctionNameAH = "__anyhit__occlusion";
	logSize = sizeof(log);
	OptixProgramGroup pgs[3];
	if (optixProgramGroupCreate(g_ctx, pgd, 3, &pgo, log, &logSize, pgs) != OPTIX_SUCCESS)
	{
		clMsg("[GpuBaker] program group create failed: %s", logSize ? log : "<no log>");
		return false;
	}
	g_pgRaygen = pgs[0];
	g_pgMiss = pgs[1];
	g_pgHit = pgs[2];

	OptixPipelineLinkOptions plo = {};
	plo.maxTraceDepth = 1;
	logSize = sizeof(log);
	if (optixPipelineCreate(g_ctx, &pco, &plo, pgs, 3, log, &logSize, &g_pipeline) != OPTIX_SUCCESS)
	{
		clMsg("[GpuBaker] pipeline create failed: %s", logSize ? log : "<no log>");
		return false;
	}

	// shader binding table: single record per group, no user data
	if (!cuda_check(cudaMalloc(&g_sbtRaygen, OPTIX_SBT_RECORD_HEADER_SIZE), "sbt raygen")
		|| !cuda_check(cudaMalloc(&g_sbtMiss, OPTIX_SBT_RECORD_HEADER_SIZE), "sbt miss")
		|| !cuda_check(cudaMalloc(&g_sbtHit, OPTIX_SBT_RECORD_HEADER_SIZE), "sbt hit"))
		return false;

	unsigned char header[OPTIX_SBT_RECORD_HEADER_SIZE];
	optixSbtRecordPackHeader(g_pgRaygen, header);
	cuda_check(cudaMemcpy(g_sbtRaygen, header, OPTIX_SBT_RECORD_HEADER_SIZE, cudaMemcpyHostToDevice), "sbt raygen upload");
	optixSbtRecordPackHeader(g_pgMiss, header);
	cuda_check(cudaMemcpy(g_sbtMiss, header, OPTIX_SBT_RECORD_HEADER_SIZE, cudaMemcpyHostToDevice), "sbt miss upload");
	optixSbtRecordPackHeader(g_pgHit, header);
	cuda_check(cudaMemcpy(g_sbtHit, header, OPTIX_SBT_RECORD_HEADER_SIZE, cudaMemcpyHostToDevice), "sbt hit upload");

	g_sbt.raygenRecord = (CUdeviceptr)g_sbtRaygen;
	g_sbt.missRecordBase = (CUdeviceptr)g_sbtMiss;
	g_sbt.missRecordStrideInBytes = OPTIX_SBT_RECORD_HEADER_SIZE;
	g_sbt.missRecordCount = 1;
	g_sbt.hitgroupRecordBase = (CUdeviceptr)g_sbtHit;
	g_sbt.hitgroupRecordStrideInBytes = OPTIX_SBT_RECORD_HEADER_SIZE;
	g_sbt.hitgroupRecordCount = 1;

	if (!cuda_check(cudaMalloc(&g_dParams, sizeof(GpuLaunchParams)), "params malloc"))
		return false;

	if (!upload_textures())
		return false;
	if (!upload_lights())
		return false;
	g_glLinear = inlc_global_data()->gl_linear() ? 1 : 0;

	g_ready = true;
	Status("- GPU baker initialized (OptiX)");
	return true;
}

GpuBakeScene* gpu_optix_scene_create(const float* verts, size_t vertCountF,
	const unsigned int* indices, const GpuFaceData* faces, size_t faceCount,
	float tmin, float energyMax)
{
	if (!g_ready || !verts || !indices || !faces || !faceCount || vertCountF < 3)
		return nullptr;

	SceneImpl* s = new SceneImpl();
	s->tmin = tmin;
	s->energyMax = energyMax;
	do
	{
		s->vertBytes = vertCountF * sizeof(float);
		s->indexBytes = faceCount * 3 * sizeof(unsigned int);
		s->faceBytes = faceCount * sizeof(GpuFaceData);
		if (!cuda_check(cudaMalloc(&s->verts, s->vertBytes), "scene verts")
			|| !cuda_check(cudaMalloc(&s->indices, s->indexBytes), "scene indices")
			|| !cuda_check(cudaMalloc(&s->faces, s->faceBytes), "scene faces"))
			break;
		if (!cuda_check(cudaMemcpy(s->verts, verts, s->vertBytes, cudaMemcpyHostToDevice), "scene verts upload")
			|| !cuda_check(cudaMemcpy(s->indices, indices, s->indexBytes, cudaMemcpyHostToDevice), "scene indices upload")
			|| !cuda_check(cudaMemcpy(s->faces, faces, s->faceBytes, cudaMemcpyHostToDevice), "scene faces upload"))
			break;

		OptixBuildInput bi = {};
		bi.type = OPTIX_BUILD_INPUT_TYPE_TRIANGLES;
		bi.triangleArray.vertexFormat = OPTIX_VERTEX_FORMAT_FLOAT3;
		bi.triangleArray.numVertices = (unsigned int)(vertCountF / 3);
		bi.triangleArray.vertexBuffers = (const CUdeviceptr*)&s->verts;
		bi.triangleArray.indexFormat = OPTIX_INDICES_FORMAT_UNSIGNED_INT3;
		bi.triangleArray.numIndexTriplets = (unsigned int)faceCount;
		bi.triangleArray.indexBuffer = (CUdeviceptr)s->indices;
		const unsigned int biFlag = OPTIX_GEOMETRY_FLAG_NONE;
		bi.triangleArray.flags = &biFlag;
		bi.triangleArray.numSbtRecords = 1;

		OptixAccelBuildOptions ao = {};
		ao.buildFlags = OPTIX_BUILD_FLAG_PREFER_FAST_TRACE;
		ao.operation = OPTIX_BUILD_OPERATION_BUILD;

		OptixAccelBufferSizes sizes = {};
		if (optixAccelComputeMemoryUsage(g_ctx, &ao, &bi, 1, &sizes) != OPTIX_SUCCESS)
			break;

		void* gasTmp = nullptr;
		if (!cuda_check(cudaMalloc(&s->gasOutput, sizes.outputSizeInBytes), "gas output")
			|| !cuda_check(cudaMalloc(&gasTmp, sizes.tempSizeInBytes), "gas tmp"))
			break;
		s->gasOutBytes = sizes.outputSizeInBytes;
		if (optixAccelBuild(g_ctx, nullptr, &ao, &bi, 1,
			(CUdeviceptr)gasTmp, sizes.tempSizeInBytes,
			(CUdeviceptr)s->gasOutput, sizes.outputSizeInBytes,
			&s->handle, nullptr, 0) != OPTIX_SUCCESS)
			break;
		cudaFree(gasTmp);

		return (GpuBakeScene*)s;
	} while (false);

	clMsg("[GpuBaker] scene create failed");
	gpu_optix_scene_destroy((GpuBakeScene*)s);
	return nullptr;
}

GpuBakeScene* gpu_optix_scene_from_cdb(CDB::MODEL* mdl)
{
	if (!g_ready || !mdl)
		return nullptr;

	const CDB::TRI* tris = mdl->get_tris();
	const int ts = mdl->get_tris_count();
	const Fvector* vs = mdl->get_verts();
	const int vs_count = mdl->get_verts_count();
	if (!ts || !vs_count)
		return nullptr;

	std::vector<float> verts((size_t)vs_count * 3);
	for (int i = 0; i < vs_count; i++)
	{
		verts[(size_t)i * 3 + 0] = vs[i].x;
		verts[(size_t)i * 3 + 1] = vs[i].y;
		verts[(size_t)i * 3 + 2] = vs[i].z;
	}
	std::vector<unsigned int> indices((size_t)ts * 3);
	std::vector<GpuFaceData> fd(ts);
	for (int i = 0; i < ts; i++)
	{
		const CDB::TRI& T = tris[i];
		indices[(size_t)i * 3 + 0] = T.verts[0];
		indices[(size_t)i * 3 + 1] = T.verts[1];
		indices[(size_t)i * 3 + 2] = T.verts[2];
		fill_face_data(convert_nax(T.dummy), fd[i]);
	}
	// model-self scenes must match the CPU OPCODE path: rays start at the origin
	// (no tnear) and the transmission product runs without a practical cutoff
	return gpu_optix_scene_create(verts.data(), verts.size(), indices.data(), fd.data(), (size_t)ts, 0.0f, 1e-4f);
}

void gpu_optix_scene_destroy(GpuBakeScene* scene)
{
	if (!scene)
		return;
	SceneImpl* s = (SceneImpl*)scene;
	if (s->verts) cudaFree(s->verts);
	if (s->indices) cudaFree(s->indices);
	if (s->faces) cudaFree(s->faces);
	if (s->gasOutput) cudaFree(s->gasOutput);
	delete s;
}

void gpu_optix_rebuild_global_scene()
{
	if (!g_ready)
		return;
	if (g_globalScene)
	{
		gpu_optix_scene_destroy(g_globalScene);
		g_globalScene = nullptr;
	}
	g_faceIds.clear();

	std::vector<float> verts;
	std::vector<unsigned int> indices;
	std::vector<GpuFaceData> fd;
	// faces from the opaque containers must block rays outright: the Embree capture
	// already routed !bOpaque faces with no-alpha textures there, so re-deriving
	// transparency from the face alone would turn those fences see-through
	auto capture = [&verts, &indices, &fd](TriangleContainer& c, bool allowTransparent)
	{
		const unsigned int base = (unsigned int)(verts.size() / 3);
		for (const VertexEmbree& v : c.vertex())
		{
			verts.push_back(v.x);
			verts.push_back(v.y);
			verts.push_back(v.z);
		}
		for (const TriEmbree& t : c.faces())
		{
			indices.push_back(t.point1 + base);
			indices.push_back(t.point2 + base);
			indices.push_back(t.point3 + base);
		}
		for (Face* F : c.dummy)
		{
			GpuFaceData d;
			fill_face_data(F, d);
			if (!allowTransparent)
			{
				d.opaque = 1;
				d.texIdx = -1;
			}
			fd.push_back(d);
		}
	};
	capture(EmbreeMain.static_geom, false);
	capture(EmbreeMain.static_geom_transp, true);
	capture(EmbreeMain.murefs_geom, false);
	capture(EmbreeMain.murefs_geom_transp, true);

	// face id lookup for skip resolution (same order as the face data array)
	{
		size_t faceIdx = 0;
		auto map_ids = [&faceIdx](TriangleContainer& c)
		{
			for (Face* F : c.dummy)
				g_faceIds[F] = (int)faceIdx++;
		};
		map_ids(EmbreeMain.static_geom);
		map_ids(EmbreeMain.static_geom_transp);
		map_ids(EmbreeMain.murefs_geom);
		map_ids(EmbreeMain.murefs_geom_transp);
	}

	// Embree parity for the global scene: same tnear as the CPU Embree path
	// (EMBREE_TNEAR 0.01 - 0.1 tunnelled through thin walls) and the Embree
	// energy cutoff
	g_globalScene = gpu_optix_scene_create(verts.data(), verts.size(), indices.data(), fd.data(), fd.size(), 0.01f, EmbreeEnergyMAX);
	if (!g_globalScene)
		clMsg("[GpuBaker] global scene build failed");
	else
		AditionalData("[GpuBaker] global scene: %u faces", (unsigned)fd.size());
}

GpuBakeScene* gpu_optix_global_scene()
{
	return g_globalScene;
}

bool gpu_optix_ready()
{
	return g_ready && g_globalScene;
}

int gpu_optix_face_id(base_Face* F)
{
	std::lock_guard<std::mutex> lock(g_faceIdsMutex);
	auto it = g_faceIds.find(F);
	return it == g_faceIds.end() ? -1 : it->second;
}

bool gpu_optix_bake_points(GpuBakeScene* scene, const GpuBakePoint* points, int count,
	GpuBakeColor* out, unsigned int flags)
{
	if (!g_ready || !scene || !points || count <= 0 || !out)
		return false;

	// per-thread stream + scratch buffers so worker threads can bake in parallel
	static thread_local cudaStream_t tls_stream = nullptr;
	static thread_local void* tls_points = nullptr;		size_t tls_pointsBytes = 0;
	static thread_local void* tls_out = nullptr;		size_t tls_outBytes = 0;
	static thread_local GpuLaunchParams* tls_params = nullptr;

	if (!tls_stream && !cuda_check(cudaStreamCreate(&tls_stream), "stream create"))
		return false;
	if (!tls_params && !cuda_check(cudaMalloc(&tls_params, sizeof(GpuLaunchParams)), "params malloc"))
		return false;

	const size_t pointsBytes = (size_t)count * sizeof(GpuBakePoint);
	const size_t outBytes = (size_t)count * sizeof(GpuBakeColor);
	if (tls_pointsBytes < pointsBytes)
	{
		if (tls_points) cudaFree(tls_points);
		if (!cuda_check(cudaMalloc(&tls_points, pointsBytes), "points malloc"))
			return false;
		tls_pointsBytes = pointsBytes;
	}
	if (tls_outBytes < outBytes)
	{
		if (tls_out) cudaFree(tls_out);
		if (!cuda_check(cudaMalloc(&tls_out, outBytes), "out malloc"))
			return false;
		tls_outBytes = outBytes;
	}
	if (!cuda_check(cudaMemcpyAsync(tls_points, points, pointsBytes, cudaMemcpyHostToDevice, tls_stream), "points upload"))
		return false;

	SceneImpl* s = (SceneImpl*)scene;
	GpuLaunchParams p;
	p.handle = s->handle;
	p.points = (const GpuBakePoint*)tls_points;
	p.out = (GpuBakeColor*)tls_out;
	p.pointCount = count;
	p.lightsRgb = (const GpuLight*)g_dLightsRgb;		p.lightsRgbCount = g_lightsRgbCount;
	p.lightsSun = (const GpuLight*)g_dLightsSun;		p.lightsSunCount = g_lightsSunCount;
	p.lightsHemi = (const GpuLight*)g_dLightsHemi;		p.lightsHemiCount = g_lightsHemiCount;
	p.textures = (const GpuTextureDesc*)g_dTextures;	p.textureCount = g_textureCount;
	p.faces = (const GpuFaceData*)s->faces;
	p.energyMax = s->energyMax;
	p.tmin = s->tmin;
	p.glLinear = g_glLinear;
	p.flags = flags;
	if (!cuda_check(cudaMemcpyAsync(tls_params, &p, sizeof(p), cudaMemcpyHostToDevice, tls_stream), "params upload"))
		return false;

	const unsigned int threads = 256;
	unsigned int blocks = (unsigned int)((count + threads - 1) / threads);
	if (blocks > 32768)
		blocks = 32768;
	if (optixLaunch(g_pipeline, tls_stream, (CUdeviceptr)tls_params, sizeof(GpuLaunchParams), &g_sbt, blocks, 1, 1) != OPTIX_SUCCESS)
	{
		clMsg("[GpuBaker] optixLaunch failed");
		return false;
	}
	if (!cuda_check(cudaMemcpy(out, tls_out, outBytes, cudaMemcpyDeviceToHost), "results download"))
		return false;
	return true;
}

void gpu_optix_shutdown()
{
	g_ready = false;
	if (g_globalScene)
	{
		gpu_optix_scene_destroy(g_globalScene);
		g_globalScene = nullptr;
	}
	g_faceIds.clear();
	if (g_dParams) cudaFree(g_dParams);
	if (g_dTextures) cudaFree(g_dTextures);
	for (void* p : g_texturePixels)
		if (p) cudaFree(p);
	g_texturePixels.clear();
	g_textureCount = 0;
	if (g_dLightsRgb) cudaFree(g_dLightsRgb);
	if (g_dLightsSun) cudaFree(g_dLightsSun);
	if (g_dLightsHemi) cudaFree(g_dLightsHemi);
	g_dLightsRgb = g_dLightsSun = g_dLightsHemi = nullptr;
	g_lightsRgbCount = g_lightsSunCount = g_lightsHemiCount = 0;
	if (g_sbtRaygen) cudaFree(g_sbtRaygen);
	if (g_sbtMiss) cudaFree(g_sbtMiss);
	if (g_sbtHit) cudaFree(g_sbtHit);
	g_sbt = {};
	if (g_pipeline) optixPipelineDestroy(g_pipeline);
	if (g_pgRaygen) optixProgramGroupDestroy(g_pgRaygen);
	if (g_pgMiss) optixProgramGroupDestroy(g_pgMiss);
	if (g_pgHit) optixProgramGroupDestroy(g_pgHit);
	if (g_module) optixModuleDestroy(g_module);
	if (g_ctx) optixDeviceContextDestroy(g_ctx);
	g_pipeline = nullptr;
	g_pgRaygen = g_pgMiss = g_pgHit = nullptr;
	g_module = nullptr;
	g_ctx = nullptr;
}

#else // !XR_GPU_BAKER_HAS_OPTIX

#include "OptiXBakerHost.h"

// stubs: GPU baker compiled out, all callers fall back to the CPU path
bool gpu_optix_init() { return false; }
void gpu_optix_rebuild_global_scene() {}
GpuBakeScene* gpu_optix_global_scene() { return nullptr; }
void gpu_optix_shutdown() {}
bool gpu_optix_ready() { return false; }
GpuBakeScene* gpu_optix_scene_create(const float*, size_t, const unsigned int*, const GpuFaceData*, size_t) { return nullptr; }
GpuBakeScene* gpu_optix_scene_from_cdb(CDB::MODEL*) { return nullptr; }
void gpu_optix_scene_destroy(GpuBakeScene*) {}
int gpu_optix_face_id(base_Face*) { return -1; }
bool gpu_optix_bake_points(GpuBakeScene*, const GpuBakePoint*, int, GpuBakeColor*, unsigned int) { return false; }

#endif
