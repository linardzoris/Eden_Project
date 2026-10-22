//---------------------------------------------------------------------------
#include "stdafx.h"
#include "EnvGenerator.h"
#include "../Entry/StaticObject/SceneObject.h"
#include "../Entry/Terrain/Terrain.h"
//---------------------------------------------------------------------------

extern bool Stbi_Load(LPCSTR full_name, U32Vec& data, u32& w, u32& h, u32& a);

static bool ends_with_terrain(LPCSTR name)
{
	if (!name)
		return false;

	size_t len = xr_strlen(name);
	return len >= 8 && 0 == stricmp(name + len - 8, "_terrain");
}

bool EEnvGenerator::IsTerrainObject(CCustomObject* object)
{
	// ixray heightmap-based editable terrain
	if (smart_cast<CTerrain*>(object))
		return true;

	// legacy heightmap terrains stored as scene objects
	CSceneObject* so = smart_cast<CSceneObject*>(object);
	if (!so)
		return false;

	return ends_with_terrain(so->RefName()) || ends_with_terrain(object->GetName());
}

EEnvGenerator::EEnvGenerator()
{
	Flags.zero();
	MinScale.set(1.f, 1.f, 1.f);
	MaxScale.set(1.f, 1.f, 1.f);
	MinRotation.set(0.f, 0.f, 0.f);
	MaxRotation.set(0.f, 0.f, 0.f);
}

//---------------------------------------------------------------------------

bool EEnvGenerator::LoadHeatmap(const char* path)
{
	UnloadHeatmap();

	u32 w, h, a;
	if (!Stbi_Load(path, Heatmap, w, h, a))
		return false;

	HeatmapW = w;
	HeatmapH = h;
	HeatmapFile = path;
	return true;
}

void EEnvGenerator::UnloadHeatmap()
{
	Heatmap.clear();
	HeatmapW = 0;
	HeatmapH = 0;
	HeatmapFile.clear();
}

IC float EEnvGenerator::SampleWeight(int ch, float u, float v) const
{
	if (!HeatmapReady())
		return 0.f;

	int x = iFloor(std::min(std::max(u, 0.f), 0.9999f) * (float)HeatmapW);
	int y = iFloor(std::min(std::max(v, 0.f), 0.9999f) * (float)HeatmapH);
	u32 p = Heatmap[y * HeatmapW + x];

	switch (ch)
	{
	case CH_R: return (float)((p >> 16) & 0xFF) / 255.f;
	case CH_G: return (float)((p >> 8) & 0xFF) / 255.f;
	case CH_B: return (float)(p & 0xFF) / 255.f;
	case CH_A: return (float)((p >> 24) & 0xFF) / 255.f;
	}
	return 0.f;
}

float EEnvGenerator::AverageWeight(int ch) const
{
	if (!HeatmapReady())
		return 0.f;

	u64 sum = 0;
	for (u32 p : Heatmap)
	{
		switch (ch)
		{
		case CH_R: sum += (p >> 16) & 0xFF; break;
		case CH_G: sum += (p >> 8) & 0xFF; break;
		case CH_B: sum += p & 0xFF; break;
		case CH_A: sum += (p >> 24) & 0xFF; break;
		}
	}
	return (float)(sum / 255ULL) / (float)Heatmap.size();
}

//---------------------------------------------------------------------------

void EEnvGenerator::SNameIndex::Build()
{
	Used.clear();
	NextIndex.clear();

	for (SceneToolsMapPairIt it = Scene->FirstTool(); it != Scene->LastTool(); ++it)
	{
		ESceneCustomOTool* ot = smart_cast<ESceneCustomOTool*>(it->second);
		if (!ot)
			continue;

		for (CCustomObject* object : ot->GetObjects())
			Used.insert(object->GetName());
	}
}

// same naming scheme as EScene::GenObjectName ("pref", "pref_0", "pref_1"...),
// but resolved with hash lookups instead of a full scene scan per candidate
void EEnvGenerator::SNameIndex::Get(char* buffer, const char* pref)
{
	int& i = NextIndex[pref];
	for (;; ++i)
	{
		xr_string temp;
		if (i != 0)
			temp = xr_string(pref) + "_" + xr_string::ToString(i - 1);
		else
			temp = pref;

		if (Used.insert(temp.c_str()).second)
		{
			xr_strcpy(buffer, 256, temp.c_str());
			return;
		}
	}
}

//---------------------------------------------------------------------------

void EEnvGenerator::SummarizeTargets(const xr_vector<CCustomObject*>& targets, bool heatmap_ready, float& total_area, int& estimate) const
{
	total_area = 0.f;
	float counted = 0.f;

	for (CCustomObject* target : targets)
	{
		Fbox bbox;
		if (!target->GetBox(bbox))
			continue;

		float area = (bbox.x2 - bbox.x1) * (bbox.z2 - bbox.z1);
		if (area <= 0.f)
			continue;

		total_area += area;

		if (heatmap_ready && IsTerrainObject(target))
		{
			for (int ch = 0; ch < CH_COUNT; ++ch)
			{
				const SChannel& C = Channels[ch];
				float dens = ClampDensity(C);
				if (dens <= 0.f || C.Objects.empty())
					continue;

				counted += area * dens * AverageWeight(ch);
			}
		}
		else
		{
			const SChannel& C = Channels[CH_R];
			float dens = ClampDensity(C);
			if (dens > 0.f && !C.Objects.empty())
				counted += area * dens;
		}
	}

	estimate = iFloor(counted + 0.5f);
}

bool EEnvGenerator::SampleSurfacePoint(CCustomObject* target, const Fbox& bbox, Fvector& pos, Fvector& normal)
{
	float rnd_x = ::Random.randF() * (bbox.x2 - bbox.x1) + bbox.x1;
	float rnd_z = ::Random.randF() * (bbox.z2 - bbox.z1) + bbox.z1;

	Fvector start;
	start.set(rnd_x, bbox.y2 + 1.f, rnd_z);
	Fvector dir;
	dir.set(0.f, -1.f, 0.f);

	SPickQuery PQ;
	PQ.prepare_rq(start, dir, bbox.y2 - bbox.y1 + 1.f, CDB::OPT_CULL | CDB::OPT_ONLYNEAREST);
	target->RayQuery(PQ);

	if (!PQ.r_count())
		return false;

	pos = start;
	pos.mad(dir, PQ.r_begin()->range);
	normal.mknormal(PQ.r_begin()->verts[0], PQ.r_begin()->verts[1], PQ.r_begin()->verts[2]);
	return true;
}

bool EEnvGenerator::PlaceObject(CCustomObject* target, const Fvector& pos, const Fvector& normal, const SChannel& ch, SNameIndex& idx, SPBItem* pb)
{
	LPCSTR ref_name = ch.Objects[::Random.randI((int)ch.Objects.size())].c_str();
	string256 namebuffer;
	idx.Get(namebuffer, ref_name);

	CSceneObject* obj = new CSceneObject((LPVOID)0, namebuffer);
	CEditableObject* ref = obj->SetReference(ref_name);
	if (!ref)
	{
		ELog.DlgMsg(mtError, "Environment generator: can't load reference object '%s'.", ref_name);
		xr_delete(obj);
		return false;
	}

	if (Flags.is(flRandomRotation))
	{
		Fvector p;
		p.set(::Random.randF(std::min(MinRotation.x, MaxRotation.x), std::max(MinRotation.x, MaxRotation.x)),
			::Random.randF(std::min(MinRotation.y, MaxRotation.y), std::max(MinRotation.y, MaxRotation.y)),
			::Random.randF(std::min(MinRotation.z, MaxRotation.z), std::max(MinRotation.z, MaxRotation.z)));
		obj->SetRotation(p);
	}

	if (Flags.is(flRandomScale))
	{
		Fvector s;
		if (Flags.is(flScaleProportional))
		{
			float k = ::Random.randF(std::min(MinScale.x, MaxScale.x), std::max(MinScale.x, MaxScale.x));
			s.set(k, k, k);
		}
		else
		{
			s.set(::Random.randF(std::min(MinScale.x, MaxScale.x), std::max(MinScale.x, MaxScale.x)),
				::Random.randF(std::min(MinScale.y, MaxScale.y), std::max(MinScale.y, MaxScale.y)),
				::Random.randF(std::min(MinScale.z, MaxScale.z), std::max(MinScale.z, MaxScale.z)));
		}
		obj->SetScale(s);
	}

	obj->MoveTo(pos, normal);
	Scene->AppendObject(obj, false);

	if (pb)
		pb->Inc();

	return true;
}

void EEnvGenerator::ScatterUniform(CCustomObject* target, const SChannel& ch, int count, SNameIndex& idx, SPBItem* pb, int& placed)
{
	Fbox bbox;
	if (!target->GetBox(bbox))
		return;

	int misses = 0;
	for (int i = 0; i < count;)
	{
		Fvector pos, normal;
		if (!SampleSurfacePoint(target, bbox, pos, normal))
		{
			if (++misses > 512)
			{
				ELog.Msg(mtError, "Environment generator: too many ray misses on '%s', stopping this channel.", target->GetName());
				return;
			}
			continue;
		}
		misses = 0;

		if (!PlaceObject(target, pos, normal, ch, idx, pb))
			return;

		++i;
		++placed;
	}
}

void EEnvGenerator::ScatterHeatmap(CCustomObject* target, const SChannel& ch, int ch_idx, int count, SNameIndex& idx, SPBItem* pb, int& placed)
{
	Fbox bbox;
	if (!target->GetBox(bbox))
		return;

	float sx = bbox.x2 - bbox.x1;
	float sz = bbox.z2 - bbox.z1;

	// expected count already includes the channel's average weight, so the
	// rejection loop keeps the distribution correct without runaway retries
	int attempts_left = count * 12 + 256;
	int misses = 0;

	for (int i = 0; i < count && attempts_left > 0;)
	{
		--attempts_left;

		float u = ::Random.randF();
		float v = ::Random.randF();
		if (::Random.randF() > SampleWeight(ch_idx, u, v))
			continue;

		Fvector pos, normal;
		if (!SampleSurfacePoint(target, bbox, pos, normal))
		{
			if (++misses > 512)
			{
				ELog.Msg(mtError, "Environment generator: too many ray misses on '%s', stopping this channel.", target->GetName());
				return;
			}
			continue;
		}
		misses = 0;

		if (!PlaceObject(target, pos, normal, ch, idx, pb))
			return;

		++i;
		++placed;
	}
}

int EEnvGenerator::Generate(const xr_vector<CCustomObject*>& targets, bool heatmap_ready, int estimate)
{
	if (targets.empty())
		return 0;

	SNameIndex idx;
	idx.Build();

	int placed = 0;
	SPBItem* pb = UI->ProgressStart(std::max(1, estimate), "Environment generation: ");

	for (CCustomObject* target : targets)
	{
		Fbox bbox;
		if (!target->GetBox(bbox))
			continue;

		float area = (bbox.x2 - bbox.x1) * (bbox.z2 - bbox.z1);
		if (area <= 0.f)
			continue;

		bool use_heat = heatmap_ready && IsTerrainObject(target);

		if (use_heat)
		{
			for (int ch = 0; ch < CH_COUNT; ++ch)
			{
				const SChannel& C = Channels[ch];
				float dens = ClampDensity(C);
				if (dens <= 0.f || C.Objects.empty())
					continue;

				int count = iFloor(area * dens * AverageWeight(ch) + 0.5f);
				ScatterHeatmap(target, C, ch, count, idx, pb, placed);
			}
		}
		else
		{
			const SChannel& C = Channels[CH_R];
			float dens = ClampDensity(C);
			if (dens > 0.f && !C.Objects.empty())
			{
				int count = iFloor(area * dens + 0.5f);
				ScatterUniform(target, C, count, idx, pb, placed);
			}
		}
	}

	UI->ProgressEnd(pb);
	return placed;
}

//---------------------------------------------------------------------------

bool EEnvGenerator::SavePreset(const char* path)
{
	IWriter* stream = FS.w_open(path);
	if (!stream)
	{
		ELog.DlgMsg(mtError, "Environment generator: can't create preset '%s'.", path);
		return false;
	}

	stream->w_u8(1);										// version
	stream->w_float(MaxDensity);
	stream->w_u32(Flags.get());
	stream->w_fvector3(MinScale);
	stream->w_fvector3(MaxScale);
	stream->w_fvector3(MinRotation);
	stream->w_fvector3(MaxRotation);

	for (int ch = 0; ch < CH_COUNT; ++ch)
	{
		stream->w_float(Channels[ch].Density);
		stream->w_stringZ(Channels[ch].ObjectsStr);
	}

	stream->w_stringZ(HeatmapFile.c_str());

	FS.w_close(stream);
	return true;
}

bool EEnvGenerator::LoadPreset(const char* path)
{
	FS.TryLoad(path);
	IReader* stream = FS.r_open(path);
	if (!stream)
	{
		ELog.DlgMsg(mtError, "Environment generator: can't open preset '%s'.", path);
		return false;
	}

	u8 ver = stream->r_u8();
	if (ver != 1)
	{
		ELog.DlgMsg(mtError, "Environment generator: unsupported preset version %d.", ver);
		FS.r_close(stream);
		return false;
	}

	MaxDensity = stream->r_float();
	Flags.flags = stream->r_u32();
	stream->r_fvector3(MinScale);
	stream->r_fvector3(MaxScale);
	stream->r_fvector3(MinRotation);
	stream->r_fvector3(MaxRotation);

	for (int ch = 0; ch < CH_COUNT; ++ch)
	{
		Channels[ch].Density = stream->r_float();
		stream->r_stringZ(Channels[ch].ObjectsStr);
		_SequenceToList(Channels[ch].Objects, *Channels[ch].ObjectsStr);
	}

	shared_str hm;
	stream->r_stringZ(hm);
	FS.r_close(stream);

	if (hm.size() && FS.exist(*hm))
	{
		if (!LoadHeatmap(*hm))
			ELog.Msg(mtError, "Environment generator: preset heatmap '%s' failed to load.", *hm);
	}
	else
	{
		if (hm.size())
			ELog.Msg(mtError, "Environment generator: preset heatmap '%s' not found.", *hm);
		UnloadHeatmap();
	}

	return true;
}
