// FLODGroupVisual.cpp: MT_LODGROUP visual implementation.

#include "stdafx.h"
#pragma hdrstop

#include "FLODGroupVisual.h"
#include "../../xrEngine/Fmesh.h"

// Normalized per-level thresholds, computed every frame in R_calculate.cpp
// from the ps_r__lodgroup_ssaN console variables (index 0 unused).
extern float r_lodgroup_ssa[4];

FLODGroupVisual::FLODGroupVisual()
{
	level_count	= 4;
	ZeroMemory	(level_child_start,	sizeof(level_child_start));
	ZeroMemory	(level_child_count,	sizeof(level_child_count));
	ZeroMemory	(ssa_threshold,		sizeof(ssa_threshold));
}

FLODGroupVisual::~FLODGroupVisual()
{
}

void FLODGroupVisual::Load(const char* N, IReader *data, u32 dwFlags)
{
	// Children links are resolved by the inherited OGF_CHILDREN_L handling
	// (Render->getVisual); the flat order matches the compiler's level order.
	FHierrarhyVisual::Load(N, data, dwFlags);

	// Level layout + per-node SSA thresholds.
	// find_chunk returns the chunk size and leaves the reader at the chunk
	// data start (same pattern as FLOD::Load).
	bool		founded	= !!data->find_chunk(OGF_LODGROUP_DEF);
	R_ASSERT3	(founded, "Missing OGF_LODGROUP_DEF chunk", N);
	const u32	version	= data->r_u32();
	R_ASSERT2	(version == 0, "Unsupported OGF_LODGROUP_DEF version");
	const u32	levels	= data->r_u32();
	R_ASSERT2	(levels >= 1 && levels <= 4, "Invalid OGF_LODGROUP level count");
	level_count	= levels;
	data->r		(level_child_count,	sizeof(u32) * 4);
	data->r		(ssa_threshold,		sizeof(float) * 4);

	// Flat list -> per-level ranges
	u32			acc		= 0;
	for (u32 i = 0; i < 4; ++i)
	{
		level_child_start[i]	= acc;
		acc						+= level_child_count[i];
	}
	R_ASSERT2	(acc == children.size(), "OGF_CHILDREN_L size mismatch");
}

void FLODGroupVisual::Copy(dxRender_Visual *pFrom)
{
	FHierrarhyVisual::Copy(pFrom);

	FLODGroupVisual*	F		= (FLODGroupVisual*)pFrom;
	level_count					= F->level_count;
	CopyMemory	(level_child_start,	F->level_child_start,	sizeof(level_child_start));
	CopyMemory	(level_child_count,	F->level_child_count,	sizeof(level_child_count));
	CopyMemory	(ssa_threshold,		F->ssa_threshold,		sizeof(ssa_threshold));
}

u32 FLODGroupVisual::SelectLevel(float ssa) const
{
	// Walk from the coarsest level down; engage the first level whose
	// threshold is above zero, whose screen-size condition is met, and that
	// actually has geometry. Level 0 is the fallback.
	for (int i = int(level_count) - 1; i >= 1; --i)
	{
		const float	thr	= ssa_threshold[i] > 0.f ? ssa_threshold[i] : r_lodgroup_ssa[i];
		if (thr > 0.f && ssa < thr && level_child_count[i] > 0)
			return u32(i);
	}
	return 0;
}
