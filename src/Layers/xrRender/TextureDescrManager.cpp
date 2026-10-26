#include "stdafx.h"

#include "TextureDescrManager.h"
#include "ETextureParams.h"
#include "../../xrCore/xr_ini.h"

// texture metadata source:
//	0 = legacy .thm scan only
//	1 = auto (default): single textures.ltx when present, else legacy .thm scan
//	2 = hybrid: .thm scan, then textures.ltx entries override same-name entries
int						ps_r__textures_ltx	= 1;

// eye-params
float					r__dtex_range	= 50;
class cl_dt_scaler		: public R_constant_setup {
public:
	float				scale;

	cl_dt_scaler		(float s) : scale(s)	{};
	virtual void setup	(R_constant* C)
	{
		RCache.set_c	(C,scale,scale,scale,1/r__dtex_range);
	}
};

void fix_texture_thm_name(LPSTR fn)
{
	LPSTR _ext = strext(fn);
	if(  _ext					&&
	  (0==_stricmp(_ext,".tga")	||
		0==_stricmp(_ext,".thm")	||
		0==_stricmp(_ext,".dds")	||
		0==_stricmp(_ext,".bmp")	||
		0==_stricmp(_ext,".ogm")	) )
		*_ext = 0;
}

//------------------------------------------------------------------------------
// textures.ltx token tables (not reusing editor token strings on purpose)
//------------------------------------------------------------------------------
static xr_token ltx_material_token[] =
{
	{ "orennayar_blin",		STextureParams::tmOrenNayar_Blin	},
	{ "blin_phong",			STextureParams::tmBlin_Phong		},
	{ "phong_metal",		STextureParams::tmPhong_Metal		},
	{ "metal_orennayar",	STextureParams::tmMetal_OrenNayar	},
	{ "pbr",				STextureParams::tmPBR_Material		},
	{ 0,					0									}
};

static xr_token ltx_bump_mode_token[] =
{
	{ "none",				STextureParams::tbmNone				},
	{ "use",				STextureParams::tbmUse				},
	{ "parallax",			STextureParams::tbmUseParallax		},
	{ 0,					0									}
};

// id doubles as texture_assoc::usage bit mask: 1 = diffuse detail (bit0),
// 2 = bump detail (bit1), 3 = both -- same mapping LoadTHM uses for
// flDiffuseDetail / flBumpDetail
static xr_token ltx_detail_usage_token[] =
{
	{ "none",				0									},
	{ "diffuse",			1									},
	{ "bump",				2									},
	{ "diffuse_bump",		3									},
	{ 0,					0									}
};

// CInifile::r_token returns 0 for "not found", which is ambiguous with valid
// ids here -- do explicit lookup instead
static bool ltx_lookup_token(LPCSTR value, const xr_token* tokens, int& out_id)
{
	for (int i = 0; tokens[i].name; ++i)
	{
		if (0 == _stricmp(value, tokens[i].name))
		{
			out_id = tokens[i].id;
			return true;
		}
	}
	return false;
}

//------------------------------------------------------------------------------
// single-file texture metadata (textures.ltx)
//
// section name = texture name without extension (parser lowercases it, so keys
// match THM naming). Recognized lines (all optional, defaults mirror
// STextureParams::Clear):
//	material        = orennayar_blin | blin_phong | phong_metal | metal_orennayar | pbr
//	material_weight = float (default 0)
//	bump_mode       = none | use | parallax (default none)
//	bump_name       = texture path without extension
//	detail_name     = texture path without extension
//	detail_scale    = float (default 1)
//	detail_usage    = none | diffuse | bump | diffuse_bump
// Supports CInifile #include (also wildcard "#include dir\*.ltx") and DLTX
// "![" overrides, so the master file can pull in any number of same-purpose
// ltx files.
//------------------------------------------------------------------------------
bool CTextureDescrMngr::LoadLtx(LPCSTR initial)
{
	string_path			ltx_name;
	bool				have_name = false;
	if (pSettings && pSettings->section_exist("texture_descr") && pSettings->line_exist("texture_descr", "file"))
	{
		shared_str custom = pSettings->r_string_wb("texture_descr", "file");
		if (custom.size())
		{
			xr_strcpy	(ltx_name, sizeof(ltx_name), custom.c_str());
			have_name	= true;
		}
	}
	if (!have_name)
		xr_strcpy		(ltx_name, sizeof(ltx_name), "textures.ltx");

	IReader* F			= FS.r_open(initial, ltx_name);
	if (!F)
		return false;

	// include resolution needs the resolved directory: CInifile concatenates
	// it with the include name and opens via single-arg FS.r_open, which looks
	// the absolute path up in the virtual FS (works loose and packed)
	string_path			ltx_dir;
	FS.update_path		(ltx_dir, initial, "");
	CInifile* ini		= new CInifile(F, ltx_dir);

	u32					count = 0;
	for (CInifile::Sect* sect : ini->sections())
	{
		shared_str		tex_name = sect->Name;
		++count;

		texture_desc&	desc	= m_texture_details[tex_name];
		cl_dt_scaler*&	dts		= m_detail_scalers[tex_name];

		// a section fully (re)defines the entry: drop any THM-derived parts
		// (matters in hybrid mode where THM entries are loaded first)
		xr_delete		(desc.m_assoc);
		xr_delete		(desc.m_spec);
		desc.m_assoc	= nullptr;
		desc.m_spec		= nullptr;

		// ---- detail part (semantics mirror LoadTHM) ----
		if (sect->line_exist("detail_name"))
		{
			shared_str	detail_name = ini->r_string_wb(tex_name.c_str(), "detail_name");

			u8			usage = 0;
			if (sect->line_exist("detail_usage"))
			{
				int		du = 0;
				if (!ltx_lookup_token(ini->r_string(tex_name.c_str(), "detail_usage"), ltx_detail_usage_token, du))
					Msg("! [textures.ltx] [%s]: unknown detail_usage value '%s'", tex_name.c_str(), ini->r_string(tex_name.c_str(), "detail_usage"));
				usage		= u8(du);
			}

			if (*detail_name && usage)
			{
#ifndef MASTER_GOLD
				string_path src_detail_name;
				FS.update_path(src_detail_name, _game_textures_, detail_name.c_str());
				xr_strcat(src_detail_name, ".dds");
				if (!FS.exist(src_detail_name))
				{
					Msg("! Detail texture not found\n"
						"  Detail texture name: %s\n"
						"  Expected path: %s\n"
						"  Source texture: %s\n",
						detail_name.c_str(),
						src_detail_name,
						tex_name.c_str());
				}
#endif // !MASTER_GOLD

				float detail_scale = 1.0f;
				if (sect->line_exist("detail_scale"))
					detail_scale = ini->r_float(tex_name.c_str(), "detail_scale");

				desc.m_assoc		= new texture_assoc();
				desc.m_assoc->detail_name = detail_name;
				if (dts)
					dts->scale		= detail_scale;
				else
					dts				= new cl_dt_scaler(detail_scale);

				desc.m_assoc->usage	= usage;
			}
		}

		// ---- spec part (semantics mirror LoadTHM) ----
		int material = STextureParams::tmBlin_Phong;
		if (sect->line_exist("material"))
		{
			if (!ltx_lookup_token(ini->r_string(tex_name.c_str(), "material"), ltx_material_token, material))
			{
				Msg("! [textures.ltx] [%s]: unknown material value '%s'", tex_name.c_str(), ini->r_string(tex_name.c_str(), "material"));
				material = STextureParams::tmBlin_Phong;
			}
		}
		float material_weight = 0.0f;
		if (sect->line_exist("material_weight"))
			material_weight = ini->r_float(tex_name.c_str(), "material_weight");

		int bump_mode = STextureParams::tbmNone;
		if (sect->line_exist("bump_mode"))
		{
			if (!ltx_lookup_token(ini->r_string(tex_name.c_str(), "bump_mode"), ltx_bump_mode_token, bump_mode))
			{
				Msg("! [textures.ltx] [%s]: unknown bump_mode value '%s'", tex_name.c_str(), ini->r_string(tex_name.c_str(), "bump_mode"));
				bump_mode = STextureParams::tbmNone;
			}
		}

		desc.m_spec					= new texture_spec();
		desc.m_spec->m_material		= (float)material + material_weight;
		desc.m_spec->m_use_pbr		= (material == STextureParams::tmPBR_Material);
		desc.m_spec->m_use_steep_parallax = bump_mode == STextureParams::tbmUseParallax;

		if (bump_mode == STextureParams::tbmUse || bump_mode == STextureParams::tbmUseParallax)
		{
			shared_str bump_name;
			if (sect->line_exist("bump_name"))
				bump_name = ini->r_string_wb(tex_name.c_str(), "bump_name");

#ifndef MASTER_GOLD
			if (*bump_name == 0)
			{
				Msg("! Missing bumpmap reference\n"
					"  Texture: %s\n"
					"  Reason: Bumpmap name is empty\n",
					tex_name.c_str());
			}
			else
			{
				string_path pathBumpDds{}, pathBumpThm{};
				FS.update_path(pathBumpDds, _game_textures_, bump_name.c_str());
				FS.update_path(pathBumpThm, _game_textures_, bump_name.c_str());
				xr_strcat(pathBumpDds, ".dds");
				xr_strcat(pathBumpThm, ".thm");

				if (!FS.exist(pathBumpDds) && !FS.exist(pathBumpThm))
				{
					Msg("! Bumpmap texture not found\n"
						"  Bumpmap name: %s\n"
						"  Source texture: %s\n"
						"  Material requires: %s\n",
						bump_name.c_str(),
						tex_name.c_str(),
						(bump_mode == STextureParams::tbmUseParallax) ? "parallax bump" : "bump");
				}
			}
#endif // !MASTER_GOLD

			// normalize null to "" -- thm mode stores a docked empty string,
			// consumers must not see a null shared_str here
			if (*bump_name == 0)
				bump_name = "";

			desc.m_spec->m_bump_name = bump_name;
		}
	}

	Msg("textures.ltx: %d texture descriptions loaded from '%s'", count, ltx_name);

	CInifile::Destroy	(ini);
	FS.r_close(F);
	return true;
}

void CTextureDescrMngr::LoadTHM(LPCSTR initial)
{
	FS_FileSet				flist;
	FS.file_list			(flist, initial, FS_ListFiles, "*.thm");
#ifdef DEBUG
	Msg						("count of .thm files=%d", flist.size());
#endif // #ifdef DEBUG
	FS_FileSetIt It			= flist.begin();
	FS_FileSetIt It_e		= flist.end();
	STextureParams			tp;
	string_path				fn;
	for(;It!=It_e;++It)
	{
		FS.update_path(fn, initial, (*It).name.c_str());
		IReader* F = FS.r_open(fn);

		shared_str InitialPath = fn;

		xr_strcpy(fn, (*It).name.c_str());
		fix_texture_thm_name(fn);

		bool FoundedChunk = !!F->find_chunk(THM_CHUNK_TYPE);
		R_ASSERT2(FoundedChunk, "Not found chunk THM_CHUNK_TYPE");

		[[maybe_unused]] u32 ThmType = F->r_u32();
		tp.Clear();
		[[maybe_unused]] bool NeedSave = tp.Load(*F);
		FS.r_close(F);

//#ifdef _EDITOR
		if (NeedSave)
		{
			string_path		query;
			FS.update_path	(query,"$app_data_root$","fix_texture_thm\\");

			xr_string path(query);
			path.append(fn);
			path.append(".thm");

			IWriter* W = FS.w_open(path.c_str());

			W->open_chunk(THM_CHUNK_VERSION);
			W->w_u16(0x0012);
			W->close_chunk();

			W->open_chunk(THM_CHUNK_TYPE);
			W->w_u32(ThmType);
			W->close_chunk();

			tp.Save(*W);
			FS.w_close(W);
		}
//#endif

		if (STextureParams::ttImage		== tp.type ||
			STextureParams::ttTerrain	== tp.type ||
			STextureParams::ttNormalMap	== tp.type	)
		{
			texture_desc&	desc	= m_texture_details[fn];
			cl_dt_scaler*&	dts		= m_detail_scalers[fn];

			if( tp.detail_name.size() &&
				tp.flags.is_any(STextureParams::flDiffuseDetail|STextureParams::flBumpDetail) )
			{
#ifndef MASTER_GOLD
				string_path src_detail_name;
				FS.update_path(src_detail_name, _game_textures_, tp.detail_name.c_str());
				xr_strcat(src_detail_name, ".dds");
				if (!FS.exist(src_detail_name))
				{
					Msg("! Detail texture not found\n"
						"  Detail texture name: %s\n"
						"  Expected path: %s\n"
						"  Source texture: %s\n",
						tp.detail_name.c_str(),
						src_detail_name,
						fn);

				}
#endif // !MASTER_GOLD

				if(desc.m_assoc)
					xr_delete				(desc.m_assoc);

				desc.m_assoc				= new texture_assoc();
				desc.m_assoc->detail_name	= tp.detail_name;
				if (dts)
					dts->scale = tp.detail_scale;
				else
					/*desc.m_assoc->cs*/dts	= new cl_dt_scaler(tp.detail_scale);

				desc.m_assoc->usage			= 0;

				if( tp.flags.is(STextureParams::flDiffuseDetail) )
					desc.m_assoc->usage		|= (1<<0);

				if( tp.flags.is(STextureParams::flBumpDetail) )
					desc.m_assoc->usage		|= (1<<1);

			}
			if(desc.m_spec)
				xr_delete				(desc.m_spec);

			desc.m_spec					= new texture_spec();
			desc.m_spec->m_material		= (float)tp.material+tp.material_weight;
			desc.m_spec->m_use_steep_parallax = tp.bump_mode == STextureParams::tbmUseParallax;
			desc.m_spec->m_use_pbr = (tp.material == STextureParams::tmPBR_Material);

			if (tp.bump_mode == STextureParams::tbmUse || tp.bump_mode == STextureParams::tbmUseParallax)
			{
#ifndef MASTER_GOLD
				string_path pathBumpDds{}, pathBumpThm{};
				FS.update_path(pathBumpDds, _game_textures_, tp.bump_name.c_str());
				FS.update_path(pathBumpThm, _game_textures_, tp.bump_name.c_str());
				xr_strcat(pathBumpDds, ".dds");
				xr_strcat(pathBumpThm, ".thm");

				if (tp.bump_name == "")
				{
					Msg("! Missing bumpmap reference\n"
						"  Texture: %s\n"
						"  Reason: Bumpmap name is empty\n",
						(*It).name.c_str(), fn);
				}
				else if (!FS.exist(pathBumpDds) && !FS.exist(pathBumpThm))
				{
					Msg("! Bumpmap texture not found\n"
						"  Bumpmap name: %s\n"
						"  Source texture: %s\n"
						"  Material requires: %s\n",
						tp.bump_name.c_str(),
						fn,
						(tp.bump_mode == STextureParams::tbmUseParallax) ? "parallax bump" : "bump");
				}
#endif // !MASTER_GOLD

				desc.m_spec->m_bump_name = tp.bump_name;
			}
		}
	}
}

void CTextureDescrMngr::Load()
{
#ifdef DEBUG
	CTimer					TT;
	TT.Start				();
#endif // #ifdef DEBUG

	// ps_r__textures_ltx: 0 = legacy only, 1 = auto (default), 2 = hybrid
	switch (ps_r__textures_ltx)
	{
	case 0:
		LoadTHM				("$game_textures$");
		LoadTHM				("$level$");
		break;
	case 2:
		LoadTHM				("$game_textures$");
		LoadTHM				("$level$");
		LoadLtx				("$game_textures$");	// same-name entries override THM
		break;
	default:
		if (!LoadLtx		("$game_textures$"))
		{
			LoadTHM			("$game_textures$");
			LoadTHM			("$level$");
		}
		break;
	}

#ifdef DEBUG
	Msg("load time=%d ms",TT.GetElapsed_ms());
#endif // #ifdef DEBUG
}

void CTextureDescrMngr::VerifyLtxVsThm()
{
	struct snap
	{
		shared_str	name;
		shared_str	bump;
		float		material;
		BOOL		steep;
		BOOL		pbr;
		shared_str	detail;
		u8			usage;
	};

	auto take_snapshot = [](map_TD& src, xr_vector<snap>& out)
	{
		out.clear();
		for (map_TDIt it = src.begin(); it != src.end(); ++it)
		{
			snap s;
			s.name		= it->first;
			s.material	= 1.0f;
			s.steep		= FALSE;
			s.pbr		= FALSE;
			s.usage		= 0;
			if (it->second.m_spec)
			{
				s.bump		= it->second.m_spec->m_bump_name;
				s.material	= it->second.m_spec->m_material;
				s.steep		= it->second.m_spec->m_use_steep_parallax;
				s.pbr		= it->second.m_spec->m_use_pbr;
			}
			if (it->second.m_assoc)
			{
				s.detail	= it->second.m_assoc->detail_name;
				s.usage		= it->second.m_assoc->usage;
			}
			// normalize null shared_str to "" -- strcmp below must not see null
			if (!s.bump)	s.bump		= "";
			if (!s.detail)	s.detail	= "";
			out.push_back(s);
		}
	};

	auto clear_state = [this]()
	{
		for (map_TDIt it = m_texture_details.begin(); it != m_texture_details.end(); ++it)
		{
			xr_delete(it->second.m_assoc);
			xr_delete(it->second.m_spec);
		}
		m_texture_details.clear();
	};

	xr_vector<snap>		ltx_state;
	take_snapshot		(m_texture_details, ltx_state);
	const u32			ltx_count = ltx_state.size();

	// rebuild from legacy thm set
	clear_state			();
	LoadTHM				("$game_textures$");

	xr_vector<snap>		thm_state;
	take_snapshot		(m_texture_details, thm_state);
	const u32			thm_count = thm_state.size();

	xr_map<shared_str, u32> thm_index;
	for (u32 i = 0; i < thm_state.size(); ++i)
		thm_index[thm_state[i].name] = i;
	xr_map<shared_str, u32> ltx_index;
	for (u32 i = 0; i < ltx_state.size(); ++i)
		ltx_index[ltx_state[i].name] = i;

	u32 diffs = 0, only_ltx = 0, only_thm = 0;
	for (u32 i = 0; i < ltx_state.size(); ++i)
	{
		const snap& s = ltx_state[i];
		xr_map<shared_str, u32>::iterator it = thm_index.find(s.name);
		if (it == thm_index.end())
		{
			Msg("! [ltx-verify] '%s': present in textures.ltx only", s.name.c_str());
			++only_ltx;
			continue;
		}
		const snap& t = thm_state[it->second];

		bool bad = false;
		if (0 != xr_strcmp(s.bump.c_str(), t.bump.c_str()))
		{ Msg("! [ltx-verify] '%s': bump '%s' vs '%s'", s.name.c_str(), s.bump.c_str(), t.bump.c_str()); bad = true; }
		if (!fsimilar(s.material, t.material))
		{ Msg("! [ltx-verify] '%s': material %f vs %f", s.name.c_str(), s.material, t.material); bad = true; }
		if (!!s.steep != !!t.steep)
		{ Msg("! [ltx-verify] '%s': steep parallax %d vs %d", s.name.c_str(), s.steep, t.steep); bad = true; }
		if (!!s.pbr != !!t.pbr)
		{ Msg("! [ltx-verify] '%s': pbr %d vs %d", s.name.c_str(), s.pbr, t.pbr); bad = true; }
		if (0 != xr_strcmp(s.detail.c_str(), t.detail.c_str()))
		{ Msg("! [ltx-verify] '%s': detail '%s' vs '%s'", s.name.c_str(), s.detail.c_str(), t.detail.c_str()); bad = true; }
		if (s.usage != t.usage)
		{ Msg("! [ltx-verify] '%s': detail usage %d vs %d", s.name.c_str(), s.usage, t.usage); bad = true; }

		if (bad)
			++diffs;
	}
	for (u32 i = 0; i < thm_state.size(); ++i)
	{
		if (ltx_index.end() == ltx_index.find(thm_state[i].name))
		{
			const snap& t = thm_state[i];
			// a default-valued thm entry is behaviorally identical to "no
			// entry" (runtime getters return these same defaults), and the
			// converter intentionally skips such textures -- not a loss
			const bool is_default = fsimilar(t.material, 1.0f) && !t.steep && !t.pbr &&
				0 == xr_strlen(t.bump.c_str()) && 0 == xr_strlen(t.detail.c_str()) && 0 == t.usage;
			if (!is_default)
			{
				Msg("! [ltx-verify] '%s': present in .thm set only (non-default: material=%f bump='%s' detail='%s')",
					t.name.c_str(), t.material, t.bump.c_str(), t.detail.c_str());
				++only_thm;
			}
		}
	}

	Msg("[ltx-verify] ltx=%d thm=%d diffs=%d ltx_only=%d thm_only=%d", ltx_count, thm_count, diffs, only_ltx, only_thm);

	// restore LTX-derived state
	clear_state	();
	if (!LoadLtx("$game_textures$"))
		Msg("! [ltx-verify] textures.ltx not found after verify, state is thm-derived now");
}

void CTextureDescrMngr::UnLoad()
{
	map_TD::iterator I = m_texture_details.begin();
	map_TD::iterator E = m_texture_details.end();
	for(;I!=E;++I)
	{
		xr_delete(I->second.m_assoc);
		xr_delete(I->second.m_spec);
	}
	m_texture_details.clear	();
}

CTextureDescrMngr::~CTextureDescrMngr()
{
	map_CS::iterator I = m_detail_scalers.begin();
	map_CS::iterator E = m_detail_scalers.end();

	for(;I!=E;++I)
		xr_delete(I->second);

	m_detail_scalers.clear	();
}

shared_str CTextureDescrMngr::GetBumpName(const shared_str& tex_name) const
{
	map_TD::const_iterator I = m_texture_details.find	(tex_name);
	if (I!=m_texture_details.end())
	{
		if(I->second.m_spec)
		{
			return I->second.m_spec->m_bump_name;
		}
	}
	return "";
}

BOOL CTextureDescrMngr::UseSteepParallax(const shared_str& tex_name) const
{
	map_TD::const_iterator I = m_texture_details.find	(tex_name);
	if (I!=m_texture_details.end())
	{
		if(I->second.m_spec)
		{
			return I->second.m_spec->m_use_steep_parallax;
		}
	}
	return FALSE;
}

BOOL CTextureDescrMngr::UsePBRTexures(const shared_str& tex_name) const {
	auto I = m_texture_details.find(tex_name);
	if(I != m_texture_details.end()) {
		if(I->second.m_spec) {
			return I->second.m_spec->m_use_pbr;
		}
	}
	return FALSE;
}

float CTextureDescrMngr::GetMaterial(const shared_str& tex_name) const
{
	map_TD::const_iterator I = m_texture_details.find	(tex_name);
	if (I!=m_texture_details.end())
	{
		if(I->second.m_spec)
		{
			return I->second.m_spec->m_material;
		}
	}
	return 1.0f;
}

void CTextureDescrMngr::GetTextureUsage	(const shared_str& tex_name, BOOL& bDiffuse, BOOL& bBump) const
{
	map_TD::const_iterator I = m_texture_details.find	(tex_name);
	if (I!=m_texture_details.end())
	{
		if(I->second.m_assoc)
		{
			u8 usage	= I->second.m_assoc->usage;
			bDiffuse	= !!(usage & (1<<0));
			bBump		= !!(usage & (1<<1));
		}
	}
}

BOOL CTextureDescrMngr::GetDetailTexture(const shared_str& tex_name, LPCSTR& res, R_constant_setup* &CS) const
{
	map_TD::const_iterator I = m_texture_details.find	(tex_name);
	if (I!=m_texture_details.end())
	{
		if(I->second.m_assoc)
		{
            texture_assoc* TA = I->second.m_assoc;
			res	= TA->detail_name.c_str();
			map_CS::const_iterator It2 = m_detail_scalers.find(tex_name);
			CS	= It2==m_detail_scalers.end()?0:It2->second;//TA->cs;
			return TRUE;
		}
	}
	return FALSE;
}
