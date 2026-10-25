#include "stdafx.h"
#include "../../xrEUI/EditorLocalization.h"

UIWorldPropertiesFrom::UIWorldPropertiesFrom()
{
}

UIWorldPropertiesFrom::~UIWorldPropertiesFrom()
{
}

void UIWorldPropertiesFrom::Draw()
{
	if (bOpen)
	{
		if (ImGui::Begin(Loc("World Properties"), &bOpen))
		{
			LTools->GetWorldProperties()->Draw();
		}
		ImGui::End();
	}
}
