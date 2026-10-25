#include "stdafx.h"
#include "../../../xrEUI/EditorLocalization.h"
UIFogVolTool::UIFogVolTool()
{
}

UIFogVolTool::~UIFogVolTool()
{
}

void UIFogVolTool::Draw()
{
	ImGui::SetNextItemOpen(true, ImGuiCond_FirstUseEver);
	if (ImGui::TreeNode(Loc("Commands")))
	{
		ImGui::Unindent(ImGui::GetTreeNodeToLabelSpacing());
		ImGui::PushItemWidth(-1);
		float size = float(ImGui::CalcItemWidth());
		{
			if (ImGui::Button(Loc("Group Selected"), ImVec2(size / 2, 0)))ParentTools->GroupSelected();
			ImGui::SameLine(0, 2);
			if (ImGui::Button(Loc("UnGroup Selected"), ImVec2(size / 2, 0)))ParentTools->UnGroupCurrent();
		}

		ImGui::Indent(ImGui::GetTreeNodeToLabelSpacing());
		ImGui::TreePop();
	}
}
