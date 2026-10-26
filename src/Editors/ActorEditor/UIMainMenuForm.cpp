#include "stdafx.h"
#include "CustomTools/UIPostProcess.h"
#include "../xrEUI/xrUITheme.h"
#include "../xrEUI/imgui_EditorEx.h"
#include "../xrEUI/EditorLocalization.h"
#include "UILodEditorForm.h"
#include "UIThmEditorForm.h"
#include "UIOmfEditorForm.h"
#include "UIOggParamsForm.h"
#include "UIDbArchiveForm.h"
UIMainMenuForm::UIMainMenuForm()
{
}

UIMainMenuForm::~UIMainMenuForm()
{
}

void UIMainMenuForm::Draw()
{
	if (IXBeginMainMenuBar())
	{
		if (ImGui::BeginMenu(Loc("File")))
		{
			if (ImGui::MenuItem(Loc("Clear"), "")) { ExecCommand(COMMAND_CLEAR); }
			if (ImGui::MenuItem(Loc("Load"), "")) { ExecCommand(COMMAND_LOAD); }
			if (ImGui::MenuItem(Loc("Save"), "")) { ExecCommand(COMMAND_SAVE, xr_string(ATools->m_LastFileName.c_str()), 0); }
			if (ImGui::MenuItem(Loc("Save as ..."), "")) { ExecCommand(COMMAND_SAVE,0, 1); }
			ImGui::Separator();
			if (ImGui::BeginMenu(Loc("Open Recent"), ""))
			{
				for (auto& str : EPrefs->scene_recent_list)
				{
					if (ImGui::MenuItem(str.c_str(), "")) { ExecCommand(COMMAND_LOAD, str); }
				}
				ImGui::EndMenu();
			}
			ImGui::Separator();
			if (ImGui::MenuItem(Loc("Import..."), "")) { ExecCommand(COMMAND_IMPORT); }
			ImGui::Separator();
			if (ImGui::MenuItem(Loc("Optimize Motions"), "")) { ExecCommand(COMMAND_OPTIMIZE_MOTIONS); }
			ImGui::Separator();
			if (ImGui::MenuItem(Loc("Batch Convert..."), "")) { ExecCommand(COMMAND_BATCH_CONVERT); }
			ImGui::Separator();
			if (ImGui::BeginMenu(Loc("Export")))
			{
				if (ImGui::MenuItem(Loc("Export OGF..."), "")) { ExecCommand(COMMAND_EXPORT_OGF); }
				if (ImGui::MenuItem(Loc("Export OMF..."), "")) { ExecCommand(COMMAND_EXPORT_OMF); }
				if (ImGui::MenuItem(Loc("Export OBJ..."), "")) { ExecCommand(COMMAND_EXPORT_OBJ); }
				if (ImGui::MenuItem(Loc("Export DM..."), "")) { ExecCommand(COMMAND_EXPORT_DM); }
				ImGui::EndMenu();
			}
			ImGui::Separator();
			if (ImGui::MenuItem(Loc("Quit"), "Alt+F4")) { ExecCommand(COMMAND_QUIT); }
			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu(Loc("Editors")))
		{
			if (ImGui::BeginMenu(Loc("Image")))
			{
				if (ImGui::MenuItem(Loc("Image Editor"), "")) { ExecCommand(COMMAND_IMAGE_EDITOR); }
				ImGui::Separator();
				if (ImGui::MenuItem(Loc("Synchronize Textures"), "")) { ExecCommand(COMMAND_REFRESH_TEXTURES); }
				if (ImGui::MenuItem(Loc("Check New Textures"), "")) { ExecCommand(COMMAND_CHECK_TEXTURES); }
				ImGui::EndMenu();
			}
			if (ImGui::BeginMenu(Loc("Sounds")))
			{
				if (ImGui::MenuItem(Loc("Sound Editor"), "")) { ExecCommand(COMMAND_SOUND_EDITOR); }
				ImGui::Separator();
				if (ImGui::MenuItem(Loc("Synchronize Sounds (Soft)"), "")) { ExecCommand(COMMAND_SYNC_SOUNDS); }
				if (ImGui::MenuItem(Loc("Synchronize Sounds (Hard)"), "")) { ExecCommand(COMMAND_SYNC_SOUNDS_HARD); }
				ImGui::EndMenu();
			}
			if (ImGui::MenuItem(Loc("LOD Editor"), "")) { UILodEditorForm::Show(); }
			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu(Loc("Tools")))
		{
			if (ImGui::MenuItem(Loc("THM Editor"), "")) { UIThmEditorForm::Show(); }
			if (ImGui::MenuItem(Loc("OMF Editor"), "")) { UIOmfEditorForm::Show(); }
			if (ImGui::MenuItem(Loc("OGG Sound Params"), "")) { UIOggParamsForm::Show(); }
			if (ImGui::MenuItem(Loc("DB Archive Tool"), "")) { UIDbArchiveForm::Show(); }
			ImGui::EndMenu();
		}


		if (ImGui::BeginMenu(Loc("Options")))
		{
			if (ImGui::BeginMenu(Loc("Render")))
			{
				if (ImGui::BeginMenu(Loc("Quality")))
				{
					static bool selected[4] = { false,false,true,false };
					if (ImGui::MenuItem("25%", "", &selected[0]))
					{
						selected[1] = selected[2] = selected[3] = false;
						UI->SetRenderQuality(1 / 4.f);
						UI->RedrawScene();
					}
					if (ImGui::MenuItem("50%", "", &selected[1]))
					{
						selected[0] = selected[2] = selected[3] = false;
						UI->SetRenderQuality(1 / 2.f);
						UI->RedrawScene();
					}
					if (ImGui::MenuItem("100%", "", &selected[2]))
					{
						selected[1] = selected[0] = selected[3] = false;
						UI->SetRenderQuality(1.f);
						UI->RedrawScene();
					}
					if (ImGui::MenuItem("200%", "", &selected[3]))
					{
						selected[1] = selected[2] = selected[0] = false;
						UI->SetRenderQuality(2.f);
						UI->RedrawScene();
					}
					ImGui::EndMenu();
				}
				if (ImGui::BeginMenu(Loc("Fill Mode")))
				{
					bool selected[3] = { EDevice->dwFillMode == D3DFILL_POINT,EDevice->dwFillMode == D3DFILL_WIREFRAME,EDevice->dwFillMode == D3DFILL_SOLID };
					if (ImGui::MenuItem(Loc("Point"), "", &selected[0]))
					{
						EDevice->dwFillMode = D3DFILL_POINT;
						UI->RedrawScene();
					}
					if (ImGui::MenuItem(Loc("Wireframe"), "", &selected[1]))
					{
						EDevice->dwFillMode = D3DFILL_WIREFRAME;
						UI->RedrawScene();
					}
					if (ImGui::MenuItem(Loc("Solid"), "", &selected[2]))
					{
						EDevice->dwFillMode = D3DFILL_SOLID;
						UI->RedrawScene();
					}
					ImGui::EndMenu();
				}
				//if (ImGui::BeginMenu("Shader Mode"))
				//{
				//	bool selected[2] = { EDevice->dwShadeMode == D3DSHADE_FLAT,EDevice->dwShadeMode == D3DSHADE_GOURAUD };
				//	if (ImGui::MenuItem("Flat", "", &selected[0]))
				//	{
				//		EDevice->dwShadeMode = D3DSHADE_FLAT;
				//		UI->RedrawScene();
				//	}
				//	if (ImGui::MenuItem("Gouraud", "", &selected[1]))
				//	{
				//		EDevice->dwShadeMode = D3DSHADE_GOURAUD;
				//		UI->RedrawScene();
				//	}
				//	ImGui::EndMenu();
				//}
				{
					bool selected = psDeviceFlags.test(rsEdgedFaces);
					if (ImGui::MenuItem(Loc("Edged Faces"), "", &selected))
					{
						psDeviceFlags.set(rsEdgedFaces, selected);
						UI->RedrawScene();
					}
				}
				ImGui::Separator();
				{
					bool selected = !Caps.bForceGPU_SW;
					if (ImGui::MenuItem(Loc("RenderHW"), "", &selected))
					{
						Caps.bForceGPU_SW = !selected;
						UI->Resize();
					}
				}
				//ImGui::Separator();
				//{
				//	bool selected = psDeviceFlags.test(rsFilterLinear);
				//	if (ImGui::MenuItem("Filter Linear", "", &selected))
				//	{
				//		psDeviceFlags.set(rsFilterLinear, selected);
				//		UI->RedrawScene();
				//	}
				//}
				//{
				//	bool selected = psDeviceFlags.test(rsRenderTextures);
				//	if (ImGui::MenuItem("Textures", "", &selected))
				//	{
				//		psDeviceFlags.set(rsRenderTextures, selected);
				//		UI->RedrawScene();
				//	}
				//}
				ImGui::EndMenu();
			}
			ImGui::Separator();
			{
				bool selected = psDeviceFlags.test(rsDrawSafeRect);
				if (ImGui::MenuItem(Loc("Draw Safe Rect"), "", &selected))
				{
					psDeviceFlags.set(rsDrawSafeRect, selected);
					UI->RedrawScene();
				}
			}
			{
				bool selected = psDeviceFlags.test(rsDrawGrid);
				if (ImGui::MenuItem(Loc("Draw Grid"), "", &selected))
				{
					psDeviceFlags.set(rsDrawGrid, selected);
					UI->RedrawScene();
				}
			}
			{
				if (ImGui::BeginMenu(Loc("Coordinate Axes")))
				{
					bool disabled = psDeviceFlags.test(rsDisableAxisCube);

					if (ImGui::MenuItem(Loc("None"), "", &disabled))
					{
						psDeviceFlags.set(rsDisableAxisCube, disabled);
					}

					ImGui::BeginDisabled(disabled);

					bool selected_a = false;
					bool selected_c = false;

					(!psDeviceFlags.test(rsDrawAxis) ? selected_c : selected_a) = true;

					if (ImGui::MenuItem(Loc("Axis"), "", &selected_a))
					{
						psDeviceFlags.set(rsDrawAxis, true);
					}
					if (ImGui::MenuItem(Loc("Cube"), "", &selected_c))
					{
						psDeviceFlags.set(rsDrawAxis, false);
					}

					ImGui::EndDisabled();
					ImGui::EndMenu();
				}
			}
			ImGui::Separator();
			{
				bool selected = psDeviceFlags.test(rsFog);
				if (ImGui::MenuItem(Loc("Fog"), "", &selected))
				{
					psDeviceFlags.set(rsFog, selected);
					UI->RedrawScene();
				}
			}
			{
				bool selected = psDeviceFlags.test(rsMuteSounds);
				if (ImGui::MenuItem(Loc("Mute Sounds"), "", &selected))
				{
					psDeviceFlags.set(rsMuteSounds, selected);
				}
			}
			{
				bool selected = psDeviceFlags.test(rsRenderRealTime);
				if (ImGui::MenuItem(Loc("Real Time"), "", &selected))
				{
					psDeviceFlags.set(rsRenderRealTime, selected);
				}
			}
			ImGui::Separator();
			{
				bool selected = psDeviceFlags.test(rsStatistic);
				if (ImGui::MenuItem(Loc("Stats"), "", &selected)) { psDeviceFlags.set(rsStatistic, selected);  UI->RedrawScene(); }

			}
			ImGui::Separator();
			if (ImGui::MenuItem(Loc("Preferences"), "")) { ExecCommand(COMMAND_EDITOR_PREF); }
			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu(Loc("Windows")))
		{
			{
				bool selected = AllowLogCommands();
				auto &PPE = CMainPPE::Instance();
				if (ImGui::MenuItem(Loc("Log"), "", &selected))
				{
					ExecCommand(COMMAND_LOG_COMMANDS);
				}

				if (ImGui::MenuItem(Loc("Post Process"), nullptr))
				{
					PPE.OpenState() = true;
				}

				CUIThemeManager& ThemeInstance = CUIThemeManager::Get();
				bool selected2 = !ThemeInstance.IsClosed();
				if (ImGui::MenuItem(Loc("Theme"), "", &selected2))
				{
					if (selected2)
					{
						if (!UI->HasWindow<CUIThemeManager>())
						{
							UI->Push(&ThemeInstance);
						}
						ThemeInstance.Show(true);
					}
					else
						ThemeInstance.Show(false);
				}
			}
			ImGui::EndMenu();
		}

		IXEndMainMenuBar();
	}
}
