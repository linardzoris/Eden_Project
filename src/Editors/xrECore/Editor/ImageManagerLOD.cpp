#include "stdafx.h"


#include "ImageManager.h"
#include "EditObject.h"
#include "EditMesh.h"
#include "ui_main.h"
#include "../xrEngine/xrHemisphere.h"

using Fvector4Vec = xr_vector<Fvector4>;
using Fvector4It = Fvector4Vec::iterator;

BOOL GetPointColor(SPickQuery::SResult* R, u32& alpha, u32& color)
{
    CSurface* surf			= R->e_mesh->GetSurfaceByFaceID(R->tag); VERIFY(surf);
    Shader_xrLC* c_sh		= EDevice->ShaderXRLC.Get(surf->_ShaderXRLCName());
    if (!c_sh->flags.bRendering) return FALSE;
    const Fvector2*			cuv[3];
    R->e_mesh->GetFaceTC	(R->tag,cuv);

    // barycentric coords
    // note: W,U,V order
    Fvector B;
    B.set	(1.0f - R->u - R->v,R->u,R->v);

    // calc UV
    Fvector2	uv;
    uv.x = cuv[0]->x*B.x + cuv[1]->x*B.y + cuv[2]->x*B.z;
    uv.y = cuv[0]->y*B.x + cuv[1]->y*B.y + cuv[2]->y*B.z;

    int U = iFloor(uv.x*float(surf->m_ImageData->w) + .5f);
    int V = iFloor(uv.y*float(surf->m_ImageData->h)+ .5f);

    if (surf->m_ImageData->w != 0)
    {
        U %= surf->m_ImageData->w;
        if (U < 0)
            U += surf->m_ImageData->w;
    }

    if (surf->m_ImageData->h != 0)
    {
        V %= surf->m_ImageData->h;
        if (V < 0)
            V += surf->m_ImageData->h;
    }


    auto& Layer = surf->m_ImageData->layers;

    if (!Layer.empty() && !Layer.back().empty())
    {
        color = Layer.back()[V * surf->m_ImageData->w + U];
        alpha = color_get_A(color);
    }
    else
    {
        Msg("! Error creating lod. Try again!");
        return false;
    }

    return TRUE;
}

struct SBuildLight{
    Flight					light;
    float					energy;
};
using BLVec = xr_vector<SBuildLight>;
using BLIt = BLVec::iterator;

ICF static void  simple_hemi_callback(float x, float y, float z, float E, LPVOID P)
{
    BLVec* dst 					= (BLVec*)P;
    SBuildLight 				T;
    T.energy     				= E;
    T.light.direction.set		(x,y,z);
    dst->push_back  			(T);
}

const u32 lod_ss_quality  = 8;

// 8 向正交捕捉，写入图集：横排为 8 个水平条带，竖排为 8 个垂直条带
void CreateLODSamples(const Fbox& bbox, U32Vec& tgt_data, u32 cell_w, u32 cell_h, bool vertical, u32 atlas_w, u32 atlas_h)
{
	U32Vec 	s_pixels,d_pixels;
    Fmatrix save_projection		= EDevice->mProject;
    Fmatrix save_view			= EDevice->mView;

    // save render params
    Flags32 old_flag			= psDeviceFlags;
	u32 old_dwFillMode			= EDevice->dwFillMode;
    u32 old_dwShadeMode			= EDevice->dwShadeMode;
    // set render params

    u32 cc						= 	EPrefs->scene_clear_color;
    EPrefs->scene_clear_color 	= 	0x0000000;
    psDeviceFlags.zero			();
    psDeviceFlags.set			(rsFilterLinear,TRUE);
	EDevice->dwFillMode			= D3DFILL_SOLID;
    EDevice->dwShadeMode			= D3DSHADE_GOURAUD;

    Fvector vP,vD,vN,vR;
    Fmatrix mV,mP;

    Fvector S;
    bbox.getradius				(S);
    float R 					= 2.f*_max(S.x,S.z);

    u32 pitch 					= atlas_w;
	tgt_data.resize				(pitch*atlas_h);
	for (int frame=0; frame<LOD_SAMPLE_COUNT; frame++){
        float angle 			= frame*(PI_MUL_2/LOD_SAMPLE_COUNT);

		Fbox bb 				= bbox;
        // build camera matrix
        bb.getcenter			( vP );
        vN.set					( 0.f,1.f,0.f );
        vD.setHP				( angle, 0 );
        vR.crossproduct			( vN,vD );
        mV.build_camera_dir		(vP,vD,vN);
        bb.xform				(mV);
        // build project matrix
        mP.build_projection_ortho(R,bb.max.y-bb.min.y,bb.min.z,bb.max.z);
	    RCache.set_xform_project(mP);
	    RCache.set_xform_view	(mV);
        EDevice->mFullTransform.mul(mP,mV);
        EDevice->MakeScreenshot	(s_pixels,cell_w*lod_ss_quality,cell_h*lod_ss_quality);
        d_pixels.resize 		(cell_w*cell_h);
        DXTUtils::Filter::Process				(d_pixels.data(),cell_w,cell_h,s_pixels.data(),cell_w*lod_ss_quality,cell_h*lod_ss_quality, DXTUtils::Filter::imf_box);
        // copy LOD cell to final atlas
        u32 dst 				= vertical ? frame*cell_h*pitch : frame*cell_w;
		for (u32 y=0; y<cell_h; y++)
    		CopyMemory			(tgt_data.data()+y*pitch+dst,d_pixels.data()+y*cell_w,cell_w*sizeof(u32));
	}

    // flip data
	for (u32 y=0; y<atlas_h/2; y++){
		u32 y2 = atlas_h-y-1;
		for (u32 x=0; x<pitch; x++){
        	std::swap	(tgt_data[y*pitch+x],tgt_data[y2*pitch+x]);
		}
	}

    // diagnostic: how much the GPU capture actually contained (all-zero = broken render target state)
    u32 cap_nonzero = 0;
    for (u32 i = 0; i < tgt_data.size(); i++)
        if (tgt_data[i] != 0) cap_nonzero++;
    Msg("LOD capture: %u / %u non-empty pixels", cap_nonzero, (u32)tgt_data.size());

    // restore render params
	EDevice->dwFillMode			= old_dwFillMode;
    EDevice->dwShadeMode			= old_dwShadeMode;
    psDeviceFlags 				= old_flag;
    EPrefs->scene_clear_color 	= cc;

    RCache.set_xform_view		(save_view);
    RCache.set_xform_project	(save_projection);
}

void CImageManager::CreateLODTexture(CEditableObject* OBJECT, U32Vec& lod_pixels, U32Vec& nm_pixels, u32 cell_w, u32 cell_h, bool vertical, int quality, u32 nm_downscale)
{
    u32 atlas_w = vertical ? cell_w : cell_w*LOD_SAMPLE_COUNT;
    u32 atlas_h = vertical ? cell_h*LOD_SAMPLE_COUNT : cell_h;

    // normal map grid, optionally downscaled relative to the color atlas
    if (nm_downscale < 1) nm_downscale = 1;
    u32 nm_cw = _max(1u, cell_w / nm_downscale);
    u32 nm_ch = _max(1u, cell_h / nm_downscale);
    u32 nm_atlas_w = vertical ? nm_cw : nm_cw*LOD_SAMPLE_COUNT;
    u32 nm_atlas_h = vertical ? nm_ch*LOD_SAMPLE_COUNT : nm_ch;

    // build hemi light
    BLVec						simple_hemi;
    // fill simple hemi
    simple_hemi.clear();
    xrHemisphereBuild(1, 2.f, simple_hemi_callback, &simple_hemi);

    Fbox bb = OBJECT->GetBox();

    // build lod normals
    lod_pixels.resize(atlas_w*atlas_h, 0);
    nm_pixels.resize(nm_atlas_w*nm_atlas_h, 0);
    U32Vec hemi_tmp(nm_atlas_w*nm_atlas_h, 0);
    Fvector 					o_center, o_size;
    Fmatrix 					M, Mi;
    bb.getradius(o_size);
    bb.getcenter(o_center);
    SPBItem* PB = UI->ProgressStart(LOD_SAMPLE_COUNT, OBJECT->GetName());
    float viewW = 2.f*_max(o_size.x, o_size.z);		// 与捕捉正交窗口一致（getradius 为半尺寸）
    float viewH = 2.f*o_size.y;
    float nm_dW = viewW / nm_cw;
    float nm_dH = viewH / nm_ch;
    float dR = bb.getradius();
    float d2R = dR * 2.f;

    CreateLODSamples(bb, lod_pixels, cell_w, cell_h, vertical, atlas_w, atlas_h);

    float 	LOD_CALC_SAMPLES = quality;
    s32		LOD_CALC_SAMPLES_LIM = LOD_CALC_SAMPLES / 2;

    // preload textures
    for (SurfaceIt surf_it = OBJECT->Surfaces().begin(); surf_it != OBJECT->Surfaces().end(); surf_it++) {
        CSurface* surf = *surf_it;
        Shader_xrLC* c_sh = EDevice->ShaderXRLC.Get(surf->_ShaderXRLCName());
        if (!c_sh->flags.bRendering) continue;
        if (0 == surf->m_ImageData)surf->CreateImageData();
    }

    // pre-build collision models: workers must never trigger lazy GenerateCFModel concurrently
    for (EditMeshIt mesh_it = OBJECT->Meshes().begin(); mesh_it != OBJECT->Meshes().end(); mesh_it++)
        (*mesh_it)->EnsureCFModel();

    CTimer TTotal;
    TTotal.Start();

    // calculate
    for (u32 sample_idx = 0; sample_idx < LOD_SAMPLE_COUNT; sample_idx++)
    {
        float angle = sample_idx * (PI_MUL_2 / LOD_SAMPLE_COUNT);
        M.setXYZ(0, angle, 0);
        M.translate_over(o_center);
        Mi.invert(M);

        // rows are independent - process them in parallel
        // (each thread owns its collider/query buffers; shared object data is read-only here)
        xr_parallel_for(u32(0), nm_ch, [&](u32 iH)
        {
            // per-thread, reused across rows and directions
            static thread_local CDB::COLLIDER	CL;
            static thread_local SPickQuery		PQ;
            static thread_local FvectorVec		n_vec;
            static thread_local Fvector4Vec		sample_pt_vec;

            // NOTE: cast to float BEFORE subtraction - unsigned math underflowed here
            // and threw most of the grid to ~4e9 (rays missed -> nm content cropped to a quarter)
            float Y = (float(iH) - float((nm_ch - 1) / 2)) * nm_dH;
            for (u32 iW = 0; iW < nm_cw; iW++)
            {
                float X = (float(iW) - float(nm_cw / 2)) * nm_dW;

                u32 row = vertical ? sample_idx*nm_ch + iH : iH;
                u32 col = vertical ? iW : sample_idx*nm_cw + iW;
                u32 pixel = (nm_atlas_h - row - 1) * nm_atlas_w + col;
                u32& tgt_n = nm_pixels[pixel];
                u32& tgt_h = hemi_tmp[pixel];

                n_vec.clear();
                sample_pt_vec.clear();
                for (s32 iiH = -LOD_CALC_SAMPLES_LIM; iiH <= LOD_CALC_SAMPLES_LIM; iiH++)
                {
                    float dY = iiH * (nm_dH / LOD_CALC_SAMPLES);
                    for (s32 iiW = -LOD_CALC_SAMPLES_LIM; iiW <= LOD_CALC_SAMPLES_LIM; iiW++)
                    {
                        float dX = iiW * (nm_dW / LOD_CALC_SAMPLES);
                        Fvector start;
                        start.set(X + dX, Y + dY, 0);
                        M.transform_tiny(start);
                        start.mad(M.k, -dR);
                        PQ.prepare_rq(start, M.k, d2R, CDB::OPT_CULL);
                        OBJECT->RayQuery(CL, PQ);

                        if (PQ.r_count())
                        {
                            PQ.r_sort();
                            Fvector N = { 0,0,0 };

                            for (s32 k = PQ.r_count() - 1; k >= 0; k--)
                            {
                                SPickQuery::SResult* R = PQ.r_begin() + k;
                                u32 	uA, uC;
                                if (!GetPointColor(R, uA, uC)) continue;
                                float	fA = float(uA) / 255.f;
                                if (uA)
                                {
                                    Fvector 	pt; 	pt.mad(PQ.m_Start, PQ.m_Direction, R->range - EPS_L);
                                    Fvector4 ptt;	ptt.set(pt.x, pt.y, pt.z, fA);
                                    sample_pt_vec.push_back(ptt);
                                }
                                // normal
                                Fvector Nn;
                                Nn.mknormal(R->verts[0], R->verts[1], R->verts[2]);
                                Nn.mul(fA);

                                N.mul(1.f - fA);
                                N.add(Nn);
                            }

                            float n_mag = N.magnitude();
                            if (!fis_zero(n_mag, EPS))
                                n_vec.push_back(N.div(n_mag));
                        }
                    }
                }

                Fvector N = { 0,0,0 };
                if (!n_vec.empty())
                {
                    for (FvectorIt it = n_vec.begin(); it != n_vec.end(); it++) N.add(*it);
                    N.div(n_vec.size());
                    N.normalize_safe();
                    Mi.transform_dir(N);
                }

                N.mul(0.5f);
                N.add(0.5f);
                N.mul(255.f);
                // alpha is transient here, the hemi value replaces it after the loop
                tgt_n = color_rgba(iFloor(N.x), iFloor(N.y), iFloor(N.z), 255);

                // hemi/AO only where the rays actually hit the object
                if (sample_pt_vec.empty())
                    continue;

                // hemi/AO: evaluate once per pixel at the most opaque hit point instead of
                // every sub-ray hit (AO is low-frequency; this removes the 25x ray explosion)
                const Fvector4* best_pt = 0;
                float best_w = 0.f;
                for (Fvector4It pt_it = sample_pt_vec.begin(); pt_it != sample_pt_vec.end(); pt_it++)
                {
                    if (pt_it->w > best_w)
                    {
                        best_w = pt_it->w;
                        best_pt = &*pt_it;
                    }
                }

                float res_transp = 0.f;
                if (best_pt)
                {
                    float avg_transp = 0.f;
                    for (BLIt it = simple_hemi.begin(); it != simple_hemi.end(); it++)
                    {
                        Fvector start;
                        start.mad(Fvector().set(best_pt->x, best_pt->y, best_pt->z), it->light.direction, -dR);
                        PQ.prepare_rq(start, it->light.direction, dR, CDB::OPT_CULL);
                        OBJECT->RayQuery(CL, PQ);
                        float ray_transp = 1.f;

                        if (PQ.r_count())
                        {
                            for (s32 k = 0; k < PQ.r_count(); k++)
                            {
                                u32 	a, uC;
                                if (!GetPointColor(PQ.r_begin() + k, a, uC)) continue;
                                ray_transp *= (1.f - float(a) / 255.f);
                                if (fis_zero(ray_transp, EPS_L)) break;
                            }
                        }
                        avg_transp += ray_transp;
                    }
                    avg_transp /= simple_hemi.size();
                    res_transp = res_transp * (1.f - best_w) + avg_transp * best_w;
                }
                u8 h = (u8)iFloor(res_transp * 255.f);
                tgt_h = color_rgba(h, h, h, 255);
            }
        });
        PB->Inc();
    }

    Msg("LOD pixels %u: %3.2fsec", atlas_w*atlas_h, TTotal.GetElapsed_sec());
    ImageLib.ApplyBorders(lod_pixels, atlas_w, atlas_h);
    ImageLib.ApplyBorders(nm_pixels, nm_atlas_w, nm_atlas_h);
    ImageLib.ApplyBorders(hemi_tmp, nm_atlas_w, nm_atlas_h);

    // fill alpha to N-channel (HEMI)
    for (int px_idx = 0; px_idx<int(nm_pixels.size()); px_idx++)
        nm_pixels[px_idx] = subst_alpha(nm_pixels[px_idx], color_get_R(hemi_tmp[px_idx]));

    UI->ProgressEnd(PB);
}

//------------------------------------------------------------------------------
//
//------------------------------------------------------------------------------
void CImageManager::CreateLODTexture(CEditableObject* OBJECT, LPCSTR tex_name, u32 tgt_w, u32 tgt_h, int samples, time_t age, int quality)
{
    U32Vec lod_pixels, nm_pixels;

    CreateLODTexture(OBJECT, lod_pixels, nm_pixels, tgt_w, tgt_h, false, quality);

    string_path out_name, src_name;
    // save lod
    strcpy(src_name, tex_name);

    strcpy(src_name, EFS.ChangeFileExt(src_name, ".thm").c_str());
    FS.file_delete(_textures_, src_name);

    strcpy(src_name, EFS.ChangeFileExt(src_name, ".tga").c_str());
    FS.update_path(out_name, _textures_, src_name);
    DXTUtils::Converter::MakeTGA(tgt_w * LOD_SAMPLE_COUNT, tgt_h, lod_pixels, out_name);
    FS.set_file_age(out_name, age);
    SynchronizeTexture(src_name, age);

    // save normal map
    xr_strconcat(src_name, tex_name, "_nm");
    strcpy(src_name, EFS.ChangeFileExt(src_name, ".thm").c_str());
    FS.file_delete(_textures_, src_name);

    strcpy(src_name, EFS.ChangeFileExt(src_name, ".tga").c_str());
    FS.update_path(out_name, _textures_, src_name);
    DXTUtils::Converter::MakeTGA(tgt_w * LOD_SAMPLE_COUNT, tgt_h, nm_pixels, out_name);
    FS.set_file_age(out_name, age);
    SynchronizeTexture(src_name, age);

    // game dds: FLOD is a standalone texture now (color DXT3, normal DXT5 with AO in alpha)
    STextureParams tp;
    tp.width			= tgt_w * LOD_SAMPLE_COUNT;
    tp.height			= tgt_h;
    tp.type				= STextureParams::ttImage;
    tp.mip_filter		= STextureParams::kMIPFilterAdvanced;
    tp.flags.assign		(STextureParams::flDitherColor | STextureParams::flGenerateMipMaps | STextureParams::flHasAlpha);

    tp.fmt				= STextureParams::tfDXT3;
    strcpy(src_name, EFS.ChangeFileExt(tex_name, ".dds").c_str());
    FS.update_path(out_name, _game_textures_, src_name);
    MakeGameTexture(out_name, lod_pixels.data(), tp);

    ETextureThumbnail* THM = new ETextureThumbnail(tex_name, false);
    THM->m_TexParams	= tp;
    THM->SetValid	();
    THM->Save		(age);
    xr_delete		(THM);

    tp.fmt				= STextureParams::tfDXT5;
    xr_strconcat(src_name, tex_name, "_nm");
    strcpy(src_name, EFS.ChangeFileExt(src_name, ".dds").c_str());
    FS.update_path(out_name, _game_textures_, src_name);
    MakeGameTexture(out_name, nm_pixels.data(), tp);

    xr_strconcat(src_name, tex_name, "_nm");
    THM = new ETextureThumbnail(src_name, false);
    THM->m_TexParams	= tp;
    THM->SetValid	();
    THM->Save		(age);
    xr_delete		(THM);
}

//------------------------------------------------------------------------------
// HQ billboard：按物体比例生成高分辨率 8 向图集，直接输出 tga/thm/dds 到 _eden\_shared\billboards
// 色彩图无后缀（DXT3，alpha=透明度），法线图带 _nm 后缀（DXT5，alpha=AO），分辨率为色彩的 1/2
//------------------------------------------------------------------------------
void CImageManager::CreateBillboardTexture(CEditableObject* OBJECT, LPCSTR tex_name, time_t age, int quality)
{
    u32 cell_w, cell_h, atlas_w, atlas_h;
    bool vertical;
    OBJECT->GetLODAtlasParams(cell_w, cell_h, vertical, atlas_w, atlas_h);
    // normal map atlas is half the color resolution per axis
    u32 nm_cw = _max(1u, cell_w / 2);
    u32 nm_ch = _max(1u, cell_h / 2);
    u32 nm_atlas_w = vertical ? nm_cw : nm_cw*LOD_SAMPLE_COUNT;
    u32 nm_atlas_h = vertical ? nm_ch*LOD_SAMPLE_COUNT : nm_ch;
    Msg("LOD billboard '%s': cell [%ux%u], atlas [%ux%u], nm [%ux%u], %s", tex_name, cell_w, cell_h, atlas_w, atlas_h, nm_atlas_w, nm_atlas_h, vertical ? "vertical" : "horizontal");

    U32Vec lod_pixels, nm_pixels;
    CreateLODTexture(OBJECT, lod_pixels, nm_pixels, cell_w, cell_h, vertical, quality, 2);

    string_path out_name, src_name, nm_name;
    xr_strconcat(nm_name, tex_name, "_nm");

    // save color tga
    strcpy(src_name, EFS.ChangeFileExt(tex_name, ".tga").c_str());
    FS.update_path(out_name, _textures_, src_name);
    VerifyPath(out_name);
    DXTUtils::Converter::MakeTGA(atlas_w, atlas_h, lod_pixels, out_name);
    FS.set_file_age(out_name, age);

    // save normal map tga
    strcpy(src_name, EFS.ChangeFileExt(nm_name, ".tga").c_str());
    FS.update_path(out_name, _textures_, src_name);
    VerifyPath(out_name);
    DXTUtils::Converter::MakeTGA(nm_atlas_w, nm_atlas_h, nm_pixels, out_name);
    FS.set_file_age(out_name, age);

    // thm + dds：色彩 DXT3（边缘 alpha），法线 DXT5（alpha 存 hemi/AO），与批量同步参数一致
    STextureParams tp;
    tp.width			= atlas_w;
    tp.height			= atlas_h;
    tp.type				= STextureParams::ttImage;
    tp.mip_filter		= STextureParams::kMIPFilterAdvanced;
    tp.flags.assign		(STextureParams::flDitherColor | STextureParams::flGenerateMipMaps | STextureParams::flHasAlpha);

    tp.fmt				= STextureParams::tfDXT3;
    strcpy(src_name, EFS.ChangeFileExt(tex_name, ".dds").c_str());
    FS.update_path(out_name, _game_textures_, src_name);
    MakeGameTexture(out_name, lod_pixels.data(), tp);

    ETextureThumbnail* THM = new ETextureThumbnail(tex_name, false);
    THM->m_TexParams	= tp;
    THM->SetValid	();
    THM->Save		(age);
    xr_delete		(THM);

    // normal map dds + thm at its own (half) resolution
    tp.width			= nm_atlas_w;
    tp.height			= nm_atlas_h;
    tp.fmt				= STextureParams::tfDXT5;
    strcpy(src_name, EFS.ChangeFileExt(nm_name, ".dds").c_str());
    FS.update_path(out_name, _game_textures_, src_name);
    MakeGameTexture(out_name, nm_pixels.data(), tp);

    THM = new ETextureThumbnail(nm_name, false);
    THM->m_TexParams	= tp;
    THM->SetValid	();
    THM->Save		(age);
    xr_delete		(THM);
}