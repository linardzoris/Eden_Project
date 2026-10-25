#include "stdafx.h"
#include "../../xrEUI/EditorLocalization.h"

UILPropertiesForm::UILPropertiesForm()
{
}

UILPropertiesForm::~UILPropertiesForm()
{
}

void UILPropertiesForm::Draw()
{
	if (bOpen)
	{
		if (ImGui::Begin(Loc("Properties"), &bOpen))
		{
			if (LTools->PropUpdateIsCompleted)
			{
				LTools->GetProperties()->Draw();
			}
			else
			{
				ImGui::Text(Loc("Async loading..."));
			}
		}
		ImGui::End();
	}
}