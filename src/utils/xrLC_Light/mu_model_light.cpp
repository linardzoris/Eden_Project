#include "stdafx.h"
#include "xrFace.h"
#include "xrMU_Model.h"
#include "xrMU_Model_Reference.h"
#include "xrLC_GlobalData.h"
#include "mu_model_light.h"

#include "../xrForms/xrThread.h"
#include "../xrForms/CompilersUI.h"

#include "../../xrCore/xrSyncronize.h"

CThreadManager mu_materials;
CThreadManager mu_secondary;
 
xrCriticalSection csMUMAPS_LOCKS;

static int ThreadTaskID = 0;

// mu-light
 
class CMULight :
	public CThread
{
public:
	CMULight(u32 ID) : CThread(ID)
	{
		thMessages = FALSE;
	}

	virtual void Execute()
	{
		// Priority
		SetThreadPriority(Platform::GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
		Sleep(0);
		while (true)
		{
  			csMUMAPS_LOCKS.Enter();
			int ID = ThreadTaskID;

			if (ThreadTaskID >= inlc_global_data()->mu_refs().size())
			{
				csMUMAPS_LOCKS.Leave();
				break;
			}

			ThreadTaskID++;

			if (ID % 512 == 0)
				Status("Models %d/%d", ID, inlc_global_data()->mu_refs().size());
			thProgress = (float(ID) / float(inlc_global_data()->mu_refs().size()));

			csMUMAPS_LOCKS.Leave();

			// Light references
			inlc_global_data()->mu_refs()[ID]->calc_lighting	();
		}
	}
};

class CMULightCalculation :
	public CThread
{
public:
	CMULightCalculation(u32 ID) : CThread(ID)
	{
		thMessages = FALSE;
	}

	virtual void	Execute()
	{
 		// Priority
		SetThreadPriority(Platform::GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
		Sleep(0);

		while (true)
		{
			csMUMAPS_LOCKS.Enter();

			int ID = ThreadTaskID;
 			if (ThreadTaskID >= inlc_global_data()->mu_models().size())
			{
				csMUMAPS_LOCKS.Leave();
				break;
			}
 			ThreadTaskID++;
			// Light references
			inlc_global_data()->mu_models()[ID]->calc_materials();
			thProgress = (float(ID) / float(inlc_global_data()->mu_models().size()));
			if (ID%512 == 0)
				Status("Models %d/%d", ID, inlc_global_data()->mu_models().size());
			csMUMAPS_LOCKS.Leave();
 
			
			inlc_global_data()->mu_models()[ID]->calc_lighting();
		}
	}
};

void run_mu_light()
{
	// Priority
	SetThreadPriority(Platform::GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
	Sleep(0);

	ThreadTaskID = 0;
	for (u32 thID = 0; thID < gCompilerMode.ThreadsPerWork; thID++)
		mu_materials.start(new CMULightCalculation(thID));
 	mu_materials.wait(100);

	// Fit the shared reference lighting once per model: the first non-unique
	// reference of each model bakes at its own position and stores c_scale/c_bias
	// into the model; every other non-unique reference then reuses it in CMULight.
	Status("MU reference lighting fits...");
	{
		xr_vector<xrMU_Reference*>	fit_refs;
		xr_vector<xrMU_Model*>		seen_models;
		for (u32 r = 0; r < inlc_global_data()->mu_refs().size(); r++)
		{
			xrMU_Reference* ref = inlc_global_data()->mu_refs()[r];
			if (ref->bake_unique)
				continue;
			if (std::find(seen_models.begin(), seen_models.end(), ref->model) != seen_models.end())
				continue;
			seen_models.push_back(ref->model);
			fit_refs.push_back(ref);
		}

		xr_parallel_for(size_t(0), fit_refs.size(), [&](size_t i)
		{
			if (!fit_refs[i]->model->reference_fitted)
				fit_refs[i]->fit_reference_lighting();
		});
	}

	// Light references
	ThreadTaskID = 0;
	for (u32 thID = 0; thID < gCompilerMode.ThreadsPerWork; thID++)
		mu_secondary.start(new CMULight(thID));
 	mu_secondary.wait(100);
}