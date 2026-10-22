#pragma once

#include "stdafx.h"

class CCustomObject;
class SPBItem;

// Environment generator: scatters library objects over the XZ extent of the
// target object(s). Works on any selected object; for terrain targets an RGBA
// heatmap can be used to drive per-channel object distribution and density.
// Without a heatmap only the R channel is active.
class EEnvGenerator
{
public:
	enum EChannel { CH_R = 0, CH_G, CH_B, CH_A, CH_COUNT };
	enum EFlags
	{
		flRandomRotation	= 1 << 0,
		flRandomScale		= 1 << 1,
		flScaleProportional	= 1 << 2,
	};

	struct SChannel
	{
		shared_str	ObjectsStr = "";		// comma-separated sequence, mirrors Objects
		RStringVec	Objects;
		float		Density	= 0.02f;		// obj/m^2, clamped to MaxDensity on use
	};

	EEnvGenerator();

	// settings
	SChannel	Channels[CH_COUNT];
	float		MaxDensity	= 0.02f;			// obj/m^2, per-channel upper bound
	Flags32		Flags;
	Fvector		MinScale;
	Fvector		MaxScale;
	Fvector		MinRotation;
	Fvector		MaxRotation;

	// heatmap (terrain targets only)
	xr_string	HeatmapFile;
	U32Vec		Heatmap;						// 0xAARRGGBB pixels
	u32			HeatmapW = 0;
	u32			HeatmapH = 0;

	float		ClampDensity(const SChannel& ch) const { return ch.Density <= 0.f ? 0.f : std::min(ch.Density, MaxDensity); }

	bool		LoadHeatmap(const char* path);
	void		UnloadHeatmap();
	bool		HeatmapReady() const { return HeatmapW && HeatmapH && Heatmap.size() == HeatmapW * HeatmapH; }
	IC float	SampleWeight(int ch, float u, float v) const;	// u,v in [0,1]
	float		AverageWeight(int ch) const;

	// terrain detection: CTerrain objects, or legacy CSceneObject whose
	// reference / object name ends with "_terrain"
	static bool	IsTerrainObject(CCustomObject* object);

	// statistics for the confirm dialog; heatmap_ready only tells that heatmap
	// data is loaded, the per-target terrain check happens inside
	void		SummarizeTargets(const xr_vector<CCustomObject*>& targets, bool heatmap_ready, float& total_area, int& estimate) const;

	// scatter over all targets, returns number of created objects
	int			Generate(const xr_vector<CCustomObject*>& targets, bool heatmap_ready, int estimate);

	// preset IO (.env)
	bool		SavePreset(const char* path);
	bool		LoadPreset(const char* path);

private:
	struct SNameIndex
	{
		xr_hash_set<shared_str>	Used;
		xr_map<xr_string, int>	NextIndex;

		void	Build();
		void	Get(char* buffer, const char* pref);
	};

	bool		PlaceObject(CCustomObject* target, const Fvector& pos, const Fvector& normal, const SChannel& ch, SNameIndex& idx, SPBItem* pb);
	void		ScatterUniform(CCustomObject* target, const SChannel& ch, int count, SNameIndex& idx, SPBItem* pb, int& placed);
	void		ScatterHeatmap(CCustomObject* target, const SChannel& ch, int ch_idx, int count, SNameIndex& idx, SPBItem* pb, int& placed);
	bool		SampleSurfacePoint(CCustomObject* target, const Fbox& bbox, Fvector& pos, Fvector& normal);
};
