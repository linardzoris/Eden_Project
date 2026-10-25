#include "stdafx.h"
#include "../../../xrEUI/EditorLocalization.h"

UIShapeTool::UIShapeTool()
{
    Tool = nullptr;
    m_AttachShape = false;
}

UIShapeTool::~UIShapeTool()
{
}

void UIShapeTool::Draw()
{
    ImGui::SetNextItemOpen(true, ImGuiCond_FirstUseEver);
    if (ImGui::TreeNode(Loc("Commands")))
    {
        ImGui::Unindent(ImGui::GetTreeNodeToLabelSpacing());
        {
            if (ImGui::RadioButton(Loc("Sphere"), m_SphereMode))
            {
                m_SphereMode = true;
            }ImGui::SameLine();
            if (ImGui::RadioButton(Loc("Box"), m_SphereMode==false))
            {
                m_SphereMode = false;
            }
        }
        ImGui::Separator();
        ImGui::Indent(ImGui::GetTreeNodeToLabelSpacing());
        ImGui::TreePop();
    }
    ImGui::SetNextItemOpen(true, ImGuiCond_FirstUseEver);
    if (ImGui::TreeNode(Loc("Edit")))
    {
        ImGui::Unindent(ImGui::GetTreeNodeToLabelSpacing());
        {
            if (ImGui::Checkbox(Loc("Attach Shape..."), &m_AttachShape))
            {
                if(m_AttachShape)
                ExecCommand(COMMAND_CHANGE_ACTION, etaAdd);
            }
            ImGui::SameLine(0, 10);
            if (ImGui::Button(Loc("Detach All"), ImVec2(-1, 0)))
            {
                ObjectList lst;
                if (Scene->GetQueryObjects(lst, OBJCLASS_SHAPE, 1, 1, 0)) {
                    Scene->SelectObjects(false, OBJCLASS_SHAPE);
                    for (ObjectIt it = lst.begin(); it != lst.end(); it++)
                        ((CEditShape*)*it)->Detach();
                }
            }
        }
        ImGui::Separator();
        ImGui::Indent(ImGui::GetTreeNodeToLabelSpacing());
        ImGui::TreePop();
    }
    ImGui::SetNextItemOpen(true, ImGuiCond_FirstUseEver);
    if (ImGui::TreeNode(Loc("Level Bound")))
    {
        ImGui::Unindent(ImGui::GetTreeNodeToLabelSpacing());
        {
            if (ImGui::Checkbox(Loc("Edit Level Bound"), &EditLevelBound))
            {
                if (EditLevelBound)
                    Tool->OnEditLevelBounds(false);
            }
            if(EditLevelBound)
            if (ImGui::Button(Loc("Recalc"), ImVec2(-1, 0)))
            {
                Tool->OnEditLevelBounds(true);
            }
        }
        ImGui::Separator();
        ImGui::Indent(ImGui::GetTreeNodeToLabelSpacing());
        ImGui::TreePop();
    }
}
