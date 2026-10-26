#include "stdafx.h"
#include "UIOmfEditorForm.h"
#include "imgui.h"
#include "../xrEUI/EditorLocalization.h"
#include <algorithm>

// OGF chunk ids used by .omf files (see xrEngine/Fmesh.h); duplicated locally
// to avoid pulling the full engine header into the editor
#define OMF_CHUNK_MOTIONS  14   // OGF_S_MOTIONS
#define OMF_CHUNK_SMPARAMS 15   // OGF_S_SMPARAMS
static const u16 OMF_SMPARAMS_VERSION = 4; // xrOGF_SMParamsVersion

UIOmfEditorForm* UIOmfEditorForm::Form = nullptr;

void UIOmfEditorForm::OmfData::Clear()
{
	animations_count = 0;
	anims.clear();
	notifies.clear();
	ogf_version = 0;
	parts.clear();
	motions_count = 0;
	motions.clear();
}

UIOmfEditorForm::UIOmfEditorForm()
{
}

UIOmfEditorForm::~UIOmfEditorForm()
{
}

// ---------------------------------------------------------------------------
// load
// ---------------------------------------------------------------------------

bool UIOmfEditorForm::Load(LPCSTR path)
{
	// files may live outside the FS roots: read via CRT and parse in memory
	FILE* file = fopen(path, "rb");
	if (!file)
	{
		ELog.Msg(mtError, "OMF Editor: can't open file '%s'.", path);
		return false;
	}
	fseek(file, 0, SEEK_END);
	long fsize = ftell(file);
	fseek(file, 0, SEEK_SET);
	if (fsize <= 0)
	{
		ELog.Msg(mtError, "OMF Editor: empty file '%s'.", path);
		fclose(file);
		return false;
	}
	xr_vector<char> buf((size_t)fsize);
	if (fread(buf.data(), 1, (size_t)fsize, file) != (size_t)fsize)
	{
		fclose(file);
		ELog.Msg(mtError, "OMF Editor: read failed '%s'.", path);
		return false;
	}
	fclose(file);

	IReader F(buf.data(), (u32)buf.size());
	OmfData data;
	m_Error.clear();

	// ---- chunk OGF_S_MOTIONS: count sub-chunk + animation chunks + notify chunks
	u32 id = F.r_u32();
	u32 size = F.r_u32();
	if (id != OMF_CHUNK_MOTIONS)
	{
		m_Error = "first chunk is not OGF_S_MOTIONS";
	}
	else
	{
		size_t motions_end = F.tell() + size;

		u32 cnt_id = F.r_u32();
		u32 cnt_size = F.r_u32();
		if (cnt_id != 0 || cnt_size != 4)
		{
			m_Error = "animation count sub-chunk not found";
		}
		else
		{
			data.animations_count = F.r_u32();
			data.anims.resize(data.animations_count);
			data.notifies.resize(data.animations_count);

			while (F.tell() < motions_end && !m_Error.size())
			{
				if (F.eof())
					break;
				u32 sub_id = F.r_u32();
				u32 sub_size = F.r_u32();
				size_t sub_end = F.tell() + sub_size;

				if (sub_id >= 1 && sub_id <= data.animations_count)
				{
					// animation chunk: name (stringZ) + raw key payload
					OmfAnim& anim = data.anims[sub_id - 1];
					F.r_stringZ(anim.name);
					size_t consumed = F.tell() - (sub_end - sub_size);
					if (sub_size < consumed)
					{
						m_Error = "animation chunk too small";
						break;
					}
					anim.data.resize(sub_size - (u32)consumed);
					if (!anim.data.empty())
						F.r(anim.data.data(), (u32)anim.data.size());
				}
				else if (sub_id >= data.animations_count + 2 && sub_id <= data.animations_count + 1 + data.animations_count)
				{
					// notify chunk for motion (sub_id - count - 2)
					u32 motion = sub_id - data.animations_count - 2;
					OmfNotify& n = data.notifies[motion];
					n.present = true;
					n.data.resize(sub_size);
					if (sub_size)
						F.r(n.data.data(), sub_size);
				}
				else
				{
					ELog.Msg(mtWarning, "OMF Editor: unknown sub-chunk %u (skipped).", sub_id);
					F.seek(sub_end);
				}
			}
		}
	}

	// ---- chunk OGF_S_SMPARAMS: version + bone parts + motion defs
	if (!m_Error.size())
	{
		if (F.eof())
		{
			m_Error = "OGF_S_SMPARAMS chunk missing";
		}
		else
		{
			id = F.r_u32();
			size = F.r_u32();
			if (id != OMF_CHUNK_SMPARAMS)
				m_Error = "second chunk is not OGF_S_SMPARAMS";
		}
	}

	if (!m_Error.size())
	{
		size_t params_end = F.tell() + size;
		data.ogf_version = F.r_s16();
		u16 parts = F.r_u16();
		data.parts.resize(parts);
		for (u16 p = 0; p < parts; ++p)
		{
			OmfPart& part = data.parts[p];
			F.r_stringZ(part.name);
			u16 bones = F.r_u16();
			part.bones.resize(bones);
			for (u16 b = 0; b < bones; ++b)
			{
				OmfBone& bone = part.bones[b];
				F.r_stringZ(bone.name);
				bone.id = F.r_u32();
			}
		}

		data.motions_count = F.r_u16();
		data.motions.resize(data.motions_count);
		for (u16 m = 0; m < data.motions_count; ++m)
		{
			OmfMotion& mot = data.motions[m];
			F.r_stringZ(mot.name);
			mot.flags = F.r_u32();
			mot.bone_or_part = F.r_u16();
			mot.motion_id = F.r_u16();
			mot.speed = F.r_float();
			mot.power = F.r_float();
			mot.accrue = F.r_float();
			mot.falloff = F.r_float();

			if (data.ogf_version >= OMF_SMPARAMS_VERSION)
			{
				u32 marks = F.r_u32();
				mot.marks.resize(marks);
				for (u32 k = 0; k < marks; ++k)
				{
					OmfMark& mark = mot.marks[k];
					// engine r_string consumes the CRLF terminator
					F.r_string(mark.name);
					u32 intervals = F.r_u32();
					mark.intervals.resize(intervals);
					for (u32 iv = 0; iv < intervals; ++iv)
					{
						mark.intervals[iv].t0 = F.r_float();
						mark.intervals[iv].t1 = F.r_float();
					}
				}
			}
		}

		if (F.tell() != params_end)
			ELog.Msg(mtWarning, "OMF Editor: SMPARAMS size mismatch (read %u of %u bytes) in '%s'.",
				(u32)(F.tell() - (params_end - size)), (u32)size, path);
	}


	if (m_Error.size())
	{
		ELog.Msg(mtError, "OMF Editor: %s ('%s').", m_Error.c_str(), path);
		return false;
	}

	// sanity: motion ids must map 1:1 onto animations (engine DEBUG checks this)
	if (data.motions_count != data.animations_count)
		ELog.Msg(mtWarning, "OMF Editor: motion count (%u) != animation count (%u).",
			(u32)data.motions_count, data.animations_count);
	for (u32 i = 0; i < data.motions.size() && i < data.animations_count; ++i)
	{
		if (data.motions[i].motion_id != i)
			ELog.Msg(mtWarning, "OMF Editor: motion '%s' has motion_id %u != index %u.",
				data.motions[i].name.c_str(), data.motions[i].motion_id, i);
		if (data.motions[i].name != data.anims[i].name)
			ELog.Msg(mtWarning, "OMF Editor: motion name '%s' != animation name '%s'.",
				data.motions[i].name.c_str(), data.anims[i].name.c_str());
	}

	m_Omf.Clear();
	m_Omf = std::move(data);
	m_FilePath = path;
	m_Dirty = false;
	m_SelectedMotion = m_Omf.motions.empty() ? -1 : 0;
	m_SelectedBone = 0;
	CollectBoneNames();
	ELog.Msg(mtInformation, "OMF Editor: loaded '%s' (%u motions, version %d).",
		path, (u32)m_Omf.motions_count, (int)m_Omf.ogf_version);
	return true;
}

// ---------------------------------------------------------------------------
// save
// ---------------------------------------------------------------------------

bool UIOmfEditorForm::Save(LPCSTR path)
{
	CMemoryWriter F;

	// ---- OGF_S_MOTIONS
	F.open_chunk(OMF_CHUNK_MOTIONS);
	F.open_chunk(0);
	F.w_u32(m_Omf.animations_count);
	F.close_chunk();
	for (u32 i = 0; i < m_Omf.anims.size(); ++i)
	{
		const OmfAnim& anim = m_Omf.anims[i];
		F.open_chunk(i + 1);
		F.w_stringZ(anim.name.c_str());
		if (!anim.data.empty())
			F.w(anim.data.data(), (u32)anim.data.size());
		F.close_chunk();
	}
	for (u32 i = 0; i < m_Omf.notifies.size(); ++i)
	{
		const OmfNotify& n = m_Omf.notifies[i];
		if (!n.present)
			continue;
		F.open_chunk(m_Omf.animations_count + 2 + i);
		if (!n.data.empty())
			F.w(n.data.data(), (u32)n.data.size());
		F.close_chunk();
	}
	F.close_chunk();

	// ---- OGF_S_SMPARAMS
	F.open_chunk(OMF_CHUNK_SMPARAMS);
	F.w_u16(m_Omf.ogf_version);
	F.w_u16((u16)m_Omf.parts.size());
	for (const OmfPart& part : m_Omf.parts)
	{
		F.w_stringZ(part.name.c_str());
		F.w_u16((u16)part.bones.size());
		for (const OmfBone& bone : part.bones)
		{
			F.w_stringZ(bone.name.c_str());
			F.w_u32(bone.id);
		}
	}
	F.w_u16(m_Omf.motions_count);
	for (const OmfMotion& mot : m_Omf.motions)
	{
		F.w_stringZ(mot.name.c_str());
		F.w_u32(mot.flags);
		F.w_u16(mot.bone_or_part);
		F.w_u16(mot.motion_id);
		F.w_float(mot.speed);
		F.w_float(mot.power);
		F.w_float(mot.accrue);
		F.w_float(mot.falloff);
		if (m_Omf.ogf_version >= OMF_SMPARAMS_VERSION)
		{
			// mark name terminator is CRLF (IWriter::w_string semantics)
			F.w_u32((u32)mot.marks.size());
			for (const OmfMark& mark : mot.marks)
			{
				F.w_string(mark.name.c_str());
				F.w_u32((u32)mark.intervals.size());
				for (const OmfMarkInterval& iv : mark.intervals)
				{
					F.w_float(iv.t0);
					F.w_float(iv.t1);
				}
			}
		}
	}
	F.close_chunk();

	if (!F.save_to(path))
	{
		ELog.Msg(mtError, "OMF Editor: can't save '%s'.", path);
		return false;
	}
	m_FilePath = path;
	m_Dirty = false;
	ELog.Msg(mtInformation, "OMF Editor: saved '%s'.", path);
	return true;
}

void UIOmfEditorForm::DoLoad()
{
	xr_string fn = m_FilePath;
	if (EFS.GetOpenName("$game_meshes$", fn, false, 0, -1, "*.omf"))
		Load(fn.c_str());
}

void UIOmfEditorForm::DoSave(bool save_as)
{
	if (m_Omf.motions.empty() && m_Omf.anims.empty())
		return;

	xr_string fn = m_FilePath;
	if (save_as || fn.empty())
	{
		if (!EFS.GetSaveName("$game_meshes$", fn, 0, -1, "*.omf"))
			return;
	}
	Save(fn.c_str());
}

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

void UIOmfEditorForm::CollectBoneNames()
{
	m_BoneNames.clear();
	for (const OmfPart& part : m_Omf.parts)
		for (const OmfBone& bone : part.bones)
			m_BoneNames.push_back(bone.name);
	if (m_SelectedBone >= (int)m_BoneNames.size())
		m_SelectedBone = 0;
}

bool UIOmfEditorForm::NameExists(const char* name, int skip_motion)
{
	for (int i = 0; i < (int)m_Omf.motions.size(); ++i)
		if (i != skip_motion && !_stricmp(m_Omf.motions[i].name.c_str(), name))
			return true;
	return false;
}

bool UIOmfEditorForm::RenameMotion(int index, const char* new_name)
{
	if (index < 0 || index >= (int)m_Omf.motions.size())
		return false;
	if (!new_name[0] || NameExists(new_name, index))
		return false;

	// motion names are matched case-insensitively by the engine
	xr_string lowered = new_name;
	std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](char c) { return (char)tolower((u8)c); });

	// rename the paired animation entry too (engine checks the name mapping)
	xr_string old_name = m_Omf.motions[index].name;
	m_Omf.motions[index].name = lowered;
	if (index < (int)m_Omf.anims.size() && !_stricmp(m_Omf.anims[index].name.c_str(), old_name.c_str()))
		m_Omf.anims[index].name = lowered;
	m_Dirty = true;
	return true;
}

bool UIOmfEditorForm::RenameBone(int global_index, const char* new_name)
{
	if (!new_name[0])
		return false;
	for (const xr_string& existing : m_BoneNames)
		if (!_stricmp(existing.c_str(), new_name))
			return false; // collision

	int gi = 0;
	for (OmfPart& part : m_Omf.parts)
	{
		for (OmfBone& bone : part.bones)
		{
			if (gi == global_index)
			{
				xr_string lowered = new_name;
				std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](char c) { return (char)tolower((u8)c); });
				bone.name = lowered;
				CollectBoneNames();
				m_Dirty = true;
				return true;
			}
			++gi;
		}
	}
	return false;
}

// ---------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------

void UIOmfEditorForm::DrawLeftPanel()
{
	if (ImGui::CollapsingHeader(Loc("Bone parts"), ImGuiTreeNodeFlags_DefaultOpen))
	{
		if (ImGui::Button(Loc("Copy to clipboard")))
		{
			xr_string out;
			for (const OmfPart& part : m_Omf.parts)
			{
				out += "["; out += part.name.c_str(); out += "]\n";
				for (const OmfBone& bone : part.bones)
				{
					out += bone.name.c_str();
					out += "\n";
				}
				out += "\n";
			}
			ImGui::SetClipboardText(out.c_str());
		}
		for (const OmfPart& part : m_Omf.parts)
		{
			if (ImGui::TreeNode(part.name.c_str()))
			{
				for (const OmfBone& bone : part.bones)
					ImGui::Text("%u: %s", bone.id, bone.name.c_str());
				ImGui::TreePop();
			}
		}
	}

	if (ImGui::CollapsingHeader(Loc("Rename bone")))
	{
		if (!m_BoneNames.empty())
		{
			if (ImGui::BeginCombo(Loc("Bone"), m_BoneNames[m_SelectedBone].c_str()))
			{
				for (int i = 0; i < (int)m_BoneNames.size(); ++i)
				{
					bool selected = (i == m_SelectedBone);
					if (ImGui::Selectable(m_BoneNames[i].c_str(), selected))
						m_SelectedBone = i;
					if (selected)
						ImGui::SetItemDefaultFocus();
				}
				ImGui::EndCombo();
			}
			ImGui::InputText(Loc("New name"), m_BoneRename, sizeof(m_BoneRename));
			if (ImGui::Button(Loc("Apply")))
			{
				if (RenameBone(m_SelectedBone, m_BoneRename))
					m_BoneRename[0] = 0;
				else
					ELog.Msg(mtError, "OMF Editor: bone rename failed (empty name or collision).");
			}
		}
	}
}

void UIOmfEditorForm::DrawMarks()
{
	OmfMotion& mot = m_Omf.motions[m_SelectedMotion];

	ImGui::SeparatorText(Loc("Motion marks"));
	if (ImGui::Button(Loc("Add mark")))
		ImGui::OpenPopup(Loc("New mark name"));
	if (ImGui::BeginPopupModal(Loc("New mark name"), nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		ImGui::InputText("##mark", m_NewMark, sizeof(m_NewMark));
		if (ImGui::Button(Loc("Ok")))
		{
			if (m_NewMark[0])
			{
				OmfMark mark;
				mark.name = m_NewMark;
				mot.marks.push_back(mark);
				m_Dirty = true;
				m_NewMark[0] = 0;
			}
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button(Loc("Cancel")))
			ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}

	for (int k = 0; k < (int)mot.marks.size(); ++k)
	{
		OmfMark& mark = mot.marks[k];
		ImGui::PushID(k);
		if (ImGui::TreeNode(mark.name.c_str()))
		{
			ImGui::InputText(Loc("Name"), m_MarkRename, sizeof(m_MarkRename));
			ImGui::SameLine();
			if (ImGui::SmallButton(Loc("Rename")))
			{
				if (m_MarkRename[0])
				{
					mark.name = m_MarkRename;
					m_MarkRename[0] = 0;
					m_Dirty = true;
				}
			}
			ImGui::SameLine();
			if (ImGui::SmallButton(Loc("Delete")))
			{
				mot.marks.erase(mot.marks.begin() + k);
				ImGui::TreePop();
				ImGui::PopID();
				m_Dirty = true;
				return;
			}

			int del = -1;
			for (int iv = 0; iv < (int)mark.intervals.size(); ++iv)
			{
				ImGui::PushID(iv);
				ImGui::DragFloat("##t0", &mark.intervals[iv].t0, 0.01f, 0.0f, 1000.0f, "%.2f");
				ImGui::SameLine();
				ImGui::DragFloat("##t1", &mark.intervals[iv].t1, 0.01f, 0.0f, 1000.0f, "%.2f");
				ImGui::SameLine();
				if (ImGui::SmallButton("x"))
					del = iv;
				ImGui::PopID();
			}
			if (del >= 0)
			{
				mark.intervals.erase(mark.intervals.begin() + del);
				m_Dirty = true;
			}
			if (ImGui::SmallButton(Loc("Add interval")))
			{
				OmfMarkInterval iv;
				mark.intervals.push_back(iv);
				m_Dirty = true;
			}
			ImGui::TreePop();
		}
		ImGui::PopID();
	}
}

void UIOmfEditorForm::DrawRightPanel()
{
	OmfMotion& mot = m_Omf.motions[m_SelectedMotion];

	if (ImGui::BeginCombo(Loc("Animation"), mot.name.c_str()))
	{
		for (int i = 0; i < (int)m_Omf.motions.size(); ++i)
		{
			bool selected = (i == m_SelectedMotion);
			if (ImGui::Selectable(m_Omf.motions[i].name.c_str(), selected))
				m_SelectedMotion = i;
			if (selected)
				ImGui::SetItemDefaultFocus();
		}
		ImGui::EndCombo();
	}

	ImGui::SameLine();
	if (ImGui::Button(Loc("Rename...")))
		ImGui::OpenPopup(Loc("Rename animation"));
	if (ImGui::BeginPopupModal(Loc("Rename animation"), nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		ImGui::InputText("##anim", m_AnimRename, sizeof(m_AnimRename));
		if (ImGui::Button(Loc("Ok")))
		{
			if (RenameMotion(m_SelectedMotion, m_AnimRename))
			{
				m_AnimRename[0] = 0;
				ImGui::CloseCurrentPopup();
			}
			else
				ELog.Msg(mtError, "OMF Editor: rename failed (empty name or collision).");
		}
		ImGui::SameLine();
		if (ImGui::Button(Loc("Cancel")))
			ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}

	ImGui::DragFloat(Loc("Speed"), &mot.speed, 0.01f, 0.0f, 100.0f, "%.3f");
	ImGui::DragFloat(Loc("Power"), &mot.power, 0.01f, 0.0f, 100.0f, "%.3f");
	ImGui::DragFloat(Loc("Accrue"), &mot.accrue, 0.01f, 0.0f, 100.0f, "%.3f");
	ImGui::DragFloat(Loc("Falloff"), &mot.falloff, 0.01f, 0.0f, 100.0f, "%.3f");

	struct FlagEntry { const char* label; u32 flag; };
	const FlagEntry flags[] = {
		{ "Stop at end",     1 << 1 },
		{ "No mix",          1 << 2 },
		{ "Sync part",       1 << 3 },
		{ "Use foot steps",  1 << 4 },
		{ "Move XForm",      1 << 5 },
		{ "Idle",            1 << 6 },
		{ "Use weapon bone", 1 << 7 },
	};
	for (const auto& f : flags)
	{
		bool on = (mot.flags & f.flag) != 0;
		if (ImGui::Checkbox(Loc(f.label), &on))
		{
			if (on)
				mot.flags |= f.flag;
			else
				mot.flags &= ~f.flag;
			m_Dirty = true;
		}
	}

	ImGui::Text("bone_or_part=%u motion_id=%u", mot.bone_or_part, mot.motion_id);

	if (m_Omf.ogf_version >= OMF_SMPARAMS_VERSION)
		DrawMarks();
	else
		ImGui::TextDisabled(Loc("No marks (omf version < 4)"));
}

void UIOmfEditorForm::Draw()
{
	if (ImGui::Button(Loc("Load...")))
		DoLoad();
	ImGui::SameLine();
	if (ImGui::Button(Loc("Save")))
		DoSave(false);
	ImGui::SameLine();
	if (ImGui::Button(Loc("Save as ...")))
		DoSave(true);
	ImGui::SameLine();
	if (ImGui::Button(Loc("Close")))
		bOpen = false;

	ImGui::SameLine();
	if (!m_FilePath.empty())
	{
		xr_string title = m_Dirty ? "* " : "";
		title += m_FilePath;
		ImGui::TextUnformatted(title.c_str());
	}
	else
		ImGui::TextDisabled(Loc("No file loaded"));

	if (m_FilePath.empty())
		return;

	ImGui::Separator();
	ImGui::BeginChild("omf_left", ImVec2(ImGui::GetWindowWidth() * 0.35f, 0), ImGuiChildFlags_ResizeX);
	DrawLeftPanel();
	ImGui::EndChild();

	ImGui::SameLine();
	ImGui::BeginChild("omf_right", ImVec2(0, 0));
	if (m_SelectedMotion >= 0 && m_SelectedMotion < (int)m_Omf.motions.size())
		DrawRightPanel();
	ImGui::EndChild();
}

void UIOmfEditorForm::Update()
{
	if (!Form)
		return;

	if (Form->bOpen)
	{
		ImGui::SetNextWindowSize(ImVec2(820, 620), ImGuiCond_FirstUseEver);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(640, 400));
		if (ImGui::Begin(Loc("OMF Editor"), &Form->bOpen))
		{
			Form->Draw();
		}
		else
		{
			Form->bOpen = false;
		}
		ImGui::End();
		ImGui::PopStyleVar();
	}
	else
	{
		xr_delete(Form);
	}
}

void UIOmfEditorForm::Show()
{
	if (Form)
	{
		Form->bOpen = true;
		return;
	}
	Form = new UIOmfEditorForm();
}
