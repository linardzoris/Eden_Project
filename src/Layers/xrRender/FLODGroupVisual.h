// FLODGroupVisual.h: MT_LODGROUP visual - distance-switched mesh-LOD group.
// Groups up to four mesh-LOD levels (level 0 = full detail) of the same level
// object behind a single visual node. Rendering picks exactly one level by
// screen-size (SSA); distant shadow phases benefit from the same selection.

#ifndef FLODGroupVisualH
#define FLODGroupVisualH
#pragma once

#include "FHierrarhyVisual.h"

class FLODGroupVisual : public FHierrarhyVisual
{
public:
	// Levels are stored flat inside 'children' (OGF_CHILDREN_L order);
	// these ranges describe the per-level slices of that flat list.
	u32								level_count;
	u32								level_child_start	[4];
	u32								level_child_count	[4];
	float							ssa_threshold		[4];	// per-node override; 0 = engine cvar
public:
									FLODGroupVisual		();
	virtual							~FLODGroupVisual	();

	virtual void	Load			(const char* N, IReader *data, u32 dwFlags);
	virtual void	Copy			(dxRender_Visual *pFrom);

	// Returns the level index to render for the given SSA value. Falls back
	// to the nearest lower non-empty level when a level has no children.
	u32				SelectLevel		(float ssa) const;
};

#endif //FLODGroupVisualH
