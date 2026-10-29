#include "stdafx.h"

#include "ETextureParams.h"
#include "../../xrCore/xr_ini.h"

xr_token					tparam_token							[ ]={
	{ "Advanced",			STextureParams::kMIPFilterAdvanced			},
																		
	{ "Point",				STextureParams::kMIPFilterPoint				},
	{ "Box",				STextureParams::kMIPFilterBox				},
	{ "Triangle",			STextureParams::kMIPFilterTriangle			},
	{ "Quadratic",			STextureParams::kMIPFilterQuadratic			},
	{ "Cubic",				STextureParams::kMIPFilterCubic				},
																		
	{ "Catrom",				STextureParams::kMIPFilterCatrom			},
	{ "Mitchell",			STextureParams::kMIPFilterMitchell			},
																		
	{ "Gaussian",			STextureParams::kMIPFilterGaussian			},
	{ "Sinc",				STextureParams::kMIPFilterSinc				},
	{ "Bessel",				STextureParams::kMIPFilterBessel			},
																		
	{ "Hanning",			STextureParams::kMIPFilterHanning			},
	{ "Hamming",			STextureParams::kMIPFilterHamming			},
	{ "Blackman",			STextureParams::kMIPFilterBlackman			},
	{ "Kaiser",				STextureParams::kMIPFilterKaiser			},
	{ 0,					0											}
};

xr_token					ttype_token								[ ]={
	{ "2D Texture",			STextureParams::ttImage						},
	{ "Cube Map",  			STextureParams::ttCubeMap					},
	{ "Bump Map",			STextureParams::ttBumpMap					},
	{ "Normal Map",			STextureParams::ttNormalMap					},
	{ "Terrain",			STextureParams::ttTerrain					},
	{ 0,					0											}
};

xr_token					tfmt_token								[ ]={
	{ "DXT1",				STextureParams::tfDXT1						},
	{ "DXT1 Alpha",			STextureParams::tfADXT1						},
	{ "DXT3",				STextureParams::tfDXT3						},
	{ "DXT5",				STextureParams::tfDXT5						},
	{ "DBC7",				STextureParams::tfBC7						},
	{ "16 bit (1:5:5:5)",	STextureParams::tf1555						},
	{ "16 bit (5:6:5)",		STextureParams::tf565						},
	{ "32 bit (8:8:8:8)",	STextureParams::tfRGBA						},
	{ "8 bit (alpha)",		STextureParams::tfA8						},
	{ "8 bit (luminance)",	STextureParams::tfL8						},
	{ "16 bit (alpha:luminance)",STextureParams::tfA8L8					},
	{ 0,					0											}
};

xr_token					tmtl_token								[ ]={
	{ "OrenNayar <-> Blin",	STextureParams::tmOrenNayar_Blin			},
	{ "Blin <-> Phong",		STextureParams::tmBlin_Phong				},
	{ "Phong <-> Metal",	STextureParams::tmPhong_Metal				},
	{ "Metal <-> OrenNayar",STextureParams::tmMetal_OrenNayar			},
	{ "PBR",                STextureParams::tmPBR_Material			    },
	{ 0,					0											}
};

xr_token					tbmode_token							[ ]={
	{ "None",				STextureParams::tbmNone						},
	{ "Use",				STextureParams::tbmUse						},
	{ "Use parallax",		STextureParams::tbmUseParallax				},
	{ 0,					0											}
};

static bool FindAndValidateChunk(IReader& F, u32 ID, bool& IncorrectChunk)
{
	u32 dwSize, dwType;
	bool success = false;
	if (F.m_last_pos != 0)
	{
		F.seek(F.m_last_pos);
		dwType = F.r_u32();
		dwSize = F.r_u32();
		if ((dwType & (~CFS_CompressMark)) == ID)
		{
			success = true;
		}
	}
	if (!success)
	{
		F.rewind();
		while (!F.eof())
		{
			dwType = F.r_u32();
			dwSize = F.r_u32();
			if ((dwType & (~CFS_CompressMark)) == ID)
			{
				success = true;
				break;
			}
			else
			{
				//if (ID == THM_CHUNK_FADE_DELAY)
				{
					const u32 pos = F.tell();
					const u32 size = F.length();
					u32 length = dwSize;
					if (pos + length != size)
					{
						bool TestSize = pos + length <= size - 8;

						if (TestSize)
						{
							F.seek(pos + length);
							TestSize = F.r_u32() == ID;
						}

						if (!TestSize)
						{
							length = 0;
							while (pos + length < size)
							{
								F.seek(pos + length);

								if (pos + length <= size - 8 && F.r_u32() == ID)
									break;

								length++;
							}
							
							Msg("! THM chunk THM_CHUNK_... fixed, wrong size = %d, correct size = %d", dwSize, length);
							IncorrectChunk = true;
						}
					}

					F.seek(pos);
					dwSize = length;
				}
				F.advance(dwSize);
			}
		}
		if (!success)
		{
			F.m_last_pos = 0;
			return 0;
		}
	}

	const u32 dwPos = F.tell();
	if (dwPos + dwSize < F.length())
	{
		F.m_last_pos = dwPos + dwSize;
	}
	else
	{
		F.m_last_pos = 0;
	}
	return dwSize;
}

bool STextureParams::Load(IReader& F)
{
	bool FoundedChunk = !!F.find_chunk(THM_CHUNK_TEXTUREPARAM);
	R_ASSERT2(FoundedChunk, "Not found chunk THM_CHUNK_TEXTUREPARAM");

	F.r(&fmt, sizeof(ETFormat));
	flags.assign(F.r_u32());
	border_color = F.r_u32();
	fade_color = F.r_u32();
	fade_amount = F.r_u32();
	mip_filter = F.r_u32();
	width = F.r_u32();
	height = F.r_u32();

	if (F.find_chunk(THM_CHUNK_TEXTURE_TYPE)) 
	{
		type = (ETType)F.r_u32();
	}

	if (F.find_chunk(THM_CHUNK_DETAIL_EXT)) 
	{
		F.r_stringZ(detail_name);
		detail_scale = F.r_float();
	}

	if (F.find_chunk(THM_CHUNK_MATERIAL)) 
	{
		material = (ETMaterial)F.r_u32();
		material_weight = F.r_float();
	}

	bool IncorrectChunk = false;
	if (FindAndValidateChunk(F, THM_CHUNK_BUMP, IncorrectChunk))
	{
		bump_virtual_height = F.r_float();
		bump_mode = (ETBumpMode)F.r_u32();

		if (bump_mode < STextureParams::tbmNone)
		{
			bump_mode = STextureParams::tbmNone; //.. временно (до полного убирания Autogen)
		}
		F.r_stringZ(bump_name);
	}

	if (FindAndValidateChunk(F, THM_CHUNK_EXT_NORMALMAP, IncorrectChunk))
		F.r_stringZ(ext_normal_map_name);

	if (FindAndValidateChunk(F, THM_CHUNK_FADE_DELAY, IncorrectChunk))
		fade_delay = F.r_u8();

	return IncorrectChunk;
}


void STextureParams::Save(IWriter& F)
{
	F.open_chunk	(THM_CHUNK_TEXTUREPARAM);
	F.w				(&fmt,sizeof(ETFormat));
	F.w_u32			(flags.get());
	F.w_u32			(border_color);
	F.w_u32			(fade_color);
	F.w_u32			(fade_amount);
	F.w_u32			(mip_filter);
	F.w_u32			(width);
	F.w_u32			(height);
	F.close_chunk	();

	F.open_chunk	(THM_CHUNK_TEXTURE_TYPE);
	F.w_u32			(type);
	F.close_chunk	();


	F.open_chunk	(THM_CHUNK_DETAIL_EXT);
	F.w_stringZ		(detail_name);
	F.w_float		(detail_scale);
	F.close_chunk	();

	F.open_chunk	(THM_CHUNK_MATERIAL);
	F.w_u32			(material);
	F.w_float		(material_weight);
	F.close_chunk	();

	F.open_chunk	(THM_CHUNK_BUMP);
	F.w_float		(bump_virtual_height);
	F.w_u32			(bump_mode);
	F.w_stringZ		(bump_name);
	F.close_chunk	();

	F.open_chunk	(THM_CHUNK_EXT_NORMALMAP);
	F.w_stringZ		(ext_normal_map_name);
	F.close_chunk	();

	F.open_chunk	(THM_CHUNK_FADE_DELAY);
	F.w_u8			(fade_delay);
	F.close_chunk	();
}


#ifdef _EDITOR
void STextureParams::OnTypeChange(PropValue* prop)
{
	switch (type){
	case ttImage:	
	case ttCubeMap:	
	break;
	case ttBumpMap:	
		flags.set			(flGenerateMipMaps,FALSE);
	break;
	case ttNormalMap:
		flags.set			(flImplicitLighted|flBinaryAlpha|flAlphaBorder|flColorBorder|flFadeToColor
							|flFadeToAlpha|flDitherColor|flDitherEachMIPLevel|flBumpDetail,FALSE);
		flags.set			(flGenerateMipMaps,TRUE);
		mip_filter			= kMIPFilterKaiser;
		fmt					= tfRGBA;
	break;
	case ttTerrain:
		flags.set			(flGenerateMipMaps|flBinaryAlpha|flAlphaBorder|flColorBorder|flFadeToColor
							|flFadeToAlpha|flDitherColor|flDitherEachMIPLevel|flBumpDetail,FALSE);
		flags.set			(flImplicitLighted,TRUE);
		fmt					= tfDXT1;
	break;
	}
	if (!OnTypeChangeEvent.empty())
		OnTypeChangeEvent(prop);
}

void STextureParams::FillProp(LPCSTR base_name, PropItemVec& items, PropValue::TOnChange on_type_change)
{                             
#if 1

	OnTypeChangeEvent	            = on_type_change;
	PropValue* P		            = PHelper().CreateToken32	(items, "Type",		(u32*)&type,		ttype_token);
	P->OnChangeEvent.bind           (this,&STextureParams::OnTypeChange);
	PHelper().CreateCaption			(items, "Source\\Width",			shared_str().printf("%d",width));
	PHelper().CreateCaption			(items, "Source\\Height",			shared_str().printf("%d",height));
	PHelper().CreateCaption			(items, "Source\\Alpha",			HasAlpha	()?"present":"absent"); 
	switch (type){
	case ttImage:	
	case ttCubeMap:	
		PHelper().CreateToken32		(items, "Format",	   				(u32*)&fmt, 		tfmt_token);

		PHelper().CreateFlag32		(items, "MipMaps\\Enabled",			&flags,				flGenerateMipMaps);
		PHelper().CreateToken32		(items, "MipMaps\\Filter",			(u32*)&mip_filter,	tparam_token);

		P = PHelper().CreateToken32	(items, "Bump\\Mode",				(u32*)&bump_mode,	tbmode_token);
		P->OnChangeEvent.bind(this,&STextureParams::OnTypeChange);
		if (tbmUse==bump_mode || tbmUseParallax==bump_mode)
		{
			AnsiString path;
			path = base_name;
			PHelper().CreateChoose	(items, "Bump\\Texture",			&bump_name,			smTexture, path.c_str());
		}
		
		PHelper().CreateFlag32		(items, "Details\\Use As Diffuse",	&flags,				flDiffuseDetail);
		PHelper().CreateFlag32		(items, "Details\\Use As Bump (R2)",&flags,				flBumpDetail);
		PHelper().CreateChoose		(items, "Details\\Texture",			&detail_name,		smTexture);
		PHelper().CreateFloat	   	(items, "Details\\Scale",			&detail_scale,		0.1f,10000.f,0.1f,2);

		PHelper().CreateToken32		(items, "Material\\Base",			(u32*)&material,	tmtl_token);
		PHelper().CreateFloat	   	(items, "Material\\Weight",			&material_weight	);
		
//		PHelper().CreateFlag32		(items, "Flags\\Binary Alpha",		&flags,				flBinaryAlpha);
		PHelper().CreateFlag32		(items, "Flags\\Dither",			&flags,				flDitherColor);
		PHelper().CreateFlag32		(items, "Flags\\Dither Each MIP",	&flags,				flDitherEachMIPLevel);
		PHelper().CreateFlag32		(items, "Flags\\Implicit Lighted",	&flags,				flImplicitLighted);

		PHelper().CreateFlag32		(items, "Fade\\Enable Color",		&flags,				flFadeToColor);
		PHelper().CreateFlag32		(items, "Fade\\Enabled Alpha",		&flags,				flFadeToAlpha);
		PHelper().CreateU8			(items,	"Fade\\Delay 'n' MIP",		&fade_delay,		0,255);
		PHelper().CreateU32			(items, "Fade\\% of color to fade in",&fade_amount,		0,100,0);
		PHelper().CreateColor	   	(items, "Fade\\Color",				&fade_color			);
		PHelper().CreateU8			(items,	"Fade\\Alpha",				((u8*)&fade_color)+3,0,255);

		PHelper().CreateFlag32		(items, "Border\\Enabled Color",	&flags,				flColorBorder);
		PHelper().CreateFlag32		(items, "Border\\Enabled Alpha",	&flags,				flAlphaBorder);
		PHelper().CreateColor	   	(items, "Border\\Color",			&border_color		);
	break;
	case ttBumpMap:	
		// PBR bump maps are compressed as-is, so format/mipmaps/material are editable
		P = PHelper().CreateToken32		(items, "Format",	   				(u32*)&fmt, 		tfmt_token);

		PHelper().CreateFlag32			(items, "MipMaps\\Enabled",			&flags,				flGenerateMipMaps);
		PHelper().CreateToken32			(items, "MipMaps\\Filter",			(u32*)&mip_filter,	tparam_token);

		PHelper().CreateChoose			(items, "Bump\\Special NormalMap",	&ext_normal_map_name,smTexture,base_name);
		PHelper().CreateFloat	   		(items, "Bump\\Virtual Height (m)",	&bump_virtual_height, 0.f, 0.1f, 0.001f, 3);

		PHelper().CreateToken32			(items, "Material\\Base",			(u32*)&material,	tmtl_token);
	break;
	case ttNormalMap:	
		P = PHelper().CreateToken32	(items, "Format",	   				(u32*)&fmt, 		tfmt_token); P->Owner()->Enable(false);

		PHelper().CreateFlag32		(items, "MipMaps\\Enabled",			&flags,				flGenerateMipMaps);
		PHelper().CreateToken32		(items, "MipMaps\\Filter",			(u32*)&mip_filter,	tparam_token);
	break;
	case ttTerrain:	
		P = PHelper().CreateToken32	(items, "Format",	   				(u32*)&fmt, 		tfmt_token); P->Owner()->Enable(false);

		PHelper().CreateFlag32		(items, "Details\\Use As Diffuse",	&flags,				flDiffuseDetail);
		PHelper().CreateFlag32		(items, "Details\\Use As Bump (R2)",&flags,				flBumpDetail);
		PHelper().CreateChoose		(items, "Details\\Texture",			&detail_name,		smTexture);
		PHelper().CreateFloat	   	(items, "Details\\Scale",			&detail_scale,		0.1f,10000.f,0.1f,2);

		PHelper().CreateToken32		(items, "Material\\Base",			(u32*)&material,	tmtl_token);
		PHelper().CreateFloat	   	(items, "Material\\Weight",			&material_weight	);

		P = PHelper().CreateFlag32	(items, "Flags\\Implicit Lighted",	&flags,				flImplicitLighted);  P->Owner()->Enable(false);
	break;
	}
#endif
}

BOOL STextureParams::similar(STextureParams& tp1, xr_vector<AnsiString>& sel_params)
{
	BOOL res 				= TRUE;
	
	xr_vector<AnsiString>::iterator it = sel_params.begin();
	xr_vector<AnsiString>::iterator it_e = sel_params.end();

	for(;it!=it_e;++it)
	{
	   const AnsiString& par_name = *it;
		if(par_name=="Type")
		{
			res = (type==tp1.type);
		}else
		if(par_name=="Source\\Width")
		{
			res = (width==tp1.width);
		}else
		if(par_name=="Source\\Height")
		{
			res = (height==tp1.height);
		}else
		if(par_name=="Source\\Alpha")
		{
			res = (HasAlpha()==tp1.HasAlpha());
		}else
		if(par_name=="Format")
		{
			res = (fmt==tp1.fmt);
		}else
		if(par_name=="MipMaps\\Enabled")
		{
			res = (flags.test(flGenerateMipMaps)==tp1.flags.test(flGenerateMipMaps));
		}else
		if(par_name=="MipMaps\\Filter")
		{
			res = (mip_filter==tp1.mip_filter);
		}else
		if(par_name=="Bump\\Mode")
		{
			res = (bump_mode==tp1.bump_mode);
		}else
		if(par_name=="Bump\\Texture")
		{
			res = (bump_name==tp1.bump_name);
		}else
		if(par_name=="Details\\Use As Diffuse")
		{           
			res = (flags.test(flDiffuseDetail)==tp1.flags.test(flDiffuseDetail));
		}else
		if(par_name=="Details\\Use As Bump (R2)")
		{
			res = (flags.test(flBumpDetail)==tp1.flags.test(flBumpDetail));
		}else
		if(par_name=="Details\\Texture")
		{
			res = (detail_name==tp1.detail_name);
		}else
		if(par_name=="Details\\Scale")
		{
			res = (fsimilar(detail_scale,tp1.detail_scale) );
		}else
		if(par_name=="Material\\Base")
		{
			res = (material==tp1.material);
		}else
		if(par_name=="Material\\Weight")
		{
			res = (fsimilar(material_weight,tp1.material_weight) );
		}else
		if(par_name=="Flags\\Binary Alpha")
		{                  
			res = (flags.test(flBinaryAlpha)==tp1.flags.test(flBinaryAlpha));
		}else
		if(par_name=="Flags\\Dither")
		{
			res = (flags.test(flDitherColor)==tp1.flags.test(flDitherColor));
		}else
		if(par_name=="Flags\\Dither Each MIP")
		{
			res = (flags.test(flDitherEachMIPLevel)==tp1.flags.test(flDitherEachMIPLevel));
		}else
		if(par_name=="Flags\\Implicit Lighted")
		{
			res = (flags.test(flImplicitLighted)==tp1.flags.test(flImplicitLighted));
		}else
		if(par_name=="Fade\\Enable Color")
		{
			res = (flags.test(flFadeToColor)==tp1.flags.test(flFadeToColor));
		}else
		if(par_name=="Fade\\Enabled Alpha")
		{
			res = (flags.test(flFadeToAlpha)==tp1.flags.test(flFadeToAlpha));
		}else
		if(par_name=="Fade\\Delay 'n' MIP")
		{
			res = (fade_delay==tp1.fade_delay);
		}else
		if(par_name=="Fade\\% of color to fade in")
		{
			res = (fade_amount==tp1.fade_amount);
		}else
		if(par_name=="Fade\\Color")
		{
			res = (fade_color==tp1.fade_color);
		}else
		if(par_name=="Fade\\Alpha")
		{
			res = (color_get_A(fade_color)==color_get_A(tp1.fade_color));
		}else
		if(par_name=="Border\\Enabled Color")
		{
			res = (flags.test(flColorBorder)==tp1.flags.test(flColorBorder));
		}else
		if(par_name=="Border\\Enabled Alpha")
		{
			res = (flags.test(flAlphaBorder)==tp1.flags.test(flAlphaBorder));
		}else
		if(par_name=="Border\\Color")
		{
			res = (border_color==tp1.border_color);
		}else
		if(par_name=="Bump\\Special NormalMap")
		{
			res = (ext_normal_map_name==tp1.ext_normal_map_name);
		}else
		if(par_name=="Bump\\Virtual Height (m)")
		{
			res = ( fsimilar(bump_virtual_height,tp1.bump_virtual_height));
		}else
			Msg("! unknown filter [%s]", par_name.c_str());
	   if(!res)
		break;
	}

	return res;
}

LPCSTR STextureParams::FormatString	()
{
	return get_token_name(tfmt_token,fmt);
}

u32 STextureParams::MemoryUsage(LPCSTR base_name)
{
	u32 mem_usage	= width*height*4;
	if (flags.test(flGenerateMipMaps))	{ mem_usage*=3ul; mem_usage/=2ul; }
	switch (fmt){
	case STextureParams::tfDXT1:
	case STextureParams::tfADXT1: 	mem_usage/=6; break;
	case STextureParams::tfDXT3:
	case STextureParams::tfDXT5: 	mem_usage/=4; break;
	case STextureParams::tf4444:
	case STextureParams::tf1555:
	case STextureParams::tf565: 	mem_usage/=2; break;
	case STextureParams::tfRGBA:	break;
	}
	string_path fn;
	FS.update_path	(fn,_game_textures_,EFS.ChangeFileExt(base_name,".seq").c_str());
	if (FS.exist(fn))
	{
		string128		buffer;
		IReader* F		= FS.r_open(0,fn);
		F->r_string		(buffer,sizeof(buffer));
		int cnt = 0;
		while (!F->eof()){
			F->r_string(buffer,sizeof(buffer));
			cnt++;
		}
		FS.r_close		(F);
		mem_usage *= cnt?cnt:1;
	}
	return mem_usage;
}

// Converts per-texture .thm metadata from $game_textures$ into a single
// textures_converted.ltx consumable by CTextureDescrMngr::LoadLtx (runtime
// reads textures.ltx, see TextureDescrManager.cpp). Only textures with
// non-default parameters and types the runtime registers are emitted.
// Output goes to a NEW file on purpose: review it, then rename to textures.ltx.

// CInifile::_parse collapses all whitespace outside double quotes, so any
// value containing spaces (yes, some thm bump names have stray spaces) must
// be written quoted
static void LtxWriteNameValue(IWriter* W, LPCSTR key, LPCSTR value)
{
	string512 line;
	if (strchr(value, ' ') || strchr(value, '\t'))
		xr_strconcat(line, key, "       = \"", value, "\"");
	else
		xr_strconcat(line, key, "       = ", value);
	W->w_string(line);
}

ECORE_API void ConvertThmToLtx()
{
	FS_FileSet				files;
	FS.file_list			(files, "$game_textures$", FS_ListFiles, "*.thm");

	IWriter* W = FS.w_open("$game_textures$", "textures_converted.ltx");
	if (!W)
	{
		Msg("! ConvertThmToLtx: cannot create $game_textures$\\textures_converted.ltx");
		return;
	}

	W->w_string("; texture metadata converted from .thm files");
	W->w_string("; review, then rename to textures.ltx (runtime default name)");
	W->w_string("; runtime defaults (not written): material=blin_phong, material_weight=0, bump_mode=none, no detail");

	u32 total = files.size(), written = 0;
	string_path fn;
	for (const FS_File& f : files)
	{
		FS.update_path		(fn, "$game_textures$", f.name.c_str());
		IReader* F = FS.r_open(fn);
		if (!F)
			continue;

		char name[256];
		xr_strcpy			(name, f.name.c_str());
		if (LPSTR ext = strext(name))
			*ext = 0;
		_strlwr				(name);

		STextureParams		tp;
		tp.Clear			();
		// THM_CHUNK_TYPE holds the thumbnail class (LoadTHM reads it only as a
		// sanity check and ignores the value); the texture type lives in
		// THM_CHUNK_TEXTURE_TYPE, parsed by STextureParams::Load into tp.type
		bool ok = !!F->find_chunk(THM_CHUNK_TYPE);
		if (ok)
		{
			[[maybe_unused]] u32 thm_class = F->r_u32();
			tp.Load			(*F);
		}
		FS.r_close			(F);

		// mirror LoadTHM: only these types produce runtime entries
		if (!ok || (tp.type != STextureParams::ttImage &&
					tp.type != STextureParams::ttTerrain &&
					tp.type != STextureParams::ttNormalMap))
			continue;

		bool has_material = tp.material != STextureParams::tmBlin_Phong || tp.material_weight != 0.0f;
		bool has_bump = (tp.bump_mode == STextureParams::tbmUse || tp.bump_mode == STextureParams::tbmUseParallax);
		bool has_detail = tp.detail_name.size() &&
			tp.flags.is_any(STextureParams::flDiffuseDetail | STextureParams::flBumpDetail);

		if (!has_material && !has_bump && !has_detail)
			continue;

		string512 line;

		W->w_string			("");
		xr_strconcat		(line, "[", name, "]");
		W->w_string			(line);

		if (has_material)
		{
			LPCSTR mat_name = "blin_phong";
			switch (tp.material)
			{
			case STextureParams::tmOrenNayar_Blin:	mat_name = "orennayar_blin";	break;
			case STextureParams::tmBlin_Phong:		mat_name = "blin_phong";		break;
			case STextureParams::tmPhong_Metal:		mat_name = "phong_metal";		break;
			case STextureParams::tmMetal_OrenNayar:	mat_name = "metal_orennayar";	break;
			case STextureParams::tmPBR_Material:	mat_name = "pbr";				break;
			}
			xr_strconcat		(line, "material        = ", mat_name);
			W->w_string			(line);
			if (tp.material_weight != 0.0f)
			{
				xr_sprintf		(line, sizeof(line), "material_weight = %g", tp.material_weight);
				W->w_string		(line);
			}
		}

		if (has_bump)
		{
			// write bump_mode even when the name is empty: thm mode keeps
			// steep-parallax state for such entries, conversion must be faithful
			xr_strconcat	(line, "bump_mode       = ", tp.bump_mode == STextureParams::tbmUseParallax ? "parallax" : "use");
			W->w_string		(line);
			if (tp.bump_name.size())
				LtxWriteNameValue(W, "bump_name", tp.bump_name.c_str());
			else
				Msg("! ConvertThmToLtx: '%s' has bump_mode but empty bump_name (kept as-is, runtime will warn)", name);
		}

		if (has_detail)
		{
			const bool bDiffuse = tp.flags.is(STextureParams::flDiffuseDetail);
			const bool bBump	= tp.flags.is(STextureParams::flBumpDetail);
			xr_strconcat		(line, "detail_name     = ", tp.detail_name.c_str());
			W->w_string			(line);
			xr_strconcat		(line, "detail_usage    = ", (bDiffuse && bBump) ? "diffuse_bump" : (bBump ? "bump" : "diffuse"));
			W->w_string			(line);
			if (!fsimilar(tp.detail_scale, 1.0f))
			{
				xr_sprintf		(line, sizeof(line), "detail_scale    = %g", tp.detail_scale);
				W->w_string		(line);
			}
		}

		// editor-only fields: kept for lossless migration, runtime LoadLtx
		// safely ignores them
		if (!fsimilar(tp.bump_virtual_height, 0.05f))
		{
			xr_sprintf			(line, sizeof(line), "virtual_height   = %g", tp.bump_virtual_height);
			W->w_string			(line);
		}
		if (tp.ext_normal_map_name.size())
		{
			xr_strconcat		(line, "ext_normal_map   = ", tp.ext_normal_map_name.c_str());
			W->w_string			(line);
		}

		++written;
	}

	FS.w_close				(W);
	Msg("ConvertThmToLtx: scanned %d thm files, wrote %d sections to $game_textures$\\textures_converted.ltx", total, written);
}

// ---- reverse conversion (Batch LTX -> THM) --------------------------------
// same value as EThumbnailTexture.cpp (not exposed in a header)
#define THM_TEXTURE_VERSION 0x0012

// token tables mirror the runtime textures.ltx vocabulary (see
// CTextureDescrMngr::LoadLtx); duplicated here because the runtime tables
// are file-local to TextureDescrManager.cpp
static xr_token ltx2thm_material_token[] =
{
	{ "orennayar_blin",		STextureParams::tmOrenNayar_Blin	},
	{ "blin_phong",			STextureParams::tmBlin_Phong		},
	{ "phong_metal",		STextureParams::tmPhong_Metal		},
	{ "metal_orennayar",	STextureParams::tmMetal_OrenNayar	},
	{ "pbr",				STextureParams::tmPBR_Material		},
	{ 0,					0									}
};

static xr_token ltx2thm_bump_mode_token[] =
{
	{ "none",				STextureParams::tbmNone				},
	{ "use",				STextureParams::tbmUse				},
	{ "parallax",			STextureParams::tbmUseParallax		},
	{ 0,					0									}
};

static xr_token ltx2thm_detail_usage_token[] =
{
	{ "none",				0									},
	{ "diffuse",			1									},
	{ "bump",				2									},
	{ "diffuse_bump",		3									},
	{ 0,					0									}
};

static bool ltx2thm_lookup_token(LPCSTR value, const xr_token* tokens, int& out_id)
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

ECORE_API void ConvertLtxToThm()
{
	// file name comes from system.ltx [texture_descr]/file, default matches
	// what ConvertThmToLtx writes
	shared_str ltx_name = "textures_converted.ltx";
	if (pSettings && pSettings->section_exist("texture_descr") && pSettings->line_exist("texture_descr", "file"))
	{
		shared_str custom = pSettings->r_string_wb("texture_descr", "file");
		if (custom.size())
			ltx_name = custom;
	}

	IReader* F = FS.r_open("$game_textures$", ltx_name.c_str());
	if (!F)
	{
		Msg("! ConvertLtxToThm: cannot open $game_textures$\\%s", ltx_name.c_str());
		return;
	}

	string_path ltx_dir;
	FS.update_path(ltx_dir, "$game_textures$", "");
	CInifile* ini = new CInifile(F, ltx_dir);

	u32 matched = 0, updated = 0, skipped = 0;
	for (CInifile::Sect* sect : ini->sections())
	{
		string_path thm_path;
		FS.update_path(thm_path, "$game_textures$", sect->Name.c_str());
		xr_strcat(thm_path, sizeof(thm_path), ".thm");

		IReader* T = FS.r_open(thm_path);
		if (!T)
		{
			Msg("! ConvertLtxToThm: [%s] has no matching .thm file, skipped", sect->Name.c_str());
			++skipped;
			continue;
		}

		u16 version = 0;
		bool ok = !!T->r_chunk(THM_CHUNK_VERSION, &version) && version == THM_TEXTURE_VERSION;
		u32 thm_type = 0;
		if (ok)
			ok = !!T->find_chunk(THM_CHUNK_TYPE);
		if (ok)
			thm_type = T->r_u32();
		if (ok)
			ok = !!T->find_chunk(THM_CHUNK_TEXTUREPARAM);
		if (!ok)
		{
			FS.r_close(T);
			Msg("! ConvertLtxToThm: '%s' is unsupported or corrupt, skipped", thm_path);
			++skipped;
			continue;
		}

		STextureParams tp;
		tp.Clear();
		tp.Load(*T);
		FS.r_close(T);

		// only the lines actually present in the section are applied;
		// everything the section omits keeps its current thm value
		bool touched = false;
		if (sect->line_exist("material"))
		{
			LPCSTR value = ini->r_string(sect->Name.c_str(), "material");
			int mat = 0;
			if (ltx2thm_lookup_token(value, ltx2thm_material_token, mat))
			{
				tp.material = (STextureParams::ETMaterial)mat;
				touched = true;
			}
			else
				Msg("! ConvertLtxToThm: [%s] unknown material value '%s'", sect->Name.c_str(), value);
		}
		if (sect->line_exist("material_weight"))
		{
			tp.material_weight = ini->r_float(sect->Name.c_str(), "material_weight");
			touched = true;
		}
		if (sect->line_exist("bump_mode"))
		{
			LPCSTR value = ini->r_string(sect->Name.c_str(), "bump_mode");
			int bm = 0;
			if (ltx2thm_lookup_token(value, ltx2thm_bump_mode_token, bm))
			{
				tp.bump_mode = (STextureParams::ETBumpMode)bm;
				touched = true;
			}
			else
				Msg("! ConvertLtxToThm: [%s] unknown bump_mode value '%s'", sect->Name.c_str(), value);
		}
		if (sect->line_exist("bump_name"))
		{
			tp.bump_name = ini->r_string_wb(sect->Name.c_str(), "bump_name");
			touched = true;
		}
		if (sect->line_exist("detail_name"))
		{
			tp.detail_name = ini->r_string_wb(sect->Name.c_str(), "detail_name");
			touched = true;
			if (!sect->line_exist("detail_usage"))
				Msg("! ConvertLtxToThm: [%s] has detail_name without detail_usage, detail flags left unchanged", sect->Name.c_str());
		}
		if (sect->line_exist("detail_usage"))
		{
			LPCSTR value = ini->r_string(sect->Name.c_str(), "detail_usage");
			int du = 0;
			if (ltx2thm_lookup_token(value, ltx2thm_detail_usage_token, du))
			{
				tp.flags.set(STextureParams::flDiffuseDetail, !!(du & 1));
				tp.flags.set(STextureParams::flBumpDetail, !!(du & 2));
				touched = true;
			}
			else
				Msg("! ConvertLtxToThm: [%s] unknown detail_usage value '%s'", sect->Name.c_str(), value);
		}
		if (sect->line_exist("detail_scale"))
		{
			tp.detail_scale = ini->r_float(sect->Name.c_str(), "detail_scale");
			touched = true;
		}
		if (sect->line_exist("virtual_height"))
		{
			tp.bump_virtual_height = ini->r_float(sect->Name.c_str(), "virtual_height");
			touched = true;
		}
		if (sect->line_exist("ext_normal_map"))
		{
			tp.ext_normal_map_name = ini->r_string_wb(sect->Name.c_str(), "ext_normal_map");
			touched = true;
		}

		if (!touched)
		{
			++matched;	// matched but contained no known keys, nothing to write
			continue;
		}

		// write back using the same layout ETextureThumbnail::Save produces
		CMemoryWriter W;
		W.open_chunk(THM_CHUNK_VERSION);
		W.w_u16(THM_TEXTURE_VERSION);
		W.close_chunk();
		W.open_chunk(THM_CHUNK_TYPE);
		W.w_u32(thm_type);
		W.close_chunk();
		tp.Save(W);

		if (W.save_to(thm_path))
		{
			++matched;
			++updated;
			Msg("ConvertLtxToThm: [%s] updated", sect->Name.c_str());
		}
		else
		{
			Msg("! ConvertLtxToThm: cannot write '%s'", thm_path);
			++skipped;
		}
	}

	CInifile::Destroy	(ini);
	FS.r_close			(F);
	Msg("ConvertLtxToThm: %d sections matched (%d thm updated), %d skipped", matched, updated, skipped);
}
#endif
