#include "StdAfx.h"
#include "Build.h"

#include "../xrForms/CompilersUI.h"
#include "../xrForms/xrThread.h"

#include "../xrLC_Light/xrDeflector.h"
#include "../xrLC_Light/xrLC_GlobalData.h"
#include "../xrLC_Light/xrLightVertex.h"
#include "../xrLC_Light/xrFace.h"

#include "../../xrCore/xrSyncronize.h"

#include "../xrLC_Light/mu_model_light.h"
xrCriticalSection task_CS;

// xrDeflectoL_Direct.cpp: resets the cross-deflector GPU bake batch state
void LmapsGpuBatchReset();

#include <random>

static thread_local std::mt19937 rng = std::mt19937(std::random_device()());
xr_vector<int>		task_pool;
xr_atomic_u32		ProgressData;
xr_atomic_u32		ProgressShown;	// monotonic guard: deflectors complete out of order
class CLMThread		: public CThread
{
private:
	HASH			H;
	CDB::COLLIDER	DB;
	base_lighting	LightsSelected;
public:
	CLMThread	(u32 ID) : CThread(ID)
	{
		// thMonitor= TRUE;
		thMessages	= FALSE;
	}

	virtual void	Execute()
	{
		CDeflector* D	= 0;

		for (;;)
		{
			// Get task
			task_CS.Enter		();

 			if (task_pool.empty())
			{
				task_CS.Leave		();
				return;
			}

			u32 ID = task_pool.back();
			D					= lc_global_data()->g_deflectors()[ID];
			task_pool.pop_back	();
			task_CS.Leave		();

			// Perform operation
			try {
				LC_AUDIT_LOG("  Deflector[%u]: starting light bake", ID);
				D->Light	(&DB,&LightsSelected,H);
				LC_AUDIT_LOG("  Deflector[%u]: light bake done", ID);
			} catch (...)
			{
				clMsg("* ERROR: CLMThread::Execute - light");
			}

			// Progress tracks COMPLETED deflectors (GPU bakes are quick, so
			// claim-count would pin the bar at 100% while heavy deflectors
			// still run); the CAS max-guard keeps the display strictly
			// monotonic even though deflectors complete out of order.
			const u32 done = ProgressData.fetch_add(1) + 1;
			if (done % 8 == 0)
				AditionalData("Deflectors: %u / %u", done, lc_global_data()->g_deflectors().size());
			u32 shown = ProgressShown.load();
			while (done > shown)
			{
				if (ProgressShown.compare_exchange_weak(shown, done))
				{
					Progress(float(done) / float(lc_global_data()->g_deflectors().size()));
					break;
				}
			}
		}
	}
};

void	CBuild::LMapsLocal				()
{
		mem_Compact		();

		// reset the cross-deflector GPU batch state (dead flag, queues, telemetry)
		LmapsGpuBatchReset();

		// Randomize deflectors
		std::shuffle(lc_global_data()->g_deflectors().begin(), lc_global_data()->g_deflectors().end(), rng);

		for(u32 dit = 0; dit<lc_global_data()->g_deflectors().size(); dit++)	
			task_pool.push_back(dit);
	

		// Main process (4 threads)
		Status			("Lighting...");
		CThreadManager	threads;
 		
		CTimer	start_time;	
		start_time.Start();				
		for				(int L=0; L< gCompilerMode.ThreadsPerWork; L++)	threads.start(new CLMThread (L));
		threads.wait	(500);
		clMsg			("%f seconds",start_time.GetElapsed_sec());
}

void	CBuild::LMaps					()
{
	LMapsLocal();
}
  
void CBuild::BuildAdaptiveHT()
{
	if (!gCompilerMode.LC_BackingDisabled)
	{
		//****************************************** HEMI-Tesselate
		Phase("Adaptive HT...");
		xrPhase_AdaptiveHT();
	}
}

void CBuild::Light()
{
	LC_AUDIT_LOG("=== LIGHT PIPELINE START ===");

 	//****************************************** Resolve materials
 	Phase("Resolving materials...");
 	LC_AUDIT_LOG("ResolveMaterials: materials=%u, deflectors pre-resolve=%u",
		materials().size(), lc_global_data()->g_deflectors().size());
 	xrPhase_ResolveMaterials();
	LC_AUDIT_LOG("ResolveMaterials done: deflectors=%u", lc_global_data()->g_deflectors().size());
	IsolateVertices(TRUE);

	//****************************************** UV mapping
 	Phase("Build UV mapping...");
 	LC_AUDIT_LOG("UVmap start: lmaps_size=%d, pixels_per_meter=%.1f, deflectors=%u",
		gCompilerMode.LC_sizeLmaps, g_params().m_lm_pixels_per_meter,
		lc_global_data()->g_deflectors().size());
 	xrPhase_UVmap();
	IsolateVertices(TRUE);
	LC_AUDIT_LOG("UVmap done, IsolateVertices done");
 
	//****************************************** Subdivide geometry
	Phase("Subdividing geometry...");
	LC_AUDIT_LOG("Subdivide start: skip=%d, faces_before=%u",
		lc_global_data()->GetSkipSubdivide(), lc_global_data()->g_faces().size());
	xrPhase_Subdivide();
	lc_global_data()->vertices_isolate_and_pool_reload();
	IsolateVertices(TRUE);
	LC_AUDIT_LOG("Subdivide done: faces=%u", lc_global_data()->g_faces().size());

	if (!gCompilerMode.LC_BackingDisabled)
	{
		//****************************************** GLOBAL-RayCast model
		Phase("Building rcast-CFORM model...");
		LC_AUDIT_LOG("BuildRapid (rcast CFORM) start");
		Light_prepare();
		BuildRapid(TRUE);
		LC_AUDIT_LOG("BuildRapid done");

		//****************************************** Implicit
		Phase("LIGHT: Implicit...");
		LC_AUDIT_LOG("ImplicitLighting start (quality=%d)", g_params().m_quality);
		if (gCompilerMode.Embree_SplitBVH)
			EmbreeMain.AttachGeometrys(true);
		ImplicitLighting();
		LC_AUDIT_LOG("ImplicitLighting done");

		//****************************************** LMAPS
		Phase("LIGHT: LMaps...");
		LC_AUDIT_LOG("LMaps start: deflectors=%u, threads=%d, jitter_samples=%d, jitter_mu=%u",
			lc_global_data()->g_deflectors().size(), gCompilerMode.ThreadsPerWork,
			g_params().m_lm_jitter_samples, lc_global_data()->GetJitterMU());
		if (gCompilerMode.Embree_SplitBVH)
			EmbreeMain.AttachGeometrys(false);
		LMaps();
		LC_AUDIT_LOG("LMaps done");

		//****************************************** Vertex
		Phase("LIGHT: Vertex...");
		LC_AUDIT_LOG("LightVertex start");
		LightVertex();
		LC_AUDIT_LOG("LightVertex done");
 
		//****************************************** Merge LMAPS
		Phase("LIGHT: Merging lightmaps...");
		LC_AUDIT_LOG("MergeLM start: lightmaps=%u", lc_global_data()->lightmaps().size());
		xrPhase_MergeLM();
		LC_AUDIT_LOG("MergeLM done: lightmaps=%u", lc_global_data()->lightmaps().size());

		// Save Lmaps
		Phase("LIGHT: Save lightmaps...");
		LC_AUDIT_LOG("SaveLmaps start: lmaps_RGBA=%d", lc_global_data()->GetLmapRGBA());
		xrPhase_SaveLmaps();
		LC_AUDIT_LOG("SaveLmaps done");
	}

	//****************************************** Merge geometry
	Phase("Merging geometry...");
 	LC_AUDIT_LOG("MergeGeometry start: faces pre-merge=%u", lc_global_data()->g_faces().size());
 	xrPhase_MergeGeometry();
	LC_AUDIT_LOG("MergeGeometry done: faces=%u", lc_global_data()->g_faces().size());

	if (!gCompilerMode.LC_BackingDisabled)
	{
		//****************************************** Starting MU
		Phase("LIGHT: Starting MU...");
		LC_AUDIT_LOG("StartMu: mu_models=%u, mu_refs=%u, EmbreeSplitBVH=%d",
			mu_models().size(), mu_refs().size(), gCompilerMode.Embree_SplitBVH);
		Light_prepare();
		if (gCompilerMode.Embree_SplitBVH)
			EmbreeMain.AttachGeometrys(true);
		StartMu();
		LC_AUDIT_LOG("StartMu done");
	}
	
	//****************************************** Destroy RCast-model
 	Phase("Destroying ray-trace model...");
 	lc_global_data()->destroy_rcmodel();
	if (lc_global_data()->GetIsIntelUse())
		EmbreeMain.IntelEmbereUNLOAD();
	LC_AUDIT_LOG("=== LIGHT PIPELINE END ===");
}

void CBuild::LightVertex	()
{
	::LightVertex();
}