#include "stdafx.h"
#include "UILodEditorForm.h"
#include "imgui.h"

UILodEditorForm* UILodEditorForm::Form = nullptr;

UILodEditorForm::UILodEditorForm()
{
}

UILodEditorForm::~UILodEditorForm()
{
}

// enumerate all library objects, keep only MU-static ones (hide static/dynamic/HOM)
void UILodEditorForm::ScanObjects()
{
	// preserve the selection across rescans
	xr_vector<Item> old;
	old.swap(items);
	groups.clear();
	m_ScanDone = true;
	m_LastClickIndex = -1;

	FS_FileSet lst;
	if (!FS.file_list(lst, _objects_, FS_ListFiles, "*.object"))
	{
		ELog.Msg(mtError, "LOD Editor: can't list objects folder.");
		return;
	}

	SPBItem* PB = UI->ProgressStart(lst.size(), "Scanning MU objects");
	string_path full;
	for (const auto& f : lst)
	{
		PB->Inc();
		FS.update_path(full, _objects_, f.name.c_str());

		CEditableObject* O = new CEditableObject(full);
		if (O->Load(full) && O->IsMUStatic())
		{
			xr_path rel(f.name.c_str());
			Item it;
			it.name = full;
			it.file_name = rel.xfilename().c_str();
			it.folder = xr_path(rel.parent_path()).xstring().c_str();
			it.UsingLOD = !!O->m_objectFlags.is(CEditableObject::eoUsingLOD);
			it.HasHQ = O->HasHQBillboard();
			it.HasLQ = O->HasLQTexture();
			for (const auto& prev : old)
				if (prev.name == it.name) { it.selected = prev.selected; break; }
			items.push_back(it);
		}
		xr_delete(O);
	}
	UI->ProgressEnd(PB);

	BuildGroups();
	ELog.Msg(mtInformation, "LOD Editor: found %d MU object(s).", items.size());
}

// the FS caches directory listings: new dds/thm files are invisible to
// HasHQBillboard until the affected roots are force-rescanned
void UILodEditorForm::RescanFS()
{
	// new blod/flod dds/thm files are invisible to HasHQBillboard/HasLQTexture
	// until the _eden\_shared\lods roots are force-rescanned
	FS.rescan_path(FS.get_path(_game_textures_)->m_Path, TRUE);
	FS.rescan_path(FS.get_path(_objects_)->m_Path, TRUE);
}

// group items by folder (list is sorted, so folders are contiguous)
void UILodEditorForm::BuildGroups()
{
	groups.clear();
	for (int i = 0; i < (int)items.size(); i++)
	{
		if (groups.empty() || groups.back().folder != items[i].folder)
		{
			FolderGroup g;
			g.folder = items[i].folder;
			groups.push_back(g);
		}
		groups.back().indices.push_back(i);
	}
}

void UILodEditorForm::ApplyUsingLOD(bool value)
{
	u32 count = 0;
	SPBItem* PB = UI->ProgressStart(items.size(), value ? "Enabling Using LOD" : "Disabling Using LOD");
	for (auto& it : items)
	{
		PB->Inc();
		if (!it.selected || it.UsingLOD == value)
			continue;

		CEditableObject* O = new CEditableObject(it.name.c_str());
		if (O->Load(it.name.c_str()))
		{
			O->m_objectFlags.set(CEditableObject::eoUsingLOD, value);
			if (!O->Save(O->GetName()))
				ELog.Msg(mtError, "LOD Editor: can't save '%s'.", O->GetName());
			else
			{
				it.UsingLOD = value;
				count++;
			}
		}
		xr_delete(O);
	}
	UI->ProgressEnd(PB);

	ELog.Msg(mtInformation, "LOD Editor: Using LOD %s for %d object(s).", value ? "enabled" : "disabled", count);
}

void UILodEditorForm::RunBatch(bool hq)
{
	xr_vector<xr_string> list;
	for (auto& it : items)
		if (it.selected)
			list.push_back(it.name);

	if (list.empty())
	{
		ELog.DlgMsg(mtInformation, "LOD Editor: no objects selected.");
		return;
	}

	// remember the current edit object and restore it after the batch
	xr_string saved;
	if (ATools->CurrentObject())
		saved = ATools->CurrentObject()->GetName();

	// editor render mode for the whole batch (same state as the single Make HQ/LQ flow)
	bool engine_render = MainForm->GetLeftBarForm()->GetRenderMode() == UILeftBarForm::Render_Engine;
	MainForm->GetLeftBarForm()->SetRenderMode(false);

	CTimer batch_timer;
	batch_timer.Start();

	SPBItem* PB = UI->ProgressStart(list.size(), hq ? "Making HQ billboards (BLOD)" : "Making LQ lods (FLOD)");
	u32 done = 0;
	for (const auto& name : list)
	{
		PB->Inc(name.c_str());
		if (!ATools->Load(name.c_str()))
			continue;

		ATools->GenerateLODForCurrentObject(hq);

		// refresh list marks from the object we just processed
		CEditableObject* O = ATools->CurrentObject();
		for (auto& it : items)
		{
			if (it.name != name)
				continue;

			if (O)
			{
				it.HasHQ = O->HasHQBillboard();
				it.HasLQ = O->HasLQTexture();
				it.UsingLOD = !!O->m_objectFlags.is(CEditableObject::eoUsingLOD);
			}
			done++;
			break;
		}
	}
	UI->ProgressEnd(PB);
	float seconds = batch_timer.GetElapsed_sec();

	// new dds/thm files must enter the FS cache before HasHQBillboard can see them
	RescanFS();
	ScanObjects();

	if (!saved.empty())
		ATools->Load(saved.c_str());

	MainForm->GetLeftBarForm()->SetRenderMode(engine_render);

	// update the time estimate from the measurement
	float per_object = seconds / (float)list.size();
	if (hq) m_EstimateHQ = per_object;
	else m_EstimateLQ = per_object;

	ELog.Msg(mtInformation, "LOD Editor: %s generated for %d of %d object(s) in %.1fsec.", hq ? "BLOD" : "FLOD", done, list.size(), seconds);
	ELog.DlgMsg(mtInformation, "Generation successfully done!");
}

void UILodEditorForm::DrawObjectList()
{
	if (ImGui::Button("Rescan"))
	{
		RescanFS();
		ScanObjects();
	}
	ImGui::SameLine();
	if (ImGui::Button("All"))
		for (auto& it : items)
			it.selected = true;
	ImGui::SameLine();
	if (ImGui::Button("None"))
	{
		for (auto& it : items)
			it.selected = false;
		m_LastClickIndex = -1;
	}
	ImGui::SameLine();
	ImGui::SetNextItemWidth(-1);
	ImGui::InputText("##filter", m_Filter, sizeof(m_Filter));

	// flat multi-select list (file names only, full path on hover)
	ImGui::BeginChild("flat list", ImVec2(0, ImGui::GetContentRegionAvail().y * 0.45f), true);
	int visible = 0;
	for (int i = 0; i < (int)items.size(); i++)
	{
		auto& it = items[i];
		if (m_Filter[0] && !strstr(it.name.c_str(), m_Filter))
			continue;
		visible++;

		ImGui::PushID(it.name.c_str());
		xr_string mark = it.HasHQ ? "[HQ]" : (it.HasLQ ? "[LQ]" : "[ - ]");
		bool clicked = ImGui::Selectable((mark + " " + it.file_name).c_str(), it.selected);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("%s", it.name.c_str());
		if (clicked)
		{
			if (ImGui::GetIO().KeyShift && m_LastClickIndex >= 0)
			{
				// shift+click: range select from the anchor to this row
				int a = _min(m_LastClickIndex, i), b = _max(m_LastClickIndex, i);
				for (int j = a; j <= b; j++)
					items[j].selected = true;
			}
			else
			{
				it.selected = !it.selected;
				m_LastClickIndex = i;
			}
		}
		ImGui::PopID();
	}
	if (visible == 0)
		ImGui::TextDisabled("no matching objects");
	ImGui::EndChild();

	// explorer-style folder tree, checkboxes synced with the flat list
	ImGui::BeginChild("folder tree", ImVec2(0, 0), true);
	ImGui::TextDisabled("Folders");
	for (auto& g : groups)
	{
		// filter: keep the folder if any member matches
		bool any = false;
		for (int idx : g.indices)
			if (!m_Filter[0] || strstr(items[idx].name.c_str(), m_Filter))
			{
				any = true;
				break;
			}
		if (!any)
			continue;

		ImGui::PushID(g.folder.c_str());
		if (ImGui::TreeNode("", "%s (%d)", g.folder.empty() ? "<root>" : g.folder.c_str(), g.indices.size()))
		{
			for (int idx : g.indices)
			{
				auto& it = items[idx];
				if (m_Filter[0] && !strstr(it.name.c_str(), m_Filter))
					continue;

				ImGui::PushID(idx);
				xr_string mark = it.HasHQ ? "[HQ]" : (it.HasLQ ? "[LQ]" : "[ - ]");
				ImGui::Checkbox((mark + " " + it.file_name).c_str(), &it.selected);
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("%s", it.name.c_str());
				ImGui::PopID();
			}
			ImGui::TreePop();
		}
		ImGui::PopID();
	}
	ImGui::EndChild();
}

void UILodEditorForm::DrawActions()
{
	u32 selected = 0, with_lod = 0;
	for (auto& it : items)
		if (it.selected)
		{
			selected++;
			if (it.UsingLOD)
				with_lod++;
		}

	ImGui::Text("Selected: %d", selected);

	// Using LOD reflects the whole selection: checked only when every selected object uses LOD
	bool using_lod = selected > 0 && with_lod == selected;
	if (ImGui::Checkbox("Using LOD", &using_lod))
		ApplyUsingLOD(using_lod);

	ImGui::Separator();

	if (ImGui::Button("MakeHQ (BLOD)", ImVec2(-1, 0)))
	{
		if (selected == 0)
			ELog.DlgMsg(mtInformation, "LOD Editor: no objects selected.");
		else
		{
			m_MakeHQConfirm = true;
			m_MakeLQConfirm = false;
			ImGui::OpenPopup("Confirm generation");
		}
	}
	if (ImGui::Button("MakeLQ (FLOD)", ImVec2(-1, 0)))
	{
		if (selected == 0)
			ELog.DlgMsg(mtInformation, "LOD Editor: no objects selected.");
		else
		{
			m_MakeLQConfirm = true;
			m_MakeHQConfirm = false;
			ImGui::OpenPopup("Confirm generation");
		}
	}

	if (ImGui::BeginPopupModal("Confirm generation", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		bool hq = m_MakeHQConfirm;
		int seconds = int(ceil(selected * (hq ? m_EstimateHQ : m_EstimateLQ)));
		ImGui::Text("Total %d MU-Objects selected,", selected);
		ImGui::Text("%d %s will be generated,", selected, hq ? "BLODs" : "FLODs");
		ImGui::Text("estimated processing time: %d seconds.", seconds);
		ImGui::Separator();
		if (ImGui::Button("Proceed", ImVec2(120, 0)))
		{
			ImGui::CloseCurrentPopup();
			RunBatch(hq);
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel", ImVec2(120, 0)))
			ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}

	ImGui::Separator();
	ImGui::TextWrapped("Only large objects should use BLODs, small objects use FLODs instead. Engine&Compiler prioritize BLODs over FLODs. BLODs and FLODs are saved as independent textures.");
	ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 200, 80, 255));
	ImGui::TextWrapped("WARNING: First-time generation results in wrong albedo, this is a bug TBD. FIX: Generate any LQ for once upon first AE use!");
	ImGui::PopStyleColor();
}

void UILodEditorForm::Draw()
{
	if (!m_ScanDone)
		ScanObjects();

	ImGui::Columns(2, "lodEditor", false);
	ImGui::SetColumnWidth(0, 430.f);

	ImGui::BeginChild("object list", ImVec2(0, 0), true);
	DrawObjectList();
	ImGui::EndChild();

	ImGui::NextColumn();

	ImGui::BeginChild("actions", ImVec2(0, 0), true);
	DrawActions();
	ImGui::EndChild();

	ImGui::Columns(1);
}

void UILodEditorForm::Update()
{
	if (Form)
	{
		if (!Form->IsClosed())
		{
			ImGui::SetNextWindowSize(ImVec2(1250, 760), ImGuiCond_FirstUseEver);
			ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1000, 600));
			if (ImGui::Begin("LOD Editor", &Form->bOpen))
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
}

void UILodEditorForm::Show()
{
	VERIFY(!Form);
	Form = new UILodEditorForm();
	Form->bOpen = true;
}
