#include "StdAfx.h"


#include "../xrLC_Light/xrMU_Model.h"
#include "../xrLC_Light/xrMU_Model_Reference.h"

#include "Build.h"
#include "OGF_Face.h"

#define	TRY(a) try { a; } catch (...) { clMsg("* E: %s", #a); }
xrCriticalSection csThreadLock;

// Create one OGF_Reference per model subdiv for the given reference and
// register them in g_TREE_ROOT; returns their g_TREE_ROOT indices.
static void export_ref_subdivs( xrMU_Reference& mu_reference, xr_vector<u32>& generated_ids )
{
	xrMU_Model *model = mu_reference.model;
	for (xrMU_Model::v_subdivs_it it=model->m_subdivs.begin(); it!=model->m_subdivs.end(); it++)
	{
		OGF_Reference*	pOGF	= new OGF_Reference ();
		b_material*		M		= &(pBuild->materials()[it->material]);	// and it's material
		R_ASSERT		(M);

		// Common data
		pOGF->Sector			= mu_reference.sector;
		pOGF->material			= it->material;

		// Collect textures
		OGF_Texture				T;
		TRY(T.name			= pBuild->textures()[M->surfidx].name);
		TRY(T.pBuildSurface	= &(pBuild->textures()[M->surfidx]));
		TRY(pOGF->textures.push_back(T));

		// Special
		pOGF->model				= it->ogf;
		pOGF->vb_id				= it->vb_id;
		pOGF->vb_start			= it->vb_start;
		pOGF->ib_id				= it->ib_id;
		pOGF->ib_start			= it->ib_start;
		pOGF->xform.set			(mu_reference.xform);
		pOGF->c_scale			= mu_reference.c_scale;
		pOGF->c_bias			= mu_reference.c_bias;
		pOGF->sw_id				= it->sw_id;

		pOGF->CalcBounds		();
		generated_ids.push_back((u32)g_tree.size());

		csThreadLock.Enter();
 		g_tree.push_back		(pOGF);
		csThreadLock.Leave();
	}
}

// Create the billboard-LOD node (OGF_LOD) for the reference's model and
// register it in g_TREE_ROOT. Returns the node's g_TREE_ROOT index, or
// u32(-1) when the model has no billboard LOD (b_mu_model.lod_id).
static u32 export_lod_node( xrMU_Reference& mu_reference, const xr_vector<u32>& generated_ids )
{
	xrMU_Model *model = mu_reference.model;
	if (u16(-1) == model->m_lod_ID)
		return u32(-1);

	// Create Node and fill it with information
	b_lod&		LOD		= pBuild->lods	[model->m_lod_ID];
	OGF_LOD*	pNode	= new OGF_LOD (1,mu_reference.sector);
	pNode->lod_Material	= LOD.dwMaterial;
	for (int lf=0; lf<8; lf++)
	{
		b_lod_face&		F = LOD.faces[lf];
		OGF_LOD::_face& D = pNode->lod_faces[lf];
		for (int lv=0; lv<4; lv++)
		{
			mu_reference.xform.transform_tiny(D.v[lv].v,F.v[lv]);
			D.v[lv].t			= F.t[lv];
			D.v[lv].c_rgb_hemi	= 0xffffffff;
			D.v[lv].c_sun		= 0xff;
		}
	}

	// Add all 'OGFs' with such LOD-id
	for (u32 o=0; o<generated_ids.size(); o++)
		pNode->AddChield(generated_ids[o]);


	if (pNode->chields.size() == 0)
	{
		xr_delete		(pNode);
		return u32(-1);
	}

	// Register node
	pNode->CalcBounds				();
	csThreadLock.Enter();
	g_tree.push_back	(pNode);
	csThreadLock.Leave();

	// Calculate colors
	const float sm_range		= 5.f;
	for (int lf=0; lf<8; lf++)
	{
		OGF_LOD::_face& F = pNode->lod_faces[lf];
		for (int lv=0; lv<4; lv++)
		{
			Fvector	ptPos	= F.v[lv].v;

			base_color_c	_C;
			float 			_N	= 0;

			for (u32 v_it=0; v_it<model->m_vertices.size(); v_it++)
			{
				// get base
				Fvector			baseP;	mu_reference.xform.transform_tiny	(baseP,model->m_vertices[v_it]->P);
				base_color_c	baseC;	mu_reference.color[v_it]._get(baseC);

				base_color_c	vC;
				float			oD	= ptPos.distance_to	(baseP);
				float			oA  = 1/(1+100*oD*oD);
				vC = 			(baseC);
				vC.mul			(oA);
				_C.add			(vC);
				_N				+= oA;
			}

			float	s			= 1/(_N+EPS);
			_C.mul				(s);
			F.v[lv].c_rgb_hemi	= color_rgba(u8_clr(_C.rgb.x),u8_clr(_C.rgb.y),u8_clr(_C.rgb.z),u8_clr(_C.hemi));
			F.v[lv].c_sun		= u8_clr	(_C.sun);
		}
	}

	return u32(g_tree.size() - 1);
}

void export_ogf( xrMU_Reference& mu_reference, const xr_vector<u32>* lod_ref_ids )
{
	xr_vector<u32>		generated_ids;
	xrMU_Model *model = mu_reference.model;
	export_ref_subdivs( mu_reference, generated_ids );

	// Unbaked models export nothing at all (existing rule); the LOD group
	// must follow the same rule, so this check stays ahead of the group branch.
	if (model->color.size() == 0)
		return;

	// Mesh-LOD group: wrap full-detail + level references into one OGF_LodGroup
	// node consumed by MT_LODGROUP at runtime
	if (lod_ref_ids && lod_ref_ids->size())
	{
		OGF_LodGroup*		pGroup	= new OGF_LodGroup(1, mu_reference.sector);

		// level 0: full-detail subdiv references
		for (u32 id : generated_ids)
			pGroup->AddChield(id);
		pGroup->level_child_count[0] = (u32)pGroup->chields.size();

		// levels 1..3: export each mesh-LOD reference into the same group
		for (u32 r : *lod_ref_ids)
		{
			xrMU_Reference*		LR		= pBuild->mu_refs()[r];
			xr_vector<u32>		lvl_ids;
			export_ref_subdivs	(*LR, lvl_ids);
			const u32			before	= (u32)pGroup->chields.size();
			for (u32 id : lvl_ids)
				pGroup->AddChield(id);
			pGroup->level_child_count[LR->mesh_lod_level] =
				(u32)pGroup->chields.size() - before;
		}

		// Hybrid mode: with 'Using LOD' set the model also carries a billboard
		// LOD (b_mu_model.lod_id); it occupies mesh-LOD level 3, the farthest
		// level. The runtime group recursion reaches the existing MT_LOD
		// handling, so the billboard's own near/far logic stays intact.
		const u32	lod_node	= export_lod_node(mu_reference, generated_ids);
		if (u32(-1) != lod_node)
		{
			pGroup->AddChield			(lod_node);
			pGroup->level_child_count[3] = 1;
		}

		// Per-level switch distances (metres, from the base reference object)
		// convert to SSA thresholds at the exported bounding sphere: a level
		// engages once ssa = R/d^2 drops below its threshold. 0 = runtime cvar.
		// Must run after CalcBounds, which produces the exported sphere radius.
		R_ASSERT			(pGroup->chields.size());
		pGroup->CalcBounds	();
		for (u32 n = 1; n <= 3; ++n)
		{
			const float		d = mu_reference.mesh_lod_dist[n - 1];
			if (d > 0.f)
				pGroup->ssa_threshold[n] = pGroup->R / (d * d);
		}
		csThreadLock.Enter();
		g_tree.push_back	(pGroup);
		csThreadLock.Leave();
		return;
	}

	// Standalone billboard LOD (no mesh-LOD levels): existing MT_LOD export
	export_lod_node		(mu_reference, generated_ids);
}
