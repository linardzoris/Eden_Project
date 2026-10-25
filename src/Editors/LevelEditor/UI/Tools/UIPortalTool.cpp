#include "stdafx.h"
#include "../../../xrEUI/EditorLocalization.h"
UIPortalTool::UIPortalTool()
{
}

UIPortalTool::~UIPortalTool()
{
}

void UIPortalTool::Draw()
{
    ImGui::SetNextItemOpen(true, ImGuiCond_FirstUseEver);
    if (ImGui::TreeNode(Loc("Command")))
    {
        ImGui::Unindent(ImGui::GetTreeNodeToLabelSpacing());
        {
            if (ImGui::Button(Loc("Invert Orientation"),ImVec2(-1,0)))
            {
                ObjectList lst;
                if (Scene->GetQueryObjects(lst, OBJCLASS_PORTAL, 1, 1, 0)) {
                    for (ObjectIt it = lst.begin(); it != lst.end(); it++) {
                        CPortal* _O = (CPortal*)*it;
                        _O->InvertOrientation(true);
                    }
                }
            }
            if (ImGui::Button(Loc("Compute All Portals"),ImVec2(-1,0)))
            {
                if (mrYes == ELog.DlgMsg(mtConfirmation, mbYes |mbNo, Loc("Are you sure want to destroy all existing portals and compute them again?")))
                {
                    int cnt = PortalUtils.CalculateAllPortals();
                    if (cnt) ELog.DlgMsg(mtInformation, Loc("Calculated '%d' portal(s)."), cnt);
                }
            }
            if (ImGui::Button(Loc("Compute Sel. Portals"),ImVec2(-1,0)))
            {
                if (mrYes == ELog.DlgMsg(mtConfirmation, mbYes |mbNo, Loc("Are you sure want to destroy all existing portals and compute them again?")))
                {
                    int cnt = PortalUtils.CalculateSelectedPortals();
                    if (cnt) ELog.DlgMsg(mtInformation, Loc("Calculated '%d' portal(s)."), cnt);
                }
            }
            if (ImGui::Button(Loc("Remove Similar"),ImVec2(-1,0)))
            {
                tool->RemoveSimilar();
            }
        }
        ImGui::Separator();
        ImGui::Indent(ImGui::GetTreeNodeToLabelSpacing());
        ImGui::TreePop();
    }
}
