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
				D->Light	(&DB,&LightsSelected,H);
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
 	//****************************************** Resolve materials
 	Phase("Resolving materials...");
 	xrPhase_ResolveMaterials();
	IsolateVertices(TRUE);

	//****************************************** UV mapping
 	Phase("Build UV mapping...");
 	xrPhase_UVmap();
	IsolateVertices(TRUE);
	 
	//****************************************** Subdivide geometry
	Phase("Subdividing geometry...");
	xrPhase_Subdivide();
	lc_global_data()->vertices_isolate_and_pool_reload();
	IsolateVertices(TRUE);

	if (!gCompilerMode.LC_BackingDisabled)
	{
		//****************************************** GLOBAL-RayCast model
		Phase("Building rcast-CFORM model...");
		Light_prepare();
		BuildRapid(TRUE);

		//****************************************** Implicit
		Phase("LIGHT: Implicit...");
		if (gCompilerMode.Embree_SplitBVH)
			EmbreeMain.AttachGeometrys(true);
		ImplicitLighting();

		//****************************************** LMAPS
		Phase("LIGHT: LMaps...");
		if (gCompilerMode.Embree_SplitBVH)
			EmbreeMain.AttachGeometrys(false);
		LMaps();

		//****************************************** Vertex
		Phase("LIGHT: Vertex...");
		LightVertex();
 
		//****************************************** Merge LMAPS
		Phase("LIGHT: Merging lightmaps...");
		xrPhase_MergeLM();

		// Save Lmaps
		Phase("LIGHT: Save lightmaps...");
		xrPhase_SaveLmaps();
	}

	//****************************************** Merge geometry
	Phase("Merging geometry...");
 	xrPhase_MergeGeometry();

	if (!gCompilerMode.LC_BackingDisabled)
	{
		//****************************************** Starting MU
		Phase("LIGHT: Starting MU...");
		Light_prepare();
		if (gCompilerMode.Embree_SplitBVH)
			EmbreeMain.AttachGeometrys(true);
		StartMu();
	}
	
	//****************************************** Destroy RCast-model
 	Phase("Destroying ray-trace model...");
 	lc_global_data()->destroy_rcmodel();
	if (lc_global_data()->GetIsIntelUse())
		EmbreeMain.IntelEmbereUNLOAD();
}

void CBuild::LightVertex	()
{
	::LightVertex();
}