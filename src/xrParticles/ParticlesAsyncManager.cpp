#include "stdafx.h"
#include "ParticlesAsyncManager.h"
#include "ParticlesObject.h"

#include "../Include/xrRender/ParticleCustom.h"
#include "../Include/xrRender/RenderVisual.h"
#include "../xrEngine/IGame_Persistent.h"

#include <condition_variable>
#include <mutex>

static CParticlesAsync Instance;

// Signaled when the per-frame particle CPU update completes. The render thread
// blocks on this (instead of the old yield-spin) so it stops competing for
// cores with the particle worker while waiting.
static std::mutex			s_particle_mutex;
static std::condition_variable	s_particle_cv;
static bool					s_particle_busy = false;

void CParticlesAsync::Play()
{
	if (!psDeviceFlags.test(mtParticles))
	{
		Instance.Start();
	}
}

void CParticlesAsync::Start()
{
	// Mark busy BEFORE any work (and before IsStarted): a Wait() landing in the
	// setup window must not slip past an update that is about to run.
	{
		std::lock_guard<std::mutex> lock(s_particle_mutex);
		s_particle_busy = true;
	}
	Instance.IsStarted = true;

	{
		PROF_EVENT("Particle Update");
		for (xr_shared_ptr<CPS_Instance> particle : g_pGamePersistent->ps_active)
		{
			if (particle->m_bDead)
				continue;

			Instance.UpdateParticle(particle.get());
		}
	}

	PROF_EVENT("Particle Shedule");
	for (xr_shared_ptr<CPS_Instance> particle : g_pGamePersistent->ps_active)
	{
		particle->Update(Device.dwTimeDelta);
	}

	{
		std::lock_guard<std::mutex> lock(s_particle_mutex);
		s_particle_busy = false;
	}
	s_particle_cv.notify_all();

	Instance.IsStarted = false;
}

void CParticlesAsync::Wait()
{
	if (psDeviceFlags.test(mtParticles))
	{
		PROF_EVENT("Particles Wait");
		std::unique_lock<std::mutex> lock(s_particle_mutex);
		s_particle_cv.wait(lock, [] { return !s_particle_busy; });
	}
}

void CParticlesAsync::ForceUpdate(CPS_Instance* Obj)
{
	if (!Instance.IsStarted)
		return;

	Instance.UpdateParticle(Obj);
}

bool CParticlesAsync::NeedForceUpdate()
{
	return Instance.IsStarted;
}

CParticlesAsync::CParticlesAsync()
{
	if (!DevicePtr || Device.IsEditorMode())
		return;

	if (g_dedicated_server)
		return;

	Device.ParticleWorkerCallback = Start;
}

void CParticlesAsync::UpdateParticle(CPS_Instance* particle) const
{
	u32 dt = Device.dwTimeGlobal - particle->dwLastTime;
	IParticleCustom* V = smart_cast<IParticleCustom*>(particle->renderable.visual);

	if (V == nullptr)
		return;

	PROF_EVENT("Particle OnFrame");
	V->OnFrame(dt);

	particle->dwLastTime = Device.dwTimeGlobal;
}