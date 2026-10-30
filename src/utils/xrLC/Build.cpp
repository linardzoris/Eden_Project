// Build.cpp: implementation of the CBuild class.
//
//////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "Build.h"

#include "../xrLC_Light/xrMU_Model.h"
#include "../xrLC_Light/xrMU_Model_Reference.h"
#include "../xrLC_Light/xrLC_GlobalData.h"
#include "../xrLC_Light/xrFace.h"
#include "../xrLC_Light/mu_model_light.h"
#include "../xrLC_Light/xrDeflector.h"
#include "../xrLC_Light/Lightmap.h"
#include "../xrLC_Light/xrDeflectorDefs.h"

#include "../xrForms/CompilersUI.h"

#ifdef IXR_WINDOWS
#	include <psapi.h>
#endif

void calc_ogf(xrMU_Model& mu_model);
void export_geometry(xrMU_Model& mu_model);
void export_ogf(xrMU_Reference& mu_reference);

extern u16 RegisterShader(LPCSTR T);

struct OGF_Base;
xr_vector<OGF_Base*> g_tree;

SBuildOptions g_build_options;
vec2Face g_XSplit;

void CBuild::CheckBeforeSave(u32 stage)
{
	bool b_g_tree_empty = g_tree.empty();
	R_ASSERT(b_g_tree_empty);
	bool b_g_XSplit_empty = g_XSplit.empty();
	R_ASSERT(b_g_XSplit_empty);
	bool b_IsOGFContainersEmpty = IsOGFContainersEmpty();
	R_ASSERT(b_IsOGFContainersEmpty);
}

void CBuild::TempSave(u32 stage)
{
	CheckBeforeSave(stage);
}

//////////////////////////////////////////////////////////////////////
#include "../xrLC_Light/embree_raytracing/EmbreeRayTrace.h"
CBuild::CBuild()
{
	// Se7kills Initialize Device Embree

}

CBuild::~CBuild()
{
	destroy_global_data();
}
 
CMemoryWriter&	CBuild::err_invalid()
{
	VERIFY(lc_global_data()); 
	return lc_global_data()->err_invalid(); 
}
CMemoryWriter&	CBuild::err_multiedge()
{
	VERIFY(lc_global_data()); 
	return lc_global_data()->err_multiedge(); 
}
CMemoryWriter	&CBuild::err_tjunction()
{
	VERIFY(lc_global_data()); 
	return lc_global_data()->err_tjunction(); 
}
xr_vector<b_material>&	CBuild::materials()	
{
	VERIFY(lc_global_data()); 
	return lc_global_data()->materials(); 
}
xr_vector<b_BuildTexture>&	CBuild::textures()		
{
	VERIFY(lc_global_data());
	return lc_global_data()->textures(); 
}

base_lighting&	CBuild::L_static()
{
	VERIFY(lc_global_data()); return lc_global_data()->L_static(); 
}

Shader_xrLC_LIB&	CBuild::shaders()		
{
	VERIFY(lc_global_data()); 
	return lc_global_data()->shaders(); 
}

void CBuild::Light_prepare()
{
	for (vecFaceIt I=lc_global_data()->g_faces().begin();	I!=lc_global_data()->g_faces().end(); I++) (*I)->CacheOpacity();
	for (u32 m=0; m<mu_models().size(); m++)	mu_models()[m]->calc_faceopacity();
}

size_t GetHeapMemory()
{
#ifdef IXR_WINDOWS
	static size_t last_update_memory = 0;
	static CTimer tMemory;
	// Не слишком часто обновляться
	if (tMemory.GetElapsed_ms() < 2000)
	{
		return last_update_memory;
	}

	PROCESS_MEMORY_COUNTERS_EX pmc;
	if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc)))
	{
		tMemory.Start();
		last_update_memory = pmc.PrivateUsage;
		return pmc.PrivateUsage;
	}
#endif

	return 0;
}

void GetMemoryUsedStorage()
{
#ifdef DEBUG
	if (!lc_global_data())
		return;

	u32 tree = pBuild->GetTreeSize() / 1024 / 1024;
	size_t defl = 0; size_t lightmaps = 0;
	for (auto& D : lc_global_data()->g_deflectors())
	{
		defl += D->size_deflector();
	}
	defl /= (1024 * 1024);
 	for (auto&  LM  : lc_global_data()->lightmaps())
	{
		lightmaps += LM->lm.memory_lmap();
 	}
	lightmaps /= (1024 * 1024);
  	 
	size_t sV = 0;
	for (auto& V : lc_global_data()->g_vertices())
	{
		sizeof(Vertex);
		sV += V->used_memory(); 
	}
	
	sV /= (1024 * 1024);

	size_t sF = 0;// lc_global_data()->g_faces().size() * sizeof(Face);
	for (auto& F : lc_global_data()->g_faces())
	{						
		sF += sizeof(*F) + sizeof(void*); //void* vector size
	}
	
	sF /= (1024 * 1024);
	
	size_t TAlloc = 0;
	for (auto& T : lc_global_data()->textures())
	{
		TAlloc += (T.dwHeight* T.dwHeight * 4);
	}
	lc_global_data()->textures();
	size_t sTex = lc_global_data()->textures().size() * sizeof(b_BuildTexture);
	sTex += TAlloc;
	sTex /= (1024 * 1024);

	size_t SplitsMemory = 0;
	for (auto X : g_XSplit)
	{
		SplitsMemory += X->capacity() * sizeof(void*);
	}
	SplitsMemory += g_XSplit.capacity() * sizeof(void*);

	u32 MB = 1024 * 1024;

	Msg("!~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~");
	Msg("- xSplits:		%u count, %u capacity, %u mb", g_XSplit.size(), g_XSplit.capacity(), SplitsMemory / 1024 / 1024);
	
	Msg("- GeomTree:	%u mb, cap:%u",		tree, g_tree.capacity());
	Msg("- Deflectors:	%u mb, cap:%u",		defl, lc_global_data()->g_deflectors().capacity());
	Msg("- Lightmaps:	%u mb, cap:%u",		lightmaps, lc_global_data()->lightmaps().capacity());
 	Msg("- vertexes:	%u mb, store: %u, cap:%u",		sV, lc_global_data()->g_vertices().size(),  lc_global_data()->g_vertices().capacity());	// Не учитываю алокацию adjucement при создании и жрет в разы больше
	Msg("- faces:		%u mb, store: %u, cap:%u",		sF, lc_global_data()->g_faces().size(), lc_global_data()->g_faces().capacity());	// Не учитываю алокацию adjucement при создании и жрет в разы больше
	Msg("- Textures:	%u mb, cap:%u",		sTex, lc_global_data()->textures().capacity());
	Msg("- Embree BVH: %umb, Static: %umb, MU: %umb", EmbreeMain.BVH_size / MB, EmbreeMain.Static_size / MB, EmbreeMain.MU_size / MB);

	u32 memdata = tree + defl + lightmaps;
	
	u32 EmbreeMem = (EmbreeMain.BVH_size / MB) + (EmbreeMain.Static_size / MB) + (EmbreeMain.MU_size / MB);
 
	if (g_XSplit.capacity() != g_XSplit.size())
		g_XSplit.shrink_to_fit();

	if (lc_global_data()->g_deflectors().capacity() != lc_global_data()->g_deflectors().size())
		lc_global_data()->g_deflectors().shrink_to_fit();

	if (lc_global_data()->lightmaps().capacity() != lc_global_data()->lightmaps().size())
		lc_global_data()->lightmaps().shrink_to_fit();

	Msg("- Total Memory (AFTER SHIRK):	%u / (Check) %u + %u + %u mb", GetHeapMemory() / (1024 * 1024), EmbreeMem, sV + sF + sTex, memdata);


	Msg("!~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~");
#endif
}

void CBuild::Run(LPCSTR P)
{
	LC_AUDIT_LOG("=== BUILD START ===");
	LC_AUDIT_LOG("Level path: %s", P);
	LC_AUDIT_LOG("Radiosity: %d, Progressive(Noise) skip: %d",
		g_build_options.b_radiosity, g_build_options.b_noise);
	LC_AUDIT_LOG("Scene BB: min(%.2f,%.2f,%.2f) max(%.2f,%.2f,%.2f)",
		scene_bb.min.x, scene_bb.min.y, scene_bb.min.z,
		scene_bb.max.x, scene_bb.max.y, scene_bb.max.z);
	LC_AUDIT_LOG("Lights: HEMI(%u) SUN(%u) STATIC(%u)",
		L_static().hemi.size(), L_static().sun.size(), L_static().rgb.size());

	lc_global_data()->initialize();

	//****************************************** Open Level
	xr_strconcat(path, P, "\\");
	string_path					lfn;
	IWriter* fs = FS.w_open(xr_strconcat(lfn, path, "level."));
	fs->open_chunk(fsL_HEADER);
	hdrLEVEL H;
	H.XRLC_version = XRCL_PRODUCTION_VERSION;
	H.XRLC_quality = g_params().m_quality;
	fs->w(&H, sizeof(H));
	fs->close_chunk();

	// Dumb entry in shader-registration
	RegisterShader("");

	// Saving lights
	{
		string256			fn;
		IWriter* fs = FS.w_open(xr_strconcat(fn, pBuild->path, "build.lights"));
		fs->w_chunk(0, &*L_static().rgb.begin(), L_static().rgb.size() * sizeof(R_Light));
		fs->w_chunk(1, &*L_static().hemi.begin(), L_static().hemi.size() * sizeof(R_Light));
		fs->w_chunk(2, &*L_static().sun.begin(), L_static().sun.size() * sizeof(R_Light));
		FS.w_close(fs);
	}

	// Optimizing + checking for T-junctions
	mem_Compact();

	Phase("Optimizing...");
	LC_AUDIT_LOG("PreOptimize start: V(%u) F(%u)",
		lc_global_data()->g_vertices().size(), lc_global_data()->g_faces().size());
	PreOptimize();
	CorrectTJunctions();
	mem_Compact();
	LC_AUDIT_LOG("PreOptimize+TJunctions done: V(%u) F(%u) invalid_faces(%u)",
		lc_global_data()->g_vertices().size(), lc_global_data()->g_faces().size(),
		InvalideFaces());

	// AdaptiveHT
	BuildAdaptiveHT();

	// Building normals
	Phase("Building normals...");
	mem_Compact();
	LC_AUDIT_LOG("CalcNormals start: smooth_groups=%d", g_using_smooth_groups);
	CalcNormals();
	LC_AUDIT_LOG("CalcNormals done");

	// Collision DB
	//should be after normals, so that double-sided faces gets separated
	Phase("Building collision database (CFORM)...");
	mem_Compact();
	LC_AUDIT_LOG("BuildCForm start");

	if (!gCompilerMode.LC_BackingDisabled)
		BuildCForm();
	BuildPortals(*fs);
	LC_AUDIT_LOG("BuildCForm done, portals=%u", portals.size());


	// GLOBAL-ILLUMINATION
	if (g_build_options.b_radiosity)
	{
		Phase("Radiosity-Solver...");
		mem_Compact();
		LC_AUDIT_LOG("Radiosity start");
		Light_prepare();
		xrPhase_Radiosity();
		LC_AUDIT_LOG("Radiosity done");
	}

	// All lighting + lmaps building and saving
	LC_AUDIT_LOG("=== LIGHT PIPELINE START ===");
	Light();
	RunAfterLight(fs);

	LC_AUDIT_LOG("=== BUILD END ===");
}

void CBuild::StartMu()
{
	run_mu_light();
}

void CBuild::RunAfterLight(IWriter* fs)
{
	LC_AUDIT_LOG("=== RUN-AFTER-LIGHT START ===");
	LC_AUDIT_LOG("RunAfterLight input: V(%u) F(%u) MU_models(%u) MU_refs(%u)",
		lc_global_data()->g_vertices().size(), lc_global_data()->g_faces().size(),
		mu_models().size(), mu_refs().size());

	Phase("Building tangent-basis...");
	LC_AUDIT_LOG("TangentBasis start: computing TB for all faces");
	xrPhase_TangentBasis();
	mem_Compact();
	LC_AUDIT_LOG("TangentBasis done");

	//****************************************** Convert to OGF
	Phase("Converting to OGFs...");
	LC_AUDIT_LOG("Flex2OGF start: static geometry -> OGF");
	mem_Compact();
	Flex2OGF();
	LC_AUDIT_LOG("Flex2OGF done");

	//****************************************** Export MU-models
	Phase("Converting MU-models to OGFs...");
	mem_Compact();
	LC_AUDIT_LOG("MU calc_ogf: models=%u, refs=%u, threads=%d",
		mu_models().size(), mu_refs().size(), gCompilerMode.ThreadsPerWork);
	{
		Status("MU : Models...");
		// models are independent: heavy OGF building (dedup, TB, VIPM) runs in
		// parallel; MakeProgressive uses a thread_local processor and CalculateTB
		// uses thread_local mender buffers
		xr_parallel_for(size_t(0), size_t(mu_models().size()), [&](size_t mId)
		{
			LC_AUDIT_LOG("  MU_OGF[%u]: exporting model '%s'... vertices=%zu faces=%zu subdivs=%zu",
				mId, mu_models()[mId]->m_name.c_str(), mu_models()[mId]->m_vertices.size(),
				mu_models()[mId]->m_faces.size(), mu_models()[mId]->m_subdivs.size());
			calc_ogf(*mu_models()[mId]);
		});

		// VB/IB/SWI containers are global - register serially
		for (u32 m = 0; m < mu_models().size(); m++)
		{
			export_geometry(*mu_models()[m]);
			LC_AUDIT_LOG("  MU_geom[%u]: geometry exported", m);
		}

		Status("MU : References...");
		for (u32 m = 0; m < mu_refs().size(); m++)
		{
			Progress(float(m) / float(mu_refs().size()));
			LC_AUDIT_LOG("  MU_ref[%u]: model='%s' sector=%u", m,
				mu_refs()[m]->model ? mu_refs()[m]->model->m_name.c_str() : "(null)",
				mu_refs()[m]->sector);
			export_ogf(*mu_refs()[m]);
		}
	}
	LC_AUDIT_LOG("MU OGF export done");

	//****************************************** Build sectors
	Phase("Building sectors...");
	mem_Compact();
	LC_AUDIT_LOG("BuildSectors start");
	BuildSectors();
	LC_AUDIT_LOG("BuildSectors done");

	//****************************************** Saving MISC stuff
	Phase("Saving...");
	mem_Compact();
	LC_AUDIT_LOG("Save level file to: %s", pBuild->path);
	SaveLights(*fs);

	fs->open_chunk(fsL_GLOWS);

	for (b_glow& G : glows)
	{
		fs->w(&G, 4 * sizeof(float));

		string1024 sid = {};
		xr_strconcat(sid, shader_render[materials()[G.dwMaterial].shader].name, "/", textures()[materials()[G.dwMaterial].surfidx].name);
		fs->w_u16(RegisterShader(sid));
	}
	fs->close_chunk();

	SaveTREE(*fs);
	SaveSectors(*fs);
	// Закрываем запись (а то бывает косяк что процесс закончился но файл не закрыло) 
	FS.w_close(fs);

	err_save();

	clMsg("File is Saved");
	clMsg("Compilation is Ended");

	LC_AUDIT_LOG("=== RUN-AFTER-LIGHT END ===");
}

void CBuild::err_save()
{
	string_path log_name;
	xr_strconcat(log_name, "build_", Core.UserName, ".err");
	FS.update_path(log_name, "$logs$", log_name);

	IWriter* fs = FS.w_open(log_name);
	IWriter& err = *fs;

	// t-junction
	err.open_chunk(0);
	err.w_u32(err_tjunction().size() / (1 * sizeof(Fvector)));
	err.w(err_tjunction().pointer(), err_tjunction().size());
	err.close_chunk();

	// m-edje
	err.open_chunk(1);
	err.w_u32(err_multiedge().size() / (2 * sizeof(Fvector)));
	err.w(err_multiedge().pointer(), err_multiedge().size());
	err.close_chunk();

	// invalid
	err.open_chunk(2);
	err.w_u32(err_invalid().size() / (3 * sizeof(Fvector)));
	err.w(err_invalid().pointer(), err_invalid().size());
	err.close_chunk();

	FS.w_close(fs);
}

void CBuild::MU_ModelsCalculateNormals()
{
	for (u32 m = 0; m < mu_models().size(); m++)
		calc_normals(*mu_models()[m]);
}

xr_vector<xrMU_Model*>&CBuild::mu_models()
{
	VERIFY(lc_global_data()); 
	return lc_global_data()->mu_models(); 
}

xr_vector<xrMU_Reference*>&CBuild::mu_refs()
{
	VERIFY(lc_global_data()); 
	return lc_global_data()->mu_refs(); 
}

void CBuild::ImplicitLighting()
{
	if (g_params().m_quality == ebqDraft)
		return;

	extern void ImplicitLightingExec();
	ImplicitLightingExec();
}

void CBuild::SaveLights(IWriter& fs)
{
	fs.open_chunk(fsL_LIGHT_DYNAMIC);
	for (b_light_dynamic& L : L_dynamic)
	{
		fs.w_u32(L.controller_ID);
		fs.w(&L.data, sizeof(L.data));
	}
	fs.close_chunk();
}