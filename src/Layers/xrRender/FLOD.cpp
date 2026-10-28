#include "stdafx.h"
#include "../../xrEngine/Fmesh.h"
#include "FLOD.h"
#include "xrRender_console.h"

struct _hw 
{
	Fvector		p0		;
	Fvector		p1		;
	Fvector		n0		;
	Fvector		n1		;
	u32			sun_af	;
	Fvector2	t0		;
	Fvector2	t1		;
	u32			rgbh0	;
	u32			rgbh1	;
};
static D3DVERTEXELEMENT9 dwDecl[]	=
{
	{ 0, 0,  D3DDECLTYPE_FLOAT3,	D3DDECLMETHOD_DEFAULT, 	D3DDECLUSAGE_POSITION,	0 },	// pos-0
	{ 0, 12, D3DDECLTYPE_FLOAT3,	D3DDECLMETHOD_DEFAULT, 	D3DDECLUSAGE_POSITION,	1 },	// pos-1
	{ 0, 24, D3DDECLTYPE_FLOAT3,	D3DDECLMETHOD_DEFAULT, 	D3DDECLUSAGE_NORMAL,	0 },	// nrm-0
	{ 0, 36, D3DDECLTYPE_FLOAT3,	D3DDECLMETHOD_DEFAULT, 	D3DDECLUSAGE_NORMAL,	1 },	// nrm-1
	{ 0, 48, D3DDECLTYPE_D3DCOLOR,	D3DDECLMETHOD_DEFAULT, 	D3DDECLUSAGE_COLOR,		0 },	// factors
	{ 0, 52, D3DDECLTYPE_FLOAT2,	D3DDECLMETHOD_DEFAULT, 	D3DDECLUSAGE_TEXCOORD,	0 },	// uv
	{ 0, 60, D3DDECLTYPE_FLOAT2,	D3DDECLMETHOD_DEFAULT, 	D3DDECLUSAGE_TEXCOORD,	1 },	// uv
	{ 0, 68, D3DDECLTYPE_D3DCOLOR,	D3DDECLMETHOD_DEFAULT, 	D3DDECLUSAGE_TEXCOORD,	2 },	// rgbh-0
	{ 0, 72, D3DDECLTYPE_D3DCOLOR,	D3DDECLMETHOD_DEFAULT, 	D3DDECLUSAGE_TEXCOORD,	3 },	// rgbh-1
	D3DDECL_END()
};

void FLOD::Load			(LPCSTR N, IReader *data, u32 dwFlags)
{
	inherited::Load		(N,data,dwFlags);

	// LOD-def
	bool FoundedChunk = !!data->find_chunk(OGF_LODDEF2);
	R_ASSERT2(FoundedChunk, "Not found chunk OGF_LODDEF2");

	for (int f=0; f<8; f++)
	{
		data->r					(facets[f].v,sizeof(facets[f].v));
		_vertex* v				= facets[f].v;

		Fvector					N_,T;
		N_.set					(0,0,0);
		T.mknormal				(v[0].v,v[1].v,v[2].v);	N_.add	(T);
		T.mknormal				(v[1].v,v[2].v,v[3].v);	N_.add	(T);
		T.mknormal				(v[2].v,v[3].v,v[0].v);	N_.add	(T);
		T.mknormal				(v[3].v,v[0].v,v[1].v);	N_.add	(T);
		N_.div					(4.f);
		facets[f].N.normalize	(N_);
		facets[f].N.invert		();
	}

	// VS
	geom.create			(dwDecl, RCache.Vertex.Buffer(), RCache.QuadIB);

	// lod correction
	Fvector3			S;
	vis.box.getradius	(S);
	float r 			= vis.sphere.R;
	std::sort			(&S.x,&S.x+3);
	float a				= S.y;
	float Sf			= 4.f*(0.5f*(r*r*asin(a/r)+a*_sqrt(r*r-a*a)));
	float Ss			= M_PI*r*r;
	lod_factor			= Sf/Ss;
}
void FLOD::Copy			(dxRender_Visual *pFrom	)
{
	inherited::Copy		(pFrom);

	FLOD* F				= (FLOD*)pFrom;
	geom				= F->geom		;
	lod_factor			= F->lod_factor	;
	CopyMemory		(facets,F->facets,sizeof(facets));
}
void FLOD::Render		(float LOD)
{
#if RENDER!=R_R1
	// Main pass: billboards are batch-rendered by r_dsgraph_render_lods, this
	// stays empty. SMAP pass (r__shadow_proxy_lod 1): this visual was inserted
	// by r_dsgraph_insert_shadow_proxy with the SE_R2_SHADOW_AREF element
	// (alpha-tested shadow shader) - draw the facet facing the shadow light.
	// The z-prefill pass also runs in PHASE_SMAP but never sees
	// shadow_proxy_active set, so it keeps the legacy mesh path.
	if (RImplementation.phase != CRender::PHASE_SMAP || !RImplementation.shadow_proxy_active || ps_r__shadow_proxy_lod != 1)
		return;

	// Direction toward the shadow light. Facet vertices are already in world
	// space (per-reference LOD node), so the identity tree xform applies.
	Fvector							Ldir;
	Ldir.sub						(vis.sphere.P, RImplementation.shadow_proxy_light_pos).normalize();

	// best-facing facet, same semantics as the main-pass facet selection
	int					best_id		= 0;
	float				best_dot	= Ldir.dotproduct(facets[0].N);
	for (int f = 1; f < 8; f++)
	{
		float dot = Ldir.dotproduct(facets[f].N);
		if (dot > best_dot) { best_dot = dot; best_id = f; }
	}

	// Pack a single quad. No view-direction shift here (the shadow bias handles
	// acne); p1/n1/t1 duplicate p0/n0/t0 - the shadow VS only reads those.
	_face&		F					= facets[best_id];
	u32			vOffset				= 0;
	_hw*		V					= (_hw*) RCache.Vertex.Lock(4, geom->vb_stride, vOffset);
	static const int	vid [4]		= {3,0,2,1};
	for (u32 vit = 0; vit < 4; ++vit)
	{
		int				id			= vid[vit];
		V[vit].p0		= F.v[id].v;			V[vit].p1		= F.v[id].v;
		V[vit].n0		= F.N;					V[vit].n1		= F.N;
		V[vit].t0		= F.v[id].t;			V[vit].t1		= F.v[id].t;
		V[vit].sun_af	= color_rgba(F.v[id].c_sun, F.v[id].c_sun, 255, 255);
		V[vit].rgbh0	= F.v[id].c_rgb_hemi;	V[vit].rgbh1	= F.v[id].c_rgb_hemi;
	}
	RCache.Vertex.Unlock			(4, geom->vb_stride);

	RCache.tree.set_m_xform			(Fidentity);
	RCache.set_Geometry				(geom);
	RCache.Render					(D3DPT_TRIANGLELIST, vOffset, 0, 4, 0, 2);
#endif
}
