#include "stdafx.h"
#include "../xrEUI/EditorLocalization.h"

UILeftBarForm::UILeftBarForm()
{
}

UILeftBarForm::~UILeftBarForm()
{
}

void UILeftBarForm::Draw()
{
	if (ImGui::Begin(Loc("LeftBar"), 0))
	{
		if (ImGui::Button(Loc("Add Group")))
		{
			PTools->AppendPG(0, "pg");
			PTools->Modified();
		}
		ImGui::SameLine();

		if (ImGui::Button(Loc("Add Particle")))
		{
			PTools->AppendPE(0, "pe");
			PTools->Modified();
		}
		ImGui::SameLine();

		if (ImGui::Button(Loc("Clone")))
		{
			PTools->CloneCurrent();
		}
		ImGui::SameLine();

		if (ImGui::Button(Loc("Remove")))
		{
			PTools->RemoveCurrent();
		}

		ImGui::Separator();

		if (ImGui::Button(Loc("Import Effect")))
		{
			PTools->ImportPE();
		}
		ImGui::Separator();

		ImGui::SetNextItemOpen(true, ImGuiCond_Once);

		if (ImGui::TreeNode("Items"))
		{
			ImGui::BeginGroup();
			PTools->m_PList->Draw();
			ImGui::EndGroup();
			ImGui::TreePop();
		}

		if (ImGui::TreeNode("Reference List"))
		{
			PTools->DrawReferenceList();
			ImGui::TreePop();
		}
	}
	
	ImGui::End();
}
