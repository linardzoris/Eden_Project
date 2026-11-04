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
	u32								hyst_level;					// last level chosen (hysteresis state)
public:
									FLODGroupVisual		();
	virtual							~FLODGroupVisual	();

	virtual void	Load			(const char* N, IReader *data, u32 dwFlags);
	virtual void	Copy			(dxRender_Visual *pFrom);

	// Returns the level index to render for the given SSA value. Falls back
	// to the nearest lower non-empty level when a level has no children.
	u32				SelectLevel		(float ssa) const;

	// Effective per-level threshold: the per-node override wins over the
	// engine cvar; 0 = level disabled.
	float			EffectiveThreshold	(u32 level) const;

	// SelectLevel with hysteresis: a level switch must clear the level
	// boundary by a relative margin (ps_r__lodgroup_hyst), which stops
	// flickering at the exact switch distance. State is per visual instance
	// (statics own one instance per placement), kept in 'hyst_level'.
	u32				SelectLevelHyst		(float ssa);
};

// Debug helpers for r__lodgroup_debug: per-frame per-level histogram of
// rendered LOD groups (counted in the main render phase only).
void			lodgroup_dbg_add	(u32 level);
const u32*		lodgroup_dbg_counts	();

#endif //FLODGroupVisualH
