#pragma once

// OptiX light baker host API: GPU occlusion tracing for the level compiler.
// All functions return false / no-op when the GPU baker is unavailable.

#include "OptiXBakerShared.h"

class base_Face;
namespace CDB
{
	class MODEL;
}
struct GpuBakeScene;

// Creates the OptiX context/pipeline and uploads textures + static lights
// (taken from lc_global_data). Safe to call multiple times (no-op after first).
bool gpu_optix_init();

// Rebuilds the persistent global scene from the Embree face capture
// (EmbreeMain containers). Must be called whenever that capture is refreshed.
void gpu_optix_rebuild_global_scene();

// Persistent scene matching the Embree capture; used when tracing against the
// whole level (reference lighting and the later deflector phases).
GpuBakeScene* gpu_optix_global_scene();

void gpu_optix_shutdown();
bool gpu_optix_ready();

// Builds a traceable scene from raw triangle soup; faces[] must be parallel to
// the index triplets. Returns null on failure (caller falls back to CPU).
GpuBakeScene* gpu_optix_scene_create(const float* verts, size_t vertCountF,
	const unsigned int* indices, const GpuFaceData* faces, size_t faceCount);

// Convenience wrapper: builds a scene from a CDB::MODEL (per-model cform),
// resolving per-tri face data through the convert_nax face registry.
GpuBakeScene* gpu_optix_scene_from_cdb(CDB::MODEL* mdl);

void gpu_optix_scene_destroy(GpuBakeScene* scene);

// Resolves a CPU face pointer to the global-scene face id used by GpuBakePoint
// (built during gpu_optix_rebuild_global_scene). Returns -1 when unknown.
int gpu_optix_face_id(base_Face* F);

// Traces one occlusion-light bundle per point using the lights uploaded at
// init (L_static). Thread-safe: per-thread streams and device buffers.
bool gpu_optix_bake_points(GpuBakeScene* scene, const GpuBakePoint* points, int count,
	GpuBakeColor* out, unsigned int flags);
