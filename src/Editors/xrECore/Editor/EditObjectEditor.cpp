#include "stdafx.h"


#include "EditObject.h"
#include "EditMesh.h"
#include "../../xrEngine/motion.h"
#include "../../xrEngine/bone.h"
#include "ExportSkeleton.h"
#include "ExportObjectOGF.h"
#include "D3DUtils.h"
#include "ui_main.h"
#include "render.h"
#include "../Public/PropertiesListHelper.h"
#include "../../Layers/xrRender/ResourceManager.h"
#include "ImageManager.h"

const float tex_w	= LOD_SAMPLE_COUNT*LOD_IMAGE_SIZE;
const float tex_h	= 1*LOD_IMAGE_SIZE;
const float offs_x 	= 1.f/tex_w;
const float offs_y 	= 1.f/tex_h;

static Fvector LOD_pos[4]={
	{-1.0f+offs_x, 1.0f-offs_y, 0.0f},
	{ 1.0f-offs_x, 1.0f-offs_y, 0.0f},
	{ 1.0f-offs_x,-1.0f+offs_y, 0.0f},
	{-1.0f+offs_x,-1.0f+offs_y, 0.0f}
};
static FVF::LIT LOD[4]={
	{{-1.0f, 1.0f, 0.0f},  0xFFFFFFFF, {0.0f,0.0f}}, // F 0
	{{ 1.0f, 1.0f, 0.0f},  0xFFFFFFFF, {0.0f,0.0f}}, // F 1
	{{ 1.0f,-1.0f, 0.0f},  0xFFFFFFFF, {0.0f,0.0f}}, // F 2
	{{-1.0f,-1.0f, 0.0f},  0xFFFFFFFF, {0.0f,0.0f}}, // F 3
};

bool CEditableObject::Reload()
{
	ClearGeometry();
	return Load(m_LoadName.c_str());
}

bool CEditableObject::RayPick(float& dist, const Fvector& S, const Fvector& D, const Fmatrix& inv_parent, SRayPickInfo* pinf)
{
	bool picked = false;
	for(EditMeshIt m = m_Meshes.begin();m!=m_Meshes.end();m++)
		if( (*m)->RayPick( dist, S, D, inv_parent, pinf ) )
			picked = true;
	return picked;
}

void CEditableObject::RayQuery(SPickQuery& pinf)
{
	for(EditMeshIt m = m_Meshes.begin();m!=m_Meshes.end();m++)
		(*m)->RayQuery(pinf);
}

void CEditableObject::RayQuery(CDB::COLLIDER& CL, SPickQuery& pinf)
{
	for(EditMeshIt m = m_Meshes.begin();m!=m_Meshes.end();m++)
		(*m)->RayQuery(CL, pinf);
}

void CEditableObject::RayQuery(const Fmatrix& parent, const Fmatrix& inv_parent, SPickQuery& pinf)
{
	for(EditMeshIt m = m_Meshes.begin();m!=m_Meshes.end();m++)
		(*m)->RayQuery(parent, inv_parent, pinf);
}

void CEditableObject::BoxQuery(const Fmatrix& parent, const Fmatrix& inv_parent, SPickQuery& pinf)
{
	for(EditMeshIt m = m_Meshes.begin();m!=m_Meshes.end();m++)
		(*m)->BoxQuery(parent, inv_parent, pinf);
}

#if 1
bool CEditableObject::FrustumPick(const CFrustum& frustum, const Fmatrix& parent){
	for(EditMeshIt m = m_Meshes.begin();m!=m_Meshes.end();m++)
		if((*m)->FrustumPick(frustum, parent))	return true;
	return false;
}

bool CEditableObject::BoxPick(CCustomObject* obj, const Fbox& box, const Fmatrix& inv_parent, SBoxPickInfoVec& pinf){
	bool picked = false;
	for(EditMeshIt m = m_Meshes.begin();m!=m_Meshes.end();m++)
		if ((*m)->BoxPick(box, inv_parent, pinf)){
			pinf.back().s_obj = obj;
			picked = true;
		}
	return picked;
}
#endif

extern float 	ssaLIMIT;
extern float	g_fSCREEN;
static const float ssaLim = 64.f*64.f/(640*480);
void CEditableObject::Render(const Fmatrix& parent, int priority, bool strictB2F, SurfaceVec* surfaces)
{
	if (!(m_LoadState.is(LS_RBUFFERS)))
		DefferedLoadRP();
	Fvector v;
	float r;
	Fbox bb;
	bb.xform(m_BBox, parent);
	bb.getsphere(v, r);

	if (EPrefs->object_flags.is(epoDrawLOD) && (m_objectFlags.is(eoUsingLOD) && (CalcSSA(v, r) < ssaLim)))
	{
		if ((1 == priority) && (true == strictB2F))
			RenderLOD(parent);
	}
	else {
		RCache.set_xform_world(parent);
		if (m_objectFlags.is(eoHOM))
		{
			if ((1 == priority) && (false == strictB2F))
				RenderEdge(parent, 0, 0, 0x40B64646);

			if ((2 == priority) && (true == strictB2F))
				RenderSelection(parent, 0, 0, 0xA0FFFFFF);

		}
		else if (m_objectFlags.is(eoSoundOccluder))
		{
			if ((1 == priority) && (false == strictB2F))
				RenderEdge(parent, 0, 0, 0xFF000000);

			if ((2 == priority) && (true == strictB2F))
				RenderSelection(parent, 0, 0, 0xA00000FF);
		}
		else 
		{
			if (psDeviceFlags.is(rsEdgedFaces) && (1 == priority) && (false == strictB2F))
				RenderEdge(parent);

			if (IsSkeleton())
				Engine.External.SetSkinningMode(4);

			size_t s_id = 0;
			for (SurfaceIt s_it = m_Surfaces.begin(); s_it != m_Surfaces.end(); s_it++)
			{
				int pr = (*s_it)->_Priority();
				bool strict = (*s_it)->_StrictB2F();

				if ((priority == pr) && (strictB2F == strict))
				{
					// FX: Панда написал систему инстансов для объектов, 
					// однако, если мы релоадим объекты и в новом объекте
					// полей больше, чем в старом, то получам выход за 
					// пределы. Поэтому просто регаем дефолтный материал.

					if (surfaces != nullptr && surfaces->size() > s_id)
					{
						EDevice->SetShader((*surfaces)[s_id]->_Shader());
					}
					else
					{
						EDevice->SetShader((*s_it)->_Shader());
					}

					for (EditMeshIt _M = m_Meshes.begin(); _M != m_Meshes.end(); _M++)
					{
						if (IsSkeleton())
							(*_M)->RenderSkeleton(parent, *s_it);
						else
							(*_M)->Render(parent, *s_it);
					}
				}
				s_id++;
			}

			Engine.External.SetSkinningMode();
		}
	}
}

void CEditableObject::RenderSingle(const Fmatrix& parent)
{
	for (int i=0; i<4; i++){
		Render(parent, i, false);
		Render(parent, i, true);
	}
}

void CEditableObject::RenderAnimation(const Fmatrix&){
}

void CEditableObject::RenderEdge(const Fmatrix& parent, CEditableMesh* mesh, CSurface* surf, u32 color)
{
	if (!(m_LoadState.is(LS_RBUFFERS))) DefferedLoadRP();

	EDevice->SetShader(EDevice->m_WireShader);
	if(mesh) mesh->RenderEdge(parent, surf, color);
	else for(EditMeshIt _M = m_Meshes.begin();_M!=m_Meshes.end();_M++)
			(*_M)->RenderEdge(parent, surf, color);
}

void CEditableObject::RenderSelection(const Fmatrix& parent, CEditableMesh* mesh, CSurface* surf, u32 color)
{
	if (!(m_LoadState.is(LS_RBUFFERS))) DefferedLoadRP();

	RCache.set_xform_world(parent);
	EDevice->SetShader(EDevice->m_SelectionShader);
	EDevice->RenderNearer(0.0005);
	if(mesh) mesh->RenderSelection(parent, surf, color);
	else for(EditMeshIt _M = m_Meshes.begin();_M!=m_Meshes.end();_M++)
			(*_M)->RenderSelection(parent, surf, color);
	EDevice->ResetNearer();
}

IC static void CalculateLODTC(int frame, int w_cnt, int h_cnt, int tex_w, int tex_h, Fvector2& lt, Fvector2& rb)
{
	Fvector2	ts, hp;
	ts.set		(1.f/(float)w_cnt,1.f/(float)h_cnt);
	hp.set		(0.5f/(float)tex_w,0.5f/(float)tex_h);
	lt.x        = (frame%w_cnt+0)*ts.x+hp.x;
	lt.y        = (frame/w_cnt+0)*ts.y+hp.y;
	rb.x        = (frame%w_cnt+1)*ts.x-hp.x;
	rb.y        = (frame/w_cnt+1)*ts.y-hp.y;
}

void CEditableObject::GetLODFrame(int frame, Fvector p[4], Fvector2 t[4], const Fmatrix* parent, int w_cnt, int h_cnt)
{
	R_ASSERT(m_objectFlags.is(eoUsingLOD));
	Fvector P,S;
	m_BBox.get_CD	(P,S);
	float r 		= _max(S.x,S.z);//sqrtf(S.x*S.x+S.z*S.z);
	Fmatrix T,matrix,rot;
	T.scale			(r,S.y,r);
	T.translate_over(P);
	if (parent)
		T.mulA_43(*parent);

	float angle 	= frame*(PI_MUL_2/float(LOD_SAMPLE_COUNT));
	rot.rotateY(-angle);
	matrix.mul(T,rot);
	Fvector2 lt, rb;
	int tex_w, tex_h;
	if (HasHQBillboard())
	{
		u32 cw, ch, aw, ah; bool vert;
		GetLODAtlasParams(cw, ch, vert, aw, ah);
		tex_w = (int)aw;
		tex_h = (int)ah;
	}
	else
	{
		tex_w = LOD_SAMPLE_COUNT*LOD_IMAGE_SIZE;
		tex_h = LOD_IMAGE_SIZE;
	}
	CalculateLODTC(frame,w_cnt,h_cnt,tex_w,tex_h,lt,rb);
	t[0].set(lt);
	t[1].set(rb.x,lt.y);
	t[2].set(rb);
	t[3].set(lt.x,rb.y);
	matrix.transform_tiny(p[0],LOD_pos[0]);
	matrix.transform_tiny(p[1],LOD_pos[1]);
	matrix.transform_tiny(p[2],LOD_pos[2]);
	matrix.transform_tiny(p[3],LOD_pos[3]);
}

void CEditableObject::RenderLOD(const Fmatrix& parent)
{
	Fvector C;
	C.sub			(parent.c,UI->CurrentView().m_Camera.GetPosition()); C.y = 0;
	float m 		= C.magnitude();
	if (m<EPS) return;
	C.div			(m);
	int max_frame;
	float max_dot	= 0;
	Fvector HPB;
	parent.getHPB	(HPB);

	for (int frame=0; frame<LOD_SAMPLE_COUNT; frame++){
		float angle = angle_normalize(frame*(PI_MUL_2/float(LOD_SAMPLE_COUNT))+HPB.x);

		Fvector D;
		D.setHP(angle,0);
		float dot = C.dotproduct(D);
		if (dot<0.7072f) continue;

		if (dot>max_dot){
			max_dot = dot;
			max_frame = frame;
		}
	}
	{
		Fvector    	p[4];
		Fvector2 	t[4];
		int w_cnt, h_cnt;
		GetLODGridForRender(w_cnt, h_cnt);
		GetLODFrame(max_frame,p,t,0,w_cnt,h_cnt);
		for (int i=0; i<4; i++){ LOD[i].p.set(p[i]); LOD[i].t.set(t[i]); }
		RCache.set_xform_world(parent);
		EDevice->SetShader		(m_LODShader?m_LODShader:EDevice->m_WireShader);
		DU_impl.DrawPrimitiveLIT	(D3DPT_TRIANGLEFAN, 2, LOD, 4, true, false);
	}
}

// shared base name: "lod_<objects-relative path with '_'>", no folder part
xr_string CEditableObject::GetLODBaseName()
{
	xr_string obj_name = EFS.ChangeFileExt(m_LibName.c_str(), "");
	string_path objs_root;
	FS.update_path(objs_root, _objects_, "");
	if (obj_name.find(objs_root) == 0)
		obj_name.erase(0, xr_strlen(objs_root));
	string512 nm;
	xr_strcpy(nm, obj_name.c_str()); _ChangeSymbol(nm,'\\','_');
	return xr_string("lod_")+nm;
}

xr_string CEditableObject::GetLODTextureName()
{
	// legacy layout (textures\lod\lod_<name>), kept for old references
	return xr_string("lod\\")+GetLODBaseName();
}

xr_string CEditableObject::GetLODFLODTextureName()
{
	// FLOD: merged into level_lods at build time, also usable as a standalone texture
	return xr_string("_eden\\_shared\\lods\\flod\\")+GetLODBaseName();
}

xr_string CEditableObject::GetLODBillboardTextureName()
{
	// BLOD: standalone high-res billboard, preferred over the merged FLOD when present
	return xr_string("_eden\\_shared\\lods\\blod\\")+GetLODBaseName();
}

bool CEditableObject::HasHQBillboard()
{
	xr_string fname	= GetLODBillboardTextureName()+xr_string(".dds");
	string_path full;
	FS.update_path	(full,_game_textures_,fname.c_str());
	return !!FS.exist(full);
}

bool CEditableObject::HasLQTexture()
{
	xr_string fname	= GetLODFLODTextureName()+xr_string(".dds");
	string_path full;
	FS.update_path	(full,_game_textures_,fname.c_str());
	return !!FS.exist(full);
}

// 单元格尺寸就近吸附到 2 的幂（64..2048），满足 DXT/mipmap 的 POT 约束
IC static u32 SnapLODCellSize(float v)
{
	v	= clampr(v, 64.f, 2048.f);
	u32 lo	= 64;
	while (lo*2 <= v) lo *= 2;
	u32 hi	= lo*2;
	return (hi - v < v - lo) ? (hi <= 2048 ? hi : lo) : lo;
}

// 按物体 AABB 真实比例计算 8 向图集布局：
//   W>H -> 8 格竖排（图集 H=8*cell_h<=2048）；W<=H -> 8 格横排（图集 W=8*cell_w<=2048）
//   比例 r 与捕捉正交窗口一致（viewW/viewH），超限或退化时回退 1:1 cell 256x256（2048x256 横排）
void CEditableObject::GetLODAtlasParams(u32& cell_w, u32& cell_h, bool& vertical, u32& atlas_w, u32& atlas_h)
{
	cell_w			= 256;
	cell_h			= 256;
	vertical		= false;
	atlas_w			= LOD_SAMPLE_COUNT*cell_w;
	atlas_h			= cell_h;

	Fvector C, S;
	m_BBox.get_CD	(C, S);					// S 为半尺寸
	float viewW		= 2.f*_max(S.x, S.z);	// 与 CreateLODSamples 正交窗口一致
	float viewH		= m_BBox.max.y - m_BBox.min.y;
	if (viewW <= EPS || viewH <= EPS)
		return;

	float ratio		= viewW/viewH;			// W/H
	vertical		= ratio > 1.f;
	if (vertical)
	{
		cell_h		= 256;					// 短边固定 256
		cell_w		= SnapLODCellSize(cell_h*ratio);
		atlas_w		= cell_w;
		atlas_h		= LOD_SAMPLE_COUNT*cell_h;
	}
	else
	{
		cell_w		= 256;
		cell_h		= SnapLODCellSize(cell_w/ratio);
		atlas_w		= LOD_SAMPLE_COUNT*cell_w;
		atlas_h		= cell_h;
	}
	VERIFY(atlas_w <= 2048 && atlas_h <= 2048);
}

void CEditableObject::GetLODGridForRender(int& w_cnt, int& h_cnt)
{
	w_cnt	= LOD_SAMPLE_COUNT;
	h_cnt	= 1;
	if (HasHQBillboard())
	{
		u32 cw, ch, aw, ah;
		bool vert;
		GetLODAtlasParams(cw, ch, vert, aw, ah);
		if (vert)
		{
			w_cnt	= 1;
			h_cnt	= LOD_SAMPLE_COUNT;
		}
	}
}

void CEditableObject::OnDeviceCreate()
{
}

void CEditableObject::OnDeviceDestroy()
{
	DefferedUnloadRP();
}

void CEditableObject::DefferedLoadRP()
{
	if (m_LoadState.is(LS_RBUFFERS)) return;

	// skeleton
	if (IsSkeleton())
		vs_SkeletonGeom.create((D3DVERTEXELEMENT9*)g_dwDeclSkinning4W,RCache.Vertex.Buffer(),RCache.Index.Buffer());

//*/
	// создать LOD shader
	xr_string l_name = HasHQBillboard() ? GetLODBillboardTextureName() : GetLODFLODTextureName();
	xr_string fname = xr_string(l_name)+xr_string(".dds");
	m_LODShader.destroy();
//    if (FS.exist(_game_textures_,fname.c_str()))
	if (m_objectFlags.is(eoUsingLOD))
		m_LODShader.create(GetLODShaderName(),l_name.c_str());
	m_LoadState.set(LS_RBUFFERS,TRUE);
}
void CEditableObject::DefferedUnloadRP()
{
	if (!(m_LoadState.is(LS_RBUFFERS))) return;
	// skeleton
	vs_SkeletonGeom.destroy();
	// удалить буфера
	for (EditMeshIt _M=m_Meshes.begin(); _M!=m_Meshes.end(); _M++)
		if (*_M) (*_M)->GenerateRenderBuffers();
	// удалить shaders
	for(SurfaceIt s_it=m_Surfaces.begin(); s_it!=m_Surfaces.end(); s_it++)
		(*s_it)->OnDeviceDestroy();
	// LOD
	m_LODShader.destroy();
	m_LoadState.set(LS_RBUFFERS,FALSE);
}
void CEditableObject::EvictObject()
{
	EditMeshIt m 				= m_Meshes.begin();
	for(;m!=m_Meshes.end();m++){
		(*m)->UnloadCForm		();
		(*m)->UnloadVNormals	(true);
		(*m)->UnloadSVertices	(true);
		(*m)->UnloadFNormals	(true);
	}
	DefferedUnloadRP			();
}

//bool CEditableObject::PrepareOGF(IWriter& F, u8 infl, bool gen_tb, CEditableMesh* mesh)
//{
//	return IsSkeleton()?PrepareSkeletonOGF(F,infl):PrepareRigidOGF(F,gen_tb,mesh);
//}
//
//bool CEditableObject::PrepareRigidOGF(IWriter& F, bool gen_tb, CEditableMesh* mesh)
//{
//    CExportObjectOGF E(this);
//    return E.Export(F,gen_tb,mesh);
//}
//
//bool CEditableObject::PrepareSVGeometry(IWriter& F, u8 infl)
//{
//    CExportSkeleton E(this);
//    return E.ExportGeometry(F, infl);
//}
//
//bool CEditableObject::PrepareSVKeys(IWriter& F)
//{
//    CExportSkeleton E(this);
//    return E.ExportMotionKeys(F);
//}
//
//bool CEditableObject::PrepareSVDefs(IWriter& F)
//{
//    CExportSkeleton E(this);
//    return E.ExportMotionDefs(F);
//}
//
//bool CEditableObject::PrepareSkeletonOGF(IWriter& F, u8 infl)
//{
//    CExportSkeleton E(this);
//    return E.Export(F,infl);
//}
//
//bool CEditableObject::PrepareOMF(IWriter& F)
//{
//    CExportSkeleton E(this);
//    return E.ExportMotions(F);
//}
//---------------------------------------------------------------------------

void  CEditableObject::OnChangeTransform(PropValue*)
{
	UI->RedrawScene();
}
//---------------------------------------------------------------------------

//. #include "../../xrEngine/blenders/Blender.h"
IC BOOL BE      (BOOL A, BOOL B)
{
	bool a = !!A;
	bool b = !!B;
	return a==b;
}
bool CEditableObject::CheckShaderCompatible()
{
	bool bRes 			= true;
	for(SurfaceIt s_it=m_Surfaces.begin(); s_it!=m_Surfaces.end(); s_it++)
	{
		IBlender* 		B = EDevice->Resources->_FindBlender(*(*s_it)->m_ShaderName);
		Shader_xrLC* 	C = EDevice->ShaderXRLC.Get(*(*s_it)->m_ShaderXRLCName);
		if (!B||!C){
			ELog.Msg	(mtError,"Object '%s': invalid or missing shader [E:'%s', C:'%s']",GetName(),(*s_it)->_ShaderName(),(*s_it)->_ShaderXRLCName());
			bRes 		= false;
		}else{
			if (!BE(B->canBeLMAPped(),!C->flags.bLIGHT_Vertex)){
				ELog.Msg	(mtError,"Object '%s', material '%s': engine shader '%s' non compatible with compiler shader '%s'", GetName(),
					(*s_it)->_Name(), (*s_it)->_ShaderName(), (*s_it)->_ShaderXRLCName());
				
				bRes 		= false;
			}
		}
	}
	return bRes;
}

void CEditableObject::CreateBone(shared_str Name)
{
	if (!m_BoneParts.empty() || m_objectFlags.test(eoDynamic))
		return;
	
	m_LoadState.set(LS_RBUFFERS, false);

	CBone* B = new CBone();
	B->SetName(Name.c_str());
	B->SetWMap("");
	B->SetRestParams(0.01f, Fvector().set(0, 0, 0), Fvector().set(0, 0, 0));

	B->ResetData();
	B->Reset();

	B->SetParentName("");
	B->parent = nullptr;
   
	SBonePart& BP = m_BoneParts.emplace_back(SBonePart());
	BP.alias = "default";

	m_objectFlags.set(eoDynamic, TRUE);

	BP.bones.push_back(B->Name());
	m_Bones.push_back(B);
	PrepareBones();

	for (CEditableMesh* _M : m_Meshes)
	{
		_M->UnloadSVertices();
	}

	AssignBoneName = Name;
}

void CEditableObject::AddBone(CBone* parent_bone)
{
	CBone* B 			= new CBone();

	string256			new_name;
	u32 i				= 0;

	do{
		sprintf				(new_name,"bone%.3d", i++);
	}while(    FindBoneByName(new_name) );

	B->SetName			(new_name);
	B->SetWMap			("");
	B->SetRestParams	(0.01f, Fvector().set(0,0,0), Fvector().set(0,0,0) );

	B->ResetData		();
	B->Reset			();

	if(parent_bone)
	{
		B->SetParentName(parent_bone->Name().c_str());
	}else
	{
		int	bid = 		GetRootBoneID	();
		GetBone(bid)->SetParentName		(B->Name().c_str());
		B->SetParentName				("");
	}

	m_BoneParts[0].bones.push_back(B->Name());
	m_Bones.push_back	(B);
	PrepareBones		();

	for (EditMeshIt _M=m_Meshes.begin(); _M!=m_Meshes.end(); _M++)
	{
		(*_M)->UnloadSVertices();
	}
}

CBone* 	bone_to_delete = NULL;
u32 	bone_to_delete_frame = 0;

void CEditableObject::DeleteBone(CBone* bone)
{
	CBone* PB = bone->Parent();

	for (BoneIt b_it=m_Bones.begin(); b_it!=m_Bones.end(); ++b_it)
	{
		CBone* B = *b_it;
		if(B->Parent()==bone)
			B->SetParentName(PB?PB->Name().c_str():"");
	}
	BoneIt bit 		= std::find(m_Bones.begin(), m_Bones.end(), bone);
	if(bit==m_Bones.end())
		Msg("! bone not found -((");
	else
		m_Bones.erase	(bit);

	BPIt bpit 			= BonePart(bone);
	RStringVec::iterator iit 	= (*bpit).bones.begin();
	RStringVec::iterator iit_e 	= (*bpit).bones.end();
	for( ;iit!=iit_e; ++iit)
	{
		if(*iit==bone->Name())
		{
			(*bpit).bones.erase(iit);
			break;
		}
	}


	bone_to_delete = bone;
	bone_to_delete_frame = EDevice->dwFrame;
	PrepareBones	();

	for (EditMeshIt _M=m_Meshes.begin(); _M!=m_Meshes.end(); _M++)
	{
		(*_M)->UnloadSVertices();
	}
}

//---------------------------------------------------------------------------

BPIt CEditableObject::BonePart(CBone* B)
{
	BPIt it 		= FirstBonePart();
	BPIt it_e 		= LastBonePart();
	for( ;it!=it_e; ++it)
	{
		RStringVec::iterator iit 	= (*it).bones.begin();
		RStringVec::iterator iit_e 	= (*it).bones.end();
		for( ;iit!=iit_e; ++iit)
		{
			if(*iit==B->Name())
				return it;
		}
	}
	return it_e;
}

void CEditableObject::RenameBone(CBone* bone, LPCSTR new_name)
{
	BPIt bpit 			= BonePart(bone);
	RStringVec::iterator iit 	= (*bpit).bones.begin();
	RStringVec::iterator iit_e 	= (*bpit).bones.end();

	for( ;iit!=iit_e; ++iit)
	{
		if(*iit==bone->Name())
		{
			*iit = new_name;
			break;
		}
	}

	for (BoneIt b_it=m_Bones.begin(); b_it!=m_Bones.end(); ++b_it)
	{
		CBone* B = *b_it;
		if(B->ParentName()==bone->Name())
			B->SetParentName(new_name);
	}

	for (SMotionIt s_it=m_SMotions.begin(); s_it!=m_SMotions.end(); ++s_it) 
	{
		CSMotion* M = *s_it;
		for(BoneMotionIt bm_it=M->BoneMotions().begin(); bm_it!=M->BoneMotions().end(); ++bm_it)
		{
			if(bm_it->name == bone->Name())
				bm_it->name = new_name;
		}
		
	}

	//bone->SetName(new_name);
	
	Modified();
}

