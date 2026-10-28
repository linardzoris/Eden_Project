#include "stdafx.h"
//#include "build.h"
//#include "std_classes.h"
#include "../xrForms/xrThread.h"
#include "../xrForms/CompilersUI.h"
#include "xrDeflector.h"
#include "xrLC_GlobalData.h"
#include "light_point.h"
#include "xrFace.h"

#include "gpu_baker/OptiXBakerHost.h"

#include <memory>
#include <mutex>

extern void Jitter_Select	(Fvector2* &Jitter, u32& Jcount);
extern CompilersMode gCompilerMode;

// GPU bake of the deflector light layers: the same per-lumel jitter/triangle
// walk as L_Direct, but every (lumel, jitter, face) LightPoint runs in batched
// OptiX launches instead of per-ray CPU tracing. Light selection equivalence:
// base_lighting::select only drops point lights that cannot reach the deflector
// sphere, and process_light re-checks range per point, so baking against the
// full uploaded light set yields identical contributions.
//
// Samples from ALL deflectors (all worker threads) are aggregated into one
// global batch: a launch only fires once ~1M samples have accumulated. A
// per-deflector launch would pay the fixed WDDM submit/sync cost (~20ms) a few
// thousand times for tiny batches, which measured slower than the CPU path.
// Each sample carries a shared fold-context (the owning deflector's float
// accumulators); after a launch the submitter folds results back through those
// contexts, and the owning thread waits until all of its samples are folded
// before writing its layer.
// Any failure (fault/bake error) flips a global dead flag; every thread then
// reruns its own CPU loop, which overwrites every texel, so partial GPU results
// need no cleanup.

struct LmapsFoldCtx
{
	u32						W = 0, H = 0;
	xr_vector<base_color_c>	acc;
	xr_vector<u32>			counts;
	std::atomic<u32>		pushed{ 0 };	// samples queued for this deflector
	std::atomic<u32>		folded{ 0 };	// samples folded back into acc/counts
};
using LmapsFoldCtxPtr = std::shared_ptr<LmapsFoldCtx>;

static std::mutex					s_batchMutex;
static xr_vector<GpuBakePoint>		s_batchPts;
static xr_vector<LmapsFoldCtxPtr>	s_batchCtx;
static xr_vector<u32>				s_batchTexel;
static std::atomic<u32>				s_bakesInFlight{ 0 };
static bool							s_gpuDead = false;
static u32							s_batchFlags = 0;
static GpuBakeScene*				s_batchScene = nullptr;

static xr_atomic_u32	s_lmapsGpuCount = 0;
static xr_atomic_u32	s_lmapsFallbackCount = 0;
static xr_atomic_u32	s_lmapsCappedCount = 0;
static xr_atomic_u32	s_lmapsBakeMs = 0;
static xr_atomic_u32	s_lmapsLaunches = 0;

// Per-phase reset (called once before the LMaps worker threads start).
void LmapsGpuBatchReset()
{
	std::lock_guard<std::mutex> L(s_batchMutex);
	s_batchPts.clear();
	s_batchCtx.clear();
	s_batchTexel.clear();
	s_gpuDead = false;
	s_lmapsGpuCount.store(0);
	s_lmapsFallbackCount.store(0);
	s_lmapsCappedCount.store(0);
	s_lmapsBakeMs.store(0);
	s_lmapsLaunches.store(0);
}

// Bake the accumulated global batch and fold results into the owners' contexts.
// Returns false only when the GPU is dead - all callers fall back to CPU.
static bool LmapsDrainBatch()
{
	xr_vector<GpuBakePoint>		pts;
	xr_vector<LmapsFoldCtxPtr>	ctxs;
	xr_vector<u32>				texels;
	{
		std::lock_guard<std::mutex> L(s_batchMutex);
		if (s_gpuDead)
			return false;
		pts.swap(s_batchPts);
		ctxs.swap(s_batchCtx);
		texels.swap(s_batchTexel);
	}
	if (pts.empty())
		return true;

	xr_vector<GpuBakeColor> colors(pts.size());
	s_lmapsLaunches.fetch_add(1);
	s_bakesInFlight.fetch_add(1);
	CTimer tmr; tmr.Start();
	const bool ok = gpu_optix_bake_points(s_batchScene, &pts[0], (int)pts.size(), &colors[0], s_batchFlags);
	s_bakesInFlight.fetch_sub(1);
	if (u32 ms = tmr.GetElapsed_ms())
		s_lmapsBakeMs.fetch_add(ms);
	if (!ok)
	{
		std::lock_guard<std::mutex> L(s_batchMutex);
		s_gpuDead = true;
		clMsg("[GpuBaker] lmaps: bake failed (%u points), GPU dead - all deflectors fall back to CPU", (u32)pts.size());
		return false;
	}

	// Fold under the batch mutex: two concurrent drainers may hold samples of
	// the SAME deflector, and acc[t].add/counts[t]++ are not atomic.
	std::lock_guard<std::mutex> L(s_batchMutex);
	for (size_t i = 0; i < pts.size(); i++)
	{
		LmapsFoldCtx& C = *ctxs[i];
		const GpuBakeColor& c = colors[i];
		base_color_c cc;
		cc.rgb.set(c.rgb[0], c.rgb[1], c.rgb[2]);
		cc.sun = c.sun;
		cc.hemi = c.hemi;
		const u32 t = texels[i];
		C.acc[t].add(cc);
		C.counts[t]++;
		C.folded.fetch_add(1, std::memory_order_release);
	}
	return true;
}

static bool L_Direct_GPU_impl(lm_layer& lm, HASH& H, u32 flags)
{
	const u32 W = lm.width;
	const u32 Hgt = lm.height;
	if (!W || !Hgt)
		return false;

	// The fold buffers cost ~36 bytes per texel; giant deflectors (up to the
	// full atlas size) would allocate hundreds of MB per worker thread - leave
	// those to the CPU path, which streams texel by texel.
	if (size_t(W) * Hgt > (1u << 20))
	{
		s_lmapsCappedCount.fetch_add(1);
		return false;
	}

	// Full scene, matching the CPU Embree path (AttachGeometrys always attaches
	// all four containers, so static lightmap rays see MU proxies there too).
	GpuBakeScene* scene = gpu_optix_global_scene();
	if (!scene)
		return false;
	{
		// drain uses the phase-wide scene/flags
		std::lock_guard<std::mutex> L(s_batchMutex);
		s_batchScene = scene;
		s_batchFlags = flags;
	}

	// Setup variables (identical to L_Direct)
	Fvector2	dim, half;
	dim.set		(float(W), float(Hgt));
	half.set	(.5f / dim.x, .5f / dim.y);

	Fvector2	JS;
	JS.set		(.4999f / dim.x, .4999f / dim.y);

	u32			Jcount;
	Fvector2*	Jitter;
	Jitter_Select(Jitter, Jcount);

	LmapsFoldCtxPtr ctx = std::make_shared<LmapsFoldCtx>();
	ctx->W = W;
	ctx->H = Hgt;
	ctx->acc.resize(size_t(W) * Hgt);
	ctx->counts.assign(size_t(W) * Hgt, 0);

	const size_t BAKE_CHUNK = 1 << 20;	// ~1M samples per launch

	// resolve face -> GPU scene id (per-deflector one-slot cache: consecutive
	// samples usually belong to the same face)
	Face* lastFace = nullptr;
	int lastFid = -1;

	for (u32 V = 0; V < Hgt; V++)
	{
		for (u32 U = 0; U < W; U++)
		{
			for (u32 J = 0; J < Jcount; J++)
			{
				// LUMEL space
				Fvector2 P;
				P.x = float(U) / dim.x + half.x + Jitter[J].x * JS.x;
				P.y = float(V) / dim.y + half.y + Jitter[J].y * JS.y;

				xr_vector<UVtri*>&	space = H.query(P.x, P.y);

				// World space
				for (UVtri** it = &*space.begin(); it != &*space.end(); it++)
				{
					Fvector	B;
					if (!(*it)->isInside(P, B))
						continue;

					// We found triangle and have barycentric coords
					Face* F = (*it)->owner;
					Vertex* V1 = F->v[0];
					Vertex* V2 = F->v[1];
					Vertex* V3 = F->v[2];
					Fvector wP, wN;
					wP.from_bary(V1->P, V2->P, V3->P, B);
					{
						wN.from_bary(V1->N, V2->N, V3->N, B);
						exact_normalize(wN);
						wN.add(F->N);
						exact_normalize(wN);
					}

					int fid;
					if (F == lastFace)
					{
						fid = lastFid;
					}
					else
					{
						fid = gpu_optix_face_id(F);
						lastFace = F;
						lastFid = fid;
						if (fid < 0)
						{
							// face is not in the GPU scene - let the CPU path handle it
							if (s_lmapsFallbackCount.fetch_add(1) == 0)
								clMsg("[GpuBaker] lmaps: face not in GPU scene, falling back to CPU for this deflector");
							return false;
						}
					}

					GpuBakePoint bp;
					bp.P[0] = wP.x;	bp.P[1] = wP.y;	bp.P[2] = wP.z;
					bp.N[0] = wN.x;	bp.N[1] = wN.y;	bp.N[2] = wN.z;
					bp.skipFace = fid;	// same face the CPU path passes as skip

					bool needFlush = false;
					{
						std::lock_guard<std::mutex> L(s_batchMutex);
						if (s_gpuDead)
							return false;
						s_batchPts.push_back(bp);
						s_batchCtx.push_back(ctx);
						s_batchTexel.push_back(V * W + U);
						ctx->pushed.fetch_add(1, std::memory_order_relaxed);
						needFlush = (s_batchPts.size() >= BAKE_CHUNK);
					}
					if (needFlush && !LmapsDrainBatch())
						return false;
					break;	// first triangle per jitter only, same as CPU
				}
			}
		}
	}

	// Wait until every sample of THIS deflector has been folded. Draining must
	// stay lazy: eager drains per finished deflector re-create one launch per
	// deflector (the exact overhead this batching exists to avoid). Help only
	// when the queue is full, or when it stalled small (phase tail).
	u32 lastQ = u32(-1);
	u32 idleSpins = 0;
	while (true)
	{
		if (s_gpuDead)
			return false;
		if (ctx->folded.load(std::memory_order_acquire) == ctx->pushed.load(std::memory_order_acquire))
			break;

		size_t q = 0;
		{
			std::lock_guard<std::mutex> L(s_batchMutex);
			q = s_batchPts.size();
		}
		if (q >= BAKE_CHUNK)
		{
			LmapsDrainBatch();
			lastQ = u32(-1);
			idleSpins = 0;
			continue;
		}
		if (q == 0)
		{
			Sleep(0);	// our samples are inside an in-flight bake
			continue;
		}
		if (q == lastQ)
		{
			// queue stalled small - either a bake is in flight or everyone is
			// waiting; give it a chance, then drain the tail ourselves
			if (++idleSpins >= 64)
			{
				LmapsDrainBatch();
				idleSpins = 0;
				lastQ = u32(-1);
			}
			else
				Sleep(1);
		}
		else
		{
			idleSpins = 0;
			Sleep(0);
		}
		lastQ = (u32)q;
	}

	// Fold into the layer (C.scale(Fcount); C.mul(.5f), as the CPU path)
	for (u32 V = 0; V < Hgt; V++)
	{
		for (u32 U = 0; U < W; U++)
		{
			const u32 t = V * W + U;
			if (ctx->counts[t])
			{
				base_color_c C = ctx->acc[t];
				C.scale(ctx->counts[t]);
				C.mul(.5f);
				lm.surface[t]._set(C);
				lm.marker[t] = 255;
			}
			else
			{
				base_color_c Z;
				lm.surface[t]._set(Z);
				lm.marker[t] = 0;
			}
		}
	}

	// light periodic telemetry (clMsg goes to the log only)
	const u32 done = s_lmapsGpuCount.fetch_add(1) + 1;
	if (done % 256 == 0)
		clMsg("[GpuBaker] lmaps: %u GPU | %u capped | bake %ums in %u launches (~%ums/launch)",
			done, s_lmapsCappedCount.load(), s_lmapsBakeMs.load(), s_lmapsLaunches.load(),
			s_lmapsLaunches.load() ? s_lmapsBakeMs.load() / s_lmapsLaunches.load() : 0);
	return true;
}

// Exception-safe wrapper: any failure (AV, bad_alloc - C++ exceptions surface
// as SEH 0xE06D7363 under /EHa) inside the walk falls back to the CPU loop so
// a deflector can never end up unlit-black. SEH (not C++ catch) so the fault
// code gets logged - the wrapper frame itself holds no C++ objects.
static void LogGpuWalkFault(const EXCEPTION_POINTERS* ep)
{
	const DWORD code = ep ? ep->ExceptionRecord->ExceptionCode : 0;
	const void* addr = ep ? ep->ExceptionRecord->ExceptionAddress : nullptr;
	clMsg("[GpuBaker] lmaps: GPU path fault 0x%08X at %p (layer %ux%u), falling back to CPU",
		code, addr, (u32)0, (u32)0);
}

static bool L_Direct_GPU_impl(lm_layer& lm, HASH& H, u32 flags);

static bool L_Direct_GPU(lm_layer& lm, HASH& H, u32 flags)
{
	__try
	{
		return L_Direct_GPU_impl(lm, H, flags);
	}
	__except (LogGpuWalkFault(GetExceptionInformation()), EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
}

void CDeflector::L_Direct_Edge (CDB::COLLIDER* DB, base_lighting* LightsSelected, Fvector2& p1, Fvector2& p2, Fvector& v1, Fvector& v2, Fvector& N, float texel_size, Face* skip)
{
	Fvector		vdir;
	vdir.sub	(v2,v1);
	
	lm_layer&	lm	= layer;
	
	Fvector2		size; 
	size.x			= p2.x-p1.x;
	size.y			= p2.y-p1.y;
	int	du			= iCeil(_abs(size.x)/texel_size);
	int	dv			= iCeil(_abs(size.y)/texel_size);
	int steps		= _max(du,dv);
	if (steps<=0)	return;
	
	for (int I=0; I<=steps; I++)
	{
		float	time = float(I)/float(steps);
		Fvector2	uv;
		uv.x	= size.x*time+p1.x;
		uv.y	= size.y*time+p1.y;
		int	_x  = iFloor(uv.x*float(lm.width)); 
		int _y	= iFloor(uv.y*float(lm.height));
		
		if ((_x<0)||(_x>=(int)lm.width))	continue;
		if ((_y<0)||(_y>=(int)lm.height))	continue;
		
		if (lm.marker[_y*lm.width+_x])		continue;
		
		// ok - perform lighting
		base_color_c	C;
		Fvector			P;	P.mad(v1,vdir,time);
		VERIFY(inlc_global_data());

		LightPoint(DB, inlc_global_data()->RCAST_Model(), C, P, N, *LightsSelected, (inlc_global_data()->b_nosun() ? LP_dont_sun : 0) | (inlc_global_data()->b_nohemi() ? LP_dont_hemi : 0) | LP_DEFAULT, skip); //.

		C.mul		(.5f);
		if (inlc_global_data()->b_nohemi())
			C.hemi	= 1.f;	// nohemi: hemi 通道输出全白而非全黑
		lm.surface	[_y*lm.width+_x]._set	(C);
		lm.marker	[_y*lm.width+_x]		= 255;
	}
}

void CDeflector::L_Direct	(CDB::COLLIDER* DB, base_lighting* LightsSelected, HASH& H)
{
	R_ASSERT	(DB);
	R_ASSERT	(LightsSelected);

	lm_layer&	lm = layer;

	// Setup variables
	Fvector2	dim,half;
	dim.set		(float(lm.width),float(lm.height));
	half.set	(.5f/dim.x,.5f/dim.y);
	
	// Jitter data
	Fvector2	JS;
	JS.set		(.4999f/dim.x, .4999f/dim.y);
	
	u32			Jcount;
	Fvector2*	Jitter;
	Jitter_Select(Jitter, Jcount);

	u32 flags = (inlc_global_data()->b_nosun() ? LP_dont_sun : 0) | (inlc_global_data()->b_nohemi() ? LP_dont_hemi : 0) | LP_UseFaceDisable;

	// LMaps stays on the CPU path: this workload is ~1.7 billion SHORT shadow
	// rays (lumel x jitter x ~245 culled lights), which CPU Embree traces at
	// ~22ns/ray (8-wide packets, cache-friendly) while OptiX measured ~60ns/ray
	// even with full-batch occupancy - the GPU cannot win here and measured
	// slower than the CPU loop. GPU bakes remain the winners for the implicit
	// phase (sky rays) and MU model/reference lighting. The aggregated batch
	// implementation is kept for future experiments; flip the constant to
	// re-enable it.
	constexpr bool c_lmapsGpuEnabled = false;

	// GPU fast path: aggregated cross-deflector batches. On any failure the CPU
	// loop below reruns and rewrites every texel, so partial GPU results need
	// no cleanup.
	if (!(c_lmapsGpuEnabled && gCompilerMode.GPUBaking && L_Direct_GPU(lm, H, flags)))
	{
		// Lighting itself
		DB->ray_options	(0);
	
		for (u32 V=0; V<lm.height; V++)
		{
 
			for (u32 U=0; U<lm.width; U++)	
			{
 
				u32				Fcount	= 0;
				base_color_c	C;
				try {
					for (u32 J=0; J<Jcount; J++) 
					{
						// LUMEL space
						Fvector2 P;
						P.x = float(U)/dim.x + half.x + Jitter[J].x * JS.x;
						P.y = float(V)/dim.y + half.y + Jitter[J].y * JS.y;
						
						xr_vector<UVtri*>&	space	= H.query(P.x,P.y);
						
						// World space
						Fvector		wP,wN,B;
						for (UVtri** it=&*space.begin(); it!=&*space.end(); it++)
						{
							if ((*it)->isInside(P,B)) 
							{
								// We found triangle and have barycentric coords
								Face	*F	= (*it)->owner;
								Vertex	*V1 = F->v[0];
								Vertex	*V2 = F->v[1];
								Vertex	*V3 = F->v[2];
								wP.from_bary(V1->P,V2->P,V3->P,B);							
								
								{ 
									wN.from_bary(V1->N,V2->N,V3->N,B);
									exact_normalize	(wN); 
									wN.add		(F->N);				
									exact_normalize	(wN);
								}

								try 
								{
									VERIFY(inlc_global_data());

									LightPoint	(DB, inlc_global_data()->RCAST_Model(), C, wP, wN, *LightsSelected, flags, F); 

									Fcount		+= 1;
								} 
								catch (...)
								{
									clMsg("* ERROR (CDB). Recovered. ");
								}
								break;
							}
						}
					} 
				}
				catch (...)
				{
					clMsg("* ERROR (Light). Recovered. ");
				}
				
				if (Fcount) {
					C.scale			(Fcount);
					C.mul			(.5f);
					if (inlc_global_data()->b_nohemi())
						C.hemi		= 1.f;	// nohemi: hemi 通道输出全白而非全黑
					lm.surface		[V*lm.width+U]._set(C);
					lm.marker		[V*lm.width+U] = 255;
				} else {
					if (inlc_global_data()->b_nohemi())
						C.hemi		= 1.f;
					lm.surface		[V*lm.width+U]._set(C);	// 0-0-0-0-0
					lm.marker		[V*lm.width+U] = 0;
				}
			}
		}
	}
	// *** Render Edges
	float texel_size = (1.f/float(_max(lm.width,lm.height)))/8.f;
	for (u32 t=0; t<UVpolys.size(); t++)
	{
		UVtri&		T	= UVpolys[t];
		Face*		F	= T.owner;
		R_ASSERT	(F);
		try {
			L_Direct_Edge	(DB,LightsSelected, T.uv[0], T.uv[1], F->v[0]->P, F->v[1]->P, F->N, texel_size,F);
			L_Direct_Edge	(DB,LightsSelected, T.uv[1], T.uv[2], F->v[1]->P, F->v[2]->P, F->N, texel_size,F);
			L_Direct_Edge	(DB,LightsSelected, T.uv[2], T.uv[0], F->v[2]->P, F->v[0]->P, F->N, texel_size,F);
		} catch (...)
		{
			clMsg("* ERROR (Edge). Recovered. ");
		}
	}
}
