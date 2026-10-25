#include "stdafx.h"
#include "../xrEUI/EditorLocalization.h"

#include "../xrECore/Editor/EditMesh.h"

extern ECORE_API BOOL g_force16BitTransformQuant;
extern ECORE_API BOOL g_force32BitTransformQuant;

UILeftBarForm::UILeftBarForm()
{
	m_RenderMode = Render_Editor;
	m_PickMode = 2;

	if (g_force16BitTransformQuant)
	{
		m_AnimMode = e16bit;
	}
	else if (g_force32BitTransformQuant)
	{
		m_AnimMode = e32bit;
	}
}

UILeftBarForm::~UILeftBarForm()
{
}

void UILeftBarForm::Draw()
{
	if (ImGui::Begin(Loc("LeftBar"), 0))
	{
		ImGui::SetNextItemOpen(true, ImGuiCond_Once);
		if (ImGui::TreeNode(Loc("Model")))
		{
			ImGui::AlignTextToFramePadding();
			ImGui::Text(Loc("Render:")); ImGui::SameLine();
			if (ImGui::RadioButton(Loc("Editor"), m_RenderMode == Render_Editor))
			{
				ATools->PhysicsStopSimulate();
				m_RenderMode = Render_Editor;
				ExecCommand(COMMAND_UPDATE_PROPERTIES);
				UI->RedrawScene();
			}

			ImGui::SameLine();
			if (ImGui::RadioButton(Loc("Engine"), m_RenderMode == Render_Engine))
			{
				ATools->PhysicsStopSimulate();
				m_RenderMode = Render_Engine;
				if (!ATools->IsVisualPresent()) ExecCommand(COMMAND_MAKE_PREVIEW);
				if (!ATools->IsVisualPresent()) SetRenderMode(false);
				else						  SetRenderMode(true);
				ExecCommand(COMMAND_UPDATE_PROPERTIES);
				UI->RedrawScene();
			}


			ImGui::Checkbox(Loc("Auto Smooth"), &EPrefs->IsEdgeSmooth);
			ImGui::SameLine(0, 10);

			if (ImGui::Button(Loc("Bone View"))) 
			{
				ATools->BoneView->Show(true);
			}

			ImGui::SameLine(0, 10);
			if (ImGui::Button(Loc("Bone Parts")))
			{
				UIBoneForm::Show();
			}

			ImGui::Separator();
			ImGui::Text(Loc("Animation:")); ImGui::SameLine();

			if (ImGui::RadioButton(Loc("8bit"), m_AnimMode == e8bit))
			{
				m_AnimMode = e8bit;
				g_force16BitTransformQuant = false;
				g_force32BitTransformQuant = false;
			}

			ImGui::SameLine();
			if (ImGui::RadioButton(Loc("16bit"), m_AnimMode == e16bit))
			{
				m_AnimMode = e16bit;
				g_force16BitTransformQuant = true;
				g_force32BitTransformQuant = false;
			}
			
			ImGui::SameLine(); 
			if (ImGui::RadioButton(Loc("32bit"), m_AnimMode == e32bit))
			{
				m_AnimMode = e32bit;
				g_force16BitTransformQuant = false;
				g_force32BitTransformQuant = true;
			}

			static const char* PickModeList[] = { "None","Surface","Bone" };
			ImGui::Combo(Loc("Pick mode"), &m_PickMode, PickModeList, 3, -1);
			ImGui::TreePop();

		}

		ImGui::Separator();
		ImGui::SetNextItemOpen(true, ImGuiCond_Once);
		//if (ImGui::TreeNode("Object Items"))
		{
			ImGui::BeginGroup();
			ATools->m_ObjectItems->Draw();
			ImGui::EndGroup();
		//	ImGui::TreePop();
		}
	}

	if (ATools->CurrentObject() != nullptr)
	{
		if (!ATools->CurrentObject()->m_objectFlags.test(CEditableObject::eoDynamic))
		{
			if (ImGui::Button(Loc("Make dynamic")))
			{
				ATools->CurrentObject()->CreateBone("idle");

				for (EditMeshIt mesh_it = ATools->CurrentObject()->FirstMesh(); mesh_it != ATools->CurrentObject()->LastMesh(); mesh_it++)
				{
					CEditableMesh* pMesh = *mesh_it;
					pMesh->AssignMesh("idle");
				}

				ATools->RealUpdateProperties();
			}
		}
	}

	ImGui::End();


	if (ImGui::Begin(Loc("Item Properties"), 0))
	{
		ImGui::SetNextItemOpen(true, ImGuiCond_Once);
		ImGui::BeginGroup();
			ATools->m_Props->Draw();
		ImGui::EndGroup();
	}

	ImGui::End();
}

void UILeftBarForm::SetRenderMode(bool bEngineMode)
{
	if (ATools->IsVisualPresent() && bEngineMode)
		m_RenderMode = Render_Engine;
	else 		
		m_RenderMode = Render_Editor;
	ATools->PlayMotion();
}
