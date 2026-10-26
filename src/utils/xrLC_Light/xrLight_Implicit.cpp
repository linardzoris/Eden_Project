#include "stdafx.h"

#include "xrLight_Implicit.h"
#include "xrLight_ImplicitDeflector.h"

#include "light_point.h"
#include "xrDeflector.h"
#include "xrLC_GlobalData.h"
#include "xrFace.h"
#include "xrLight_ImplicitCalcGlobs.h"

#include "../../xrCore/Collision/xrCDB.h"

using Implicit = xr_map<u32, ImplicitDeflector>;
using Implicit_it = Implicit::iterator;

#include "../xrForms/xrThread.h"
#include "../xrForms/CompilersUI.h"

#include "gpu_baker/OptiXBakerHost.h"

extern CompilersMode gCompilerMode;

class ImplicitThread :
	public CThread
{
public:

	ImplicitExecute	execute;
	ImplicitThread(u32 ID, ImplicitDeflector* _DATA) : CThread(ID), execute()
	{

	}
	virtual void Execute();
};

void ImplicitThread::Execute()
{
	// Priority
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
	Sleep(0);
	execute.Execute();
}

// 2 : Mainthread + UI thread
ImplicitCalcGlobs cl_globs;
int ThreadTaskID_Implication = 0;
CTimer tImplicit;

void RunImplicitMultithread(ImplicitDeflector& defl)
{
	// Start threads
	ThreadTaskID_Implication = 0;

	tImplicit.Start();

	CThreadManager tmanager;
	for (u32 thID = 0; thID < gCompilerMode.ThreadsPerWork; thID++)
	{
		tmanager.start(new ImplicitThread(thID, &defl));
	}

	tmanager.wait();
}

xrCriticalSection csLockImplicit;

void ImplicitExecute::Execute()
{
	ImplicitDeflector& defl = cl_globs.DATA();
	CDB::COLLIDER DB;

	// Setup variables
	Fvector2 dim, half;
	dim.set(float(defl.Width()), float(defl.Height()));
	half.set(.5f / dim.x, .5f / dim.y);

	// Jitter data
	Fvector2 JS;
	JS.set(.499f / dim.x, .499f / dim.y);
	u32 Jcount;
	Fvector2* Jitter;
	Jitter_Select(Jitter, Jcount);

	// Lighting itself
	DB.ray_options(0);

	while (true)
	{
		csLockImplicit.Enter();
		int V = ThreadTaskID_Implication;
		if (ThreadTaskID_Implication >= defl.Height())
		{
			csLockImplicit.Leave();
			break;
		}
		ThreadTaskID_Implication++;

		Progress(float(V) / float(defl.Height()));
		csLockImplicit.Leave();

		for (u32 U = 0; U < defl.Width(); U++)
		{
			base_color_c C;
			u32 Fcount = 0;

			try
			{
				for (u32 J = 0; J < Jcount; J++)
				{
					// LUMEL space
					Fvector2				P;
					P.x = float(U) / dim.x + half.x + Jitter[J].x * JS.x;
					P.y = float(V) / dim.y + half.y + Jitter[J].y * JS.y;
					xr_vector<Face*>& space = cl_globs.Hash().query(P.x, P.y);

					// World space
					Fvector wP, wN, B;
					for (vecFaceIt it = space.begin(); it != space.end(); it++)
					{
						Face* F = *it;
						_TCF& tc = F->tc[0];
						if (tc.isInside(P, B))
						{
							// We found triangle and have barycentric coords
							Vertex* V1 = F->v[0];
							Vertex* V2 = F->v[1];
							Vertex* V3 = F->v[2];
							wP.from_bary(V1->P, V2->P, V3->P, B);
							wN.from_bary(V1->N, V2->N, V3->N, B);
							wN.normalize();

							u32 flags = (inlc_global_data()->b_nosun() ? LP_dont_sun : 0);
							LightPoint(&DB, inlc_global_data()->RCAST_Model(), C, wP, wN, inlc_global_data()->L_static(), flags, F);
							Fcount++;
						}
					}
				}
			}
			catch (...)
			{
				clMsg("* THREAD #%d: Access violation. Possibly recovered.");//,thID
			}

			if (Fcount)
			{
				// Calculate lighting amount
				C.scale(Fcount);
				C.mul(.5f);
				defl.Lumel(U, V)._set(C);
				defl.Marker(U, V) = 255;
			}
			else
			{
				defl.Marker(U, V) = 0;
			}
		}

		if (V % 8 == 0)
			AditionalData("CurrentV: %u | time: %.0f", V, tImplicit.GetElapsed_sec());
	}
}

static xr_vector<u32> not_clear;

// GPU bake of an implicit map: same per-texel jitter/triangle math as
// ImplicitExecute::Execute, but the LightPoint occlusion work for every
// (texel, jitter, face) sample runs in batched OptiX launches instead of one
// CPU shadow ray at a time. A 1024x1024 terrain texture is ~4M samples x ~245
// lights - hundreds of millions of rays on CPU, a handful of GPU launches here.
struct implicit_gpu_sample
{
	GpuBakePoint pt;
	u32 texel;
};

static void ImplicitLightingGPU(ImplicitDeflector& defl)
{
	CTimer tGPU;
	tGPU.Start();

	const u32 W = defl.Width();
	const u32 H = defl.Height();

	Fvector2 dim, half;
	dim.set(float(W), float(H));
	half.set(.5f / dim.x, .5f / dim.y);

	// Jitter data (same as the CPU path)
	Fvector2 JS;
	JS.set(.499f / dim.x, .499f / dim.y);
	u32 Jcount;
	Fvector2* Jitter;
	Jitter_Select(Jitter, Jcount);

	const u32 flags = (inlc_global_data()->b_nosun() ? LP_dont_sun : 0);
	GpuBakeScene* scene = gpu_optix_global_scene();
	R_ASSERT(scene);

	// pass 1: gather bake points, parallel over rows (each thread owns its row).
	// Progress must be monotonic: rows complete out of order across workers, so
	// report completed-row count instead of the row index (which jumps around).
	Status("Gathering implicit samples...");
	xr_vector<xr_vector<implicit_gpu_sample>> rows(H);
	xr_atomic_u32 rows_done = 0;
	xr_parallel_for
	(
		size_t(0), size_t(H),
		[&](size_t V)
		{
			xr_vector<implicit_gpu_sample>& dst = rows[V];
			for (u32 U = 0; U < W; U++)
			{
				for (u32 J = 0; J < Jcount; J++)
				{
					// LUMEL space
					Fvector2 P;
					P.x = float(U) / dim.x + half.x + Jitter[J].x * JS.x;
					P.y = float(V) / dim.y + half.y + Jitter[J].y * JS.y;
					xr_vector<Face*>& space = cl_globs.Hash().query(P.x, P.y);

					// World space
					for (vecFaceIt it = space.begin(); it != space.end(); it++)
					{
						Face* F = *it;
						_TCF& tc = F->tc[0];
						Fvector B;
						if (!tc.isInside(P, B))
							continue;

						// We found triangle and have barycentric coords
						Vertex* V1 = F->v[0];
						Vertex* V2 = F->v[1];
						Vertex* V3 = F->v[2];
						Fvector wP, wN;
						wP.from_bary(V1->P, V2->P, V3->P, B);
						wN.from_bary(V1->N, V2->N, V3->N, B);
						wN.normalize();

						implicit_gpu_sample s;
						s.pt.P[0] = wP.x;	s.pt.P[1] = wP.y;	s.pt.P[2] = wP.z;
						s.pt.N[0] = wN.x;	s.pt.N[1] = wN.y;	s.pt.N[2] = wN.z;
						s.pt.skipFace = gpu_optix_face_id(F);	// same face the CPU path passes as skip
						s.texel = u32(V) * W + U;
						dst.push_back(s);
					}
				}
			}
			// 0..0.9 during gathering; GPU bake continues 0.9..1.0 below
			Progress(0.9f * float(rows_done.fetch_add(1) + 1) / float(H));
		}
	);

	// flatten into contiguous point / texel arrays
	size_t total = 0;
	for (u32 V = 0; V < H; V++)
		total += rows[V].size();

	xr_vector<GpuBakePoint> points(total);
	xr_vector<u32> texel_ids(total);
	{
		size_t off = 0;
		for (u32 V = 0; V < H; V++)
		{
			const size_t n = rows[V].size();
			for (size_t i = 0; i < n; i++)
			{
				points[off + i] = rows[V][i].pt;
				texel_ids[off + i] = rows[V][i].texel;
			}
			off += n;
			rows[V].clear();
		}
	}
	rows.clear();

	clMsg("[GpuBaker] implicit '%s': %u samples", defl.texture->name, (u32)total);

	// pass 2: batched GPU bake (continues the progress scale from gathering)
	Status("GPU baking implicit samples...");
	xr_vector<GpuBakeColor> colors(total);
	bool gpu_ok = total > 0;
	const size_t CHUNK = 1 << 19;
	for (size_t off = 0; off < total; off += CHUNK)
	{
		const size_t n = (total - off) < CHUNK ? (total - off) : CHUNK;
		Progress(0.9f + 0.1f * (float(off) / float(_max(total, size_t(1)))));
		if (!gpu_optix_bake_points(scene, &points[off], (int)n, &colors[off], flags))
		{
			clMsg("[GpuBaker] implicit bake failed, falling back to CPU");
			gpu_ok = false;
			break;
		}
	}

	if (!gpu_ok)
	{
		// let the caller run the original multithreaded CPU path
		RunImplicitMultithread(defl);
		return;
	}

	// pass 3: fold per-texel results (same C.scale(Fcount); C.mul(.5f) as CPU)
	Status("Folding implicit lighting...");
	xr_vector<base_color_c> acc(W * H);
	xr_vector<u32> counts(W * H);
	for (size_t i = 0; i < total; i++)
	{
		const GpuBakeColor& c = colors[i];
		base_color_c cc;
		cc.rgb.set(c.rgb[0], c.rgb[1], c.rgb[2]);
		cc.sun = c.sun;
		cc.hemi = c.hemi;
		const u32 t = texel_ids[i];
		acc[t].add(cc);
		counts[t]++;
	}

	for (u32 V = 0; V < H; V++)
	{
		for (u32 U = 0; U < W; U++)
		{
			const u32 t = V * W + U;
			if (counts[t])
			{
				base_color_c C = acc[t];
				C.scale(counts[t]);
				C.mul(.5f);
				defl.Lumel(U, V)._set(C);
				defl.Marker(U, V) = 255;
			}
			else
			{
				defl.Marker(U, V) = 0;
			}
		}
	}

	clMsg("[GpuBaker] implicit '%s': GPU lighting done (%.1fs)", defl.texture->name, tGPU.GetElapsed_sec());
	Progress(1.f);
}

void ImplicitLightingExec()
{

	Implicit		calculator;

	cl_globs.Allocate();
	not_clear.clear();
	// Sorting
	Status("Sorting faces...");
	for (vecFaceIt I = inlc_global_data()->g_faces().begin(); I != inlc_global_data()->g_faces().end(); I++)
	{
		Face* F = *I;
		if (F->pDeflector)				continue;
		if (!F->hasImplicitLighting())	continue;

		Progress(float(I - inlc_global_data()->g_faces().begin()) / float(inlc_global_data()->g_faces().size()));
		b_material& M = inlc_global_data()->materials()[F->dwMaterial];
		u32				Tid = M.surfidx;
		b_BuildTexture* T = &(inlc_global_data()->textures()[Tid]);

		Implicit_it		it = calculator.find(Tid);
		if (it == calculator.end())
		{
			ImplicitDeflector	ImpD;
			ImpD.texture = T;
			ImpD.faces.push_back(F);
			calculator.insert(std::make_pair(Tid, ImpD));
			not_clear.push_back(Tid);
		}
		else {
			ImplicitDeflector& ImpD = it->second;
			ImpD.faces.push_back(F);
		}
	}

	// Lighing
	for (Implicit_it imp = calculator.begin(); imp != calculator.end(); imp++)
	{
		ImplicitDeflector& defl = imp->second;
		Status("Lighting implicit map '%s'...", defl.texture->name);
		Progress(0);
		defl.Allocate();

		// Setup cache
		Progress(0);
		cl_globs.Initialize(defl);

		if (gCompilerMode.GPUBaking && gpu_optix_ready())
			ImplicitLightingGPU(defl);
		else
			RunImplicitMultithread(defl);

		defl.faces.clear();

		// Expand
		Status("Processing lightmap...");
		for (u32 ref = 254; ref > 0; ref--)	if (!ApplyBorders(defl.lmap, ref)) break;

		Status("Mixing lighting with texture...");
		{
			b_BuildTexture& TEX = *defl.texture;
			VERIFY(TEX.pSurface);
			u32* color = TEX.pSurface;
			for (u32 V = 0; V < defl.Height(); V++) {
				for (u32 U = 0; U < defl.Width(); U++) {
					// Retreive Texel
					float	h = defl.Lumel(U, V).h._r();
					u32& C = color[V * defl.Width() + U];
					C = subst_alpha(C, u8_clr(h));
				}
			}
		}

		xr_vector<u32> packed;
		defl.lmap.Pack(packed);
		defl.Deallocate();

		// base
		Status("Saving base...");
		{
			string128 name;
			string_path out_name;
			xr_strcpy(name, lc_global_data()->GetLavelName());

			R_ASSERT(name[0] && defl.texture);

			b_BuildTexture& TEX = *defl.texture;
			xr_strconcat(out_name, name, "\\", TEX.name, ".dds");
			FS.update_path(out_name, "$game_levels$", out_name);
			clMsg("Saving texture '%s'...", out_name);
			VerifyPath(out_name);
			BYTE* raw_data = LPBYTE(TEX.pSurface);
			u32	w = TEX.dwWidth;
			u32	h = TEX.dwHeight;
			u32	pitch = w * 4;
			STextureParams fmt = TEX.THM;
			fmt.fmt = lc_global_data()->GetLmapRGBA() ? STextureParams::tfRGBA : STextureParams::tfDXT5;
			fmt.flags.set(STextureParams::flDitherColor, FALSE);
			fmt.flags.set(STextureParams::flGenerateMipMaps, FALSE);
			fmt.flags.set(STextureParams::flBinaryAlpha, FALSE);
			DXTUtils::Compress(out_name, raw_data, 0, w, h, pitch, &fmt, 4);
		}

		// lmap
		Status("Saving lmap...");
		{
			string128 name;
			string_path out_name;
			xr_strcpy(name, lc_global_data()->GetLavelName());

			b_BuildTexture& TEX = *defl.texture;
			xr_strconcat(out_name, name, "\\", TEX.name, "_lm.dds");
			FS.update_path(out_name, "$game_levels$", out_name);
			clMsg("Saving texture '%s'...", out_name);
			VerifyPath(out_name);
			BYTE* raw_data = LPBYTE(&*packed.begin());
			u32	w = TEX.dwWidth;
			u32	h = TEX.dwHeight;
			u32	pitch = w * 4;
			STextureParams			fmt;
			fmt.fmt = lc_global_data()->GetLmapRGBA() ? STextureParams::tfRGBA : STextureParams::tfDXT5;
			fmt.flags.set(STextureParams::flDitherColor, FALSE);
			fmt.flags.set(STextureParams::flGenerateMipMaps, FALSE);
			fmt.flags.set(STextureParams::flBinaryAlpha, FALSE);
			DXTUtils::Compress(out_name, raw_data, 0, w, h, pitch, &fmt, 4);
		}
		//defl.Deallocate				();
	}
	not_clear.clear();
	cl_globs.Deallocate();
	calculator.clear();
}