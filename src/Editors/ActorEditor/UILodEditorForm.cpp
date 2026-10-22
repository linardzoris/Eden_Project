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
	items.clear();
	m_ScanDone = true;

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
			Item it;
			it.name = full;
			it.short_name = f.name.c_str();
			it.UsingLOD = !!O->m_objectFlags.is(CEditableObject::eoUsingLOD);
			it.HasHQ = O->HasHQBillboard();
			items.push_back(it);
		}
		xr_delete(O);
	}
	UI->ProgressEnd(PB);

	ELog.Msg(mtInformation, "LOD Editor: found %d MU object(s).", items.size());
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
				it.UsingLOD = !!O->m_objectFlags.is(CEditableObject::eoUsingLOD);
			}
			done++;
			break;
		}
	}
	UI->ProgressEnd(PB);

	if (!saved.empty())
		ATools->Load(saved.c_str());

	ELog.Msg(mtInformation, "LOD Editor: %s generated for %d of %d object(s).", hq ? "BLOD" : "FLOD", done, list.size());
}

void UILodEditorForm::DrawObjectList()
{
	if (ImGui::Button("Rescan"))
		ScanObjects();
	ImGui::SameLine();
	if (ImGui::Button("All"))
		for (auto& it : items)
			it.selected = true;
	ImGui::SameLine();
	if (ImGui::Button("None"))
		for (auto& it : items)
			it.selected = false;
	ImGui::SameLine();
	ImGui::SetNextItemWidth(-1);
	ImGui::InputText("##filter", m_Filter, sizeof(m_Filter));

	ImGui::BeginChild("objects", ImVec2(0, 0), true);
	for (auto& it : items)
	{
		if (m_Filter[0] && !strstr(it.name.c_str(), m_Filter))
			continue;

		ImGui::PushID(it.name.c_str());
		xr_string mark = it.HasHQ ? "[HQ]" : (it.UsingLOD ? "[LOD]" : "[ - ]");
		ImGui::Selectable((mark + " " + it.short_name).c_str(), &it.selected);
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

	if (ImGui::Checkbox("MakeHQ (BLOD)", &m_MakeHQ))
	{
		m_MakeHQ = false;
		RunBatch(true);
	}
	if (ImGui::Checkbox("MakeLQ (FLOD)", &m_MakeLQ))
	{
		m_MakeLQ = false;
		RunBatch(false);
	}

	ImGui::Separator();
	ImGui::TextWrapped("Only large objects should use BLODs, small objects use FLODs instead. Engine&Compiler prioritize BLODs over FLODs. BLODs are saved independently, FLODs are merged into level_lods.");
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
			ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(720, 480));
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
