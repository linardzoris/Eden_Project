#include "stdafx.h"
#include "../../../xrEUI/EditorLocalization.h"
UISectorTool::UISectorTool()
{
	m_Edit = false;
    m_CreateNewMultiple = false;
    m_CreateNewSingle = false;
    m_MeshAdd = true;
    m_BoxPick = false;
}

UISectorTool::~UISectorTool()
{
}
void UISectorTool::Draw()
{
    ImGui::SetNextItemOpen(true, ImGuiCond_FirstUseEver);
    if (ImGui::TreeNode(Loc("Command")))
    {
        ImGui::Unindent(ImGui::GetTreeNodeToLabelSpacing());
        ImGui::SetNextItemWidth(-1);
        float size = float(ImGui::CalcItemWidth());
        {
            if (ImGui::Button(Loc("Validate Sectors"), ImVec2(size / 2, 0)))
            {
                PortalUtils.Validate(true);
            }
            ImGui::SameLine(0, 2);
            if (ImGui::Button(Loc("Capture Volume"), ImVec2(size / 2, 0)))
            {
                CSector* S = PortalUtils.GetSelectedSector();
                if (S) {
                    S->CaptureInsideVolume();
                    Scene->UndoSave();
                }
            }

            ImGui::Separator();
            if (ImGui::Button(Loc("Recalculate Portals"), ImVec2(size / 2, 0)))
            {
                int Size = PortalUtils.CalculateAllPortals();
                if (Size > 0)
                {
                    ELog.DlgMsg(mtInformation, Loc("Recalculated %d portals."), Size);
                }
                else
                {
                    ELog.DlgMsg(mtInformation, Loc("Recalculated portals error! Portals is empty..."));
                }
            }           
            ImGui::SameLine(0, 2);
            if (ImGui::Button(Loc("Distribute Objects"), ImVec2(size / 2, 0)))
            {
                CSector* S = PortalUtils.GetSelectedSector();
                if (S) {
                    S->DistributeInsideObjects();
                    Scene->UndoSave();
                }
            }

            ImGui::Separator();
            if (ImGui::Button(Loc("Create Default"), ImVec2(size / 2, 0)))
            {
                CCustomObject* O = Scene->FindObjectByName(DEFAULT_SECTOR_NAME, OBJCLASS_SECTOR);
                if (O) ELog.DlgMsg(mtInformation, Loc("Default sector already present. Remove this and try again."));
                else {
                    if (!PortalUtils.CreateDefaultSector()) ELog.DlgMsg(mtInformation, Loc("Default can't created."));
                }
            }
            ImGui::SameLine(0, 2);
            if (ImGui::Button(Loc("Remove Default"), ImVec2(size / 2, 0)))
            {
                if (!PortalUtils.RemoveDefaultSector()) ELog.DlgMsg(mtInformation, Loc("Default sector not found."));
            }

        }
        ImGui::Separator();
        ImGui::Indent(ImGui::GetTreeNodeToLabelSpacing());
        ImGui::TreePop();
    }
    if(m_Edit)ImGui::SetNextItemOpen(true, ImGuiCond_FirstUseEver);
    if (m_Edit && ImGui::TreeNode(Loc("Edit")))
    {
        ImGui::Unindent(ImGui::GetTreeNodeToLabelSpacing());
        {
            if (ImGui::Checkbox(Loc("Create New Single (From Mesh)"), &m_CreateNewSingle))
            {
                m_CreateNewMultiple = false;
            }
            if (ImGui::Checkbox(Loc("Create New Multiple (From Object)"), &m_CreateNewMultiple))
            {
                m_CreateNewSingle = false;
            }
            ImGui::Separator();
            ImGui::Text(Loc("Meshes"));
            ImGui::SameLine();
            if (ImGui::RadioButton("M+", m_MeshAdd))
            {
                m_MeshAdd = true;
           }
            ImGui::SameLine();
            if (ImGui::RadioButton("M-", !m_MeshAdd))
            {
                m_MeshAdd = false;
            }
            if (ImGui::Checkbox(Loc("Box Pick"), &m_BoxPick));
            if (m_CreateNewSingle || m_CreateNewMultiple)
            {
                m_BoxPick = false;
            }
        }
        ImGui::Separator();
        ImGui::Indent(ImGui::GetTreeNodeToLabelSpacing());
        ImGui::TreePop();
    }

}
