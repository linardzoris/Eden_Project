#pragma once
#include "../../Editor/Utils/EnvGenerator.h"

class UIEnvGeneratorForm : public IEditorWnd
{
public:
	static void Show();
	static void Update();

private:
	UIEnvGeneratorForm();

public:
	virtual ~UIEnvGeneratorForm();

private:
	virtual void Draw();

	void CollectTargets();
	void DrawChannel(int idx, const char* label, const ImVec4& color);
	void DrawHeatmapSection();
	void DrawTransformSection();
	void OnGenerateClick();
	void RefreshPreview();

	EEnvGenerator* Gen = nullptr;

	xr_vector<CCustomObject*> Targets;
	bool bTerrainTarget = false;
	float TotalArea = 0.f;
	int Estimate = 0;

	xr_string PresetFile;
	int m_ChooseChannel = -1;

	ImTextureID m_HeatPreview = nullptr;

	static UIEnvGeneratorForm* Form;
};
