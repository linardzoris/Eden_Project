//---------------------------------------------------------------------------
#include "stdafx.h"
#include "UIEnvGeneratorForm.h"
//---------------------------------------------------------------------------

static constexpr float kRad2Deg = 57.29577951f;
static constexpr float kDeg2Rad = 0.0174532925f;

UIEnvGeneratorForm* UIEnvGeneratorForm::Form = nullptr;

UIEnvGeneratorForm::UIEnvGeneratorForm()
{
	Gen = new EEnvGenerator();
}

UIEnvGeneratorForm::~UIEnvGeneratorForm()
{
	if (m_HeatPreview)
		IM_TEXTURE_RELEASE(m_HeatPreview);

	xr_delete(Gen);
}

void UIEnvGeneratorForm::Show()
{
	if (!Form)
		Form = new UIEnvGeneratorForm();

	Form->CollectTargets();

	if (Form->Targets.empty())
	{
		ELog.DlgMsg(mtInformation, mbOK, "An object or terrain must be selected!");
		return;
	}

	Form->bOpen = true;
}

void UIEnvGeneratorForm::Update()
{
	if (!Form)
		return;

	if (Form->m_ChooseChannel >= 0)
	{
		bool change = false;
		SStringVec lst;
		if (UIChooseForm::GetResult(change, lst))
		{
			if (change)
			{
				EEnvGenerator::SChannel& C = Form->Gen->Channels[Form->m_ChooseChannel];
				C.Objects.clear();
				for (xr_string& str : lst)
					C.Objects.push_back(shared_str(str.c_str()));
				C.ObjectsStr = _ListToSequence(C.Objects);
			}
			Form->m_ChooseChannel = -1;
		}
		UIChooseForm::Update();
	}

	if (Form->IsClosed())
		return;		// keep instance alive so settings persist for the session

	ImGui::SetNextWindowSize(ImVec2(470, 640), ImGuiCond_FirstUseEver);
	if (ImGui::Begin("Environment Generator", &Form->bOpen))
		Form->Draw();
	ImGui::End();
}

void UIEnvGeneratorForm::CollectTargets()
{
	Targets.clear();
	bTerrainTarget = false;

	ESceneCustomOTool* st = Scene->GetOTool(OBJCLASS_SCENEOBJECT);
	if (st)
	{
		for (CCustomObject* object : st->GetObjects())
		{
			if (object->Selected())
			{
				Targets.push_back(object);
				if (EEnvGenerator::IsTerrainObject(object))
					bTerrainTarget = true;
			}
		}
	}

	ESceneCustomOTool* tt = Scene->GetOTool(OBJCLASS_TERRAIN);
	if (tt)
	{
		for (CCustomObject* object : tt->GetObjects())
		{
			if (object->Selected())
			{
				Targets.push_back(object);
				bTerrainTarget = true;
			}
		}
	}
}

void UIEnvGeneratorForm::Draw()
{
	CollectTargets();
	Gen->SummarizeTargets(Targets, Gen->HeatmapReady(), TotalArea, Estimate);

	ImGui::Text("Targets: %d%s", (int)Targets.size(), bTerrainTarget ? " (terrain)" : "");
	ImGui::Text("Area: %.1f m^2   Estimated: %d", TotalArea, Estimate);
	ImGui::Separator();

	// presets
	if (!PresetFile.empty())
		ImGui::TextColored(ImVec4(1.f, 1.f, 0.7f, 1.f), PresetFile.c_str());

	if (ImGui::Button("Load"))
	{
		xr_string outfile;
		if (EFS.GetOpenName("$server_data_root$", outfile, false, 0, -1, "*.env"))
		{
			if (Gen->LoadPreset(outfile.data()))
			{
				xr_path file = outfile;
				PresetFile = file.xfilename();
				RefreshPreview();
			}
		}
	}
	ImGui::SameLine();

	if (ImGui::Button("Save"))
	{
		xr_string outfile;
		if (EFS.GetSaveName("$server_data_root$", outfile, 0, -1, "*.env"))
		{
			if (!outfile.ends_with(".env"))
				outfile += ".env";

			if (Gen->SavePreset(outfile.data()))
			{
				xr_path file = outfile;
				PresetFile = file.xfilename();
			}
		}
	}

	ImGui::Separator();

	// global density cap
	ImGui::DragFloat("Max Density", &Gen->MaxDensity, 0.001f, 0.0001f, 100.f, "%.4f");
	ImGui::TextDisabled("obj/m^2, per-channel upper bound");
	ImGui::Separator();

	// RGBA channels
	static const ImVec4 ch_colors[EEnvGenerator::CH_COUNT] =
	{
		ImVec4(0.95f, 0.30f, 0.30f, 1.f),
		ImVec4(0.35f, 0.85f, 0.40f, 1.f),
		ImVec4(0.40f, 0.55f, 0.95f, 1.f),
		ImVec4(0.85f, 0.85f, 0.85f, 1.f),
	};
	static const char* ch_names[EEnvGenerator::CH_COUNT] = { "R", "G", "B", "A" };

	for (int i = 0; i < EEnvGenerator::CH_COUNT; ++i)
		DrawChannel(i, ch_names[i], ch_colors[i]);

	DrawHeatmapSection();
	DrawTransformSection();

	ImGui::Separator();

	if (ImGui::Button("Generate", ImVec2(ImGui::GetContentRegionAvail().x, 0)))
		OnGenerateClick();
}

void UIEnvGeneratorForm::DrawChannel(int idx, const char* label, const ImVec4& color)
{
	EEnvGenerator::SChannel& C = Gen->Channels[idx];
	bool active = (idx == EEnvGenerator::CH_R) || (bTerrainTarget && Gen->HeatmapReady());

	ImGui::PushID(idx);

	ImGui::PushStyleColor(ImGuiCol_Text, color);
	bool open = ImGui::TreeNode(label);
	ImGui::PopStyleColor();

	if (open)
	{
		ImGui::Text("Objects: %s", C.Objects.empty() ? "(empty)" : *C.ObjectsStr);

		if (ImGui::SmallButton("Edit.."))
		{
			UIChooseForm::SelectItem(smObject, 512, 0);
			m_ChooseChannel = idx;
		}
		ImGui::SameLine();
		if (ImGui::SmallButton("Clear"))
		{
			C.Objects.clear();
			C.ObjectsStr = "";
		}

		ImGui::DragFloat("Density", &C.Density, 0.001f, 0.f, Gen->MaxDensity, "%.4f");
		ImGui::TextDisabled("%.1f objects estimated", TotalArea * Gen->ClampDensity(C));

		if (!active)
			ImGui::TextDisabled("inactive (needs terrain + heatmap)");

		ImGui::TreePop();
	}

	ImGui::PopID();
}

void UIEnvGeneratorForm::DrawHeatmapSection()
{
	if (!ImGui::CollapsingHeader("Heatmap", ImGuiTreeNodeFlags_DefaultOpen))
		return;

	if (!bTerrainTarget)
	{
		if (ImGui::Button("Load Heat.."))
			ELog.DlgMsg(mtInformation, mbOK, "Invalid, target is not Terrain!");
		ImGui::TextDisabled("Requires a terrain selection.");
		return;
	}

	if (ImGui::Button("Load Heat.."))
	{
		xr_string outfile;
		if (EFS.GetOpenName("$server_data_root$", outfile, false, NULL, 0, "*.tga; *.png; *.dds; *.jpg; *.jpeg; *.bmp"))
		{
			if (Gen->LoadHeatmap(outfile.data()))
				RefreshPreview();
			else
				ELog.DlgMsg(mtError, "Can't load heatmap '%s'!", outfile.data());
		}
	}

	if (Gen->HeatmapReady())
	{
		ImGui::SameLine();
		if (ImGui::Button("Clear"))
		{
			Gen->UnloadHeatmap();
			RefreshPreview();
		}

		ImGui::TextWrapped("%s  (%dx%d)", Gen->HeatmapFile.c_str(), Gen->HeatmapW, Gen->HeatmapH);
		ImGui::Image(m_HeatPreview, ImVec2(128, 128));
		ImGui::TextDisabled("Channel value (0..255) scales local density.");
	}
	else
	{
		ImGui::TextWrapped("No heatmap loaded. Only the R channel is active.");
	}
}

void UIEnvGeneratorForm::DrawTransformSection()
{
	if (!ImGui::CollapsingHeader("Random Transform", ImGuiTreeNodeFlags_DefaultOpen))
		return;

	bool rot = Gen->Flags.is(EEnvGenerator::flRandomRotation);
	if (ImGui::Checkbox("Random Rotation", &rot))
		Gen->Flags.set(EEnvGenerator::flRandomRotation, rot);

	if (rot)
	{
		float rmin[3] = { Gen->MinRotation.x * kRad2Deg, Gen->MinRotation.y * kRad2Deg, Gen->MinRotation.z * kRad2Deg };
		float rmax[3] = { Gen->MaxRotation.x * kRad2Deg, Gen->MaxRotation.y * kRad2Deg, Gen->MaxRotation.z * kRad2Deg };

		if (ImGui::DragFloat3("Min Rotation", rmin, 0.5f, -360.f, 360.f, "%.1f"))
			Gen->MinRotation.set(rmin[0] * kDeg2Rad, rmin[1] * kDeg2Rad, rmin[2] * kDeg2Rad);
		if (ImGui::DragFloat3("Max Rotation", rmax, 0.5f, -360.f, 360.f, "%.1f"))
			Gen->MaxRotation.set(rmax[0] * kDeg2Rad, rmax[1] * kDeg2Rad, rmax[2] * kDeg2Rad);
	}

	bool scl = Gen->Flags.is(EEnvGenerator::flRandomScale);
	if (ImGui::Checkbox("Random Scale", &scl))
		Gen->Flags.set(EEnvGenerator::flRandomScale, scl);

	if (scl)
	{
		bool prop = Gen->Flags.is(EEnvGenerator::flScaleProportional);
		if (ImGui::Checkbox("Proportional", &prop))
			Gen->Flags.set(EEnvGenerator::flScaleProportional, prop);

		ImGui::DragFloat3("Min Scale", (float*)&Gen->MinScale, 0.005f, 0.001f, 1000.f, "%.3f");
		ImGui::DragFloat3("Max Scale", (float*)&Gen->MaxScale, 0.005f, 0.001f, 1000.f, "%.3f");
	}
}

void UIEnvGeneratorForm::RefreshPreview()
{
	if (m_HeatPreview)
	{
		IM_TEXTURE_RELEASE(m_HeatPreview);
		m_HeatPreview = nullptr;
	}

	if (!Gen->HeatmapReady())
		return;

	// downscale to <=256 px for the preview thumbnail
	u32 pw = Gen->HeatmapW;
	u32 ph = Gen->HeatmapH;
	while (pw > 256 || ph > 256)
	{
		pw = std::max(1u, pw / 2);
		ph = std::max(1u, ph / 2);
	}

	xr_vector<u32> scaled;
	scaled.resize(pw * ph);
	for (u32 y = 0; y < ph; ++y)
	{
		u32 sy = y * Gen->HeatmapH / ph;
		for (u32 x = 0; x < pw; ++x)
			scaled[y * pw + x] = Gen->Heatmap[sy * Gen->HeatmapW + (x * Gen->HeatmapW / pw)];
	}

	ID3DTexture2D* texture = nullptr;
	R_CHK(REDevice->CreateTexture(pw, ph, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, 0));
	{
		D3DLOCKED_RECT rect;
		R_CHK(texture->LockRect(0, &rect, 0, D3DLOCK_DISCARD));
		for (u32 y = 0; y < ph; ++y)
			memcpy((u8*)rect.pBits + rect.Pitch * y, &scaled[pw * y], pw * 4);
		R_CHK(texture->UnlockRect(0));
	}

	m_HeatPreview = texture;
}

void UIEnvGeneratorForm::OnGenerateClick()
{
	if (Targets.empty())
	{
		ELog.DlgMsg(mtInformation, mbOK, "An object or terrain must be selected!");
		return;
	}

	bool heat_ready = Gen->HeatmapReady();

	float area = 0.f;
	int estimate = 0;
	Gen->SummarizeTargets(Targets, heat_ready, area, estimate);

	if (estimate <= 0)
	{
		ELog.DlgMsg(mtInformation, mbOK, "Nothing to generate: check channel object lists and densities.");
		return;
	}

	if (ELog.DlgMsg(mtConfirmation, mbYes | mbNo, "Target area size: %.1f; Estimated generation: %d, proceed?", area, estimate) != mrYes)
		return;

	int placed = Gen->Generate(Targets, heat_ready, estimate);
	ELog.Msg(mtInformation, "Environment generator: %d objects created.", placed);
}
