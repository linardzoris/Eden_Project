#include "stdafx.h"
#include "../xrEUI/xrUITheme.h"
#include "../xrEUI/imgui_EditorEx.h"
#include "../xrEUI/EditorLocalization.h"
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
            if (ImGui::MenuItem(Loc("Save")))
            {
                ExecCommand(COMMAND_SAVE);
            }
            if (ImGui::MenuItem(Loc("Reload")))
            {
                ExecCommand(COMMAND_LOAD);
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(Loc("Editors")))
        {
            if (ImGui::BeginMenu(Loc("Image")))
            {
                // Image Editor and Synchronize Textures entries are hidden intentionally
                if (ImGui::MenuItem(Loc("Cheack New Textures"), "")) { ExecCommand(COMMAND_CHECK_TEXTURES); }
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
                if (ImGui::BeginMenu(Loc("Shader Mode")))
                {
                    bool selected[2] = { EDevice->dwShadeMode == D3DSHADE_FLAT,EDevice->dwShadeMode == D3DSHADE_GOURAUD };
                    if (ImGui::MenuItem(Loc("Flat"), "", &selected[0]))
                    {
                        EDevice->dwShadeMode = D3DSHADE_FLAT;
                        UI->RedrawScene();
                    }
                    if (ImGui::MenuItem(Loc("Gouraud"), "", &selected[1]))
                    {
                        EDevice->dwShadeMode = D3DSHADE_GOURAUD;
                        UI->RedrawScene();
                    }
                    ImGui::EndMenu();
                }
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
                ImGui::Separator();
                {
                    bool selected = psDeviceFlags.test(rsFilterLinear);
                    if (ImGui::MenuItem(Loc("Filter Linear"), "", &selected))
                    {
                        psDeviceFlags.set(rsFilterLinear, selected);
                        UI->RedrawScene();
                    }
                }
                {
                    bool selected = psDeviceFlags.test(rsRenderTextures);
                    if (ImGui::MenuItem(Loc("Textures"), "", &selected))
                    {
                        psDeviceFlags.set(rsRenderTextures, selected);
                        UI->RedrawScene();
                    }
                }
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
            ImGui::Separator();
            {
                bool selected = psDeviceFlags.test(rsFog);
                if (ImGui::MenuItem(Loc("Fog"), "", &selected))
                {
                    psDeviceFlags.set(rsFog, selected);
                    UI->RedrawScene();
                }
            }
            ImGui::Separator();
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

                if (ImGui::MenuItem(Loc("Log"), "", &selected)) { ExecCommand(COMMAND_LOG_COMMANDS); }

                CUIThemeManager& ThemeInstance = CUIThemeManager::Get();
                bool selected2 = !ThemeInstance.IsClosed();
                if (ImGui::MenuItem(Loc("Theme"), "", &selected2))
                {
                    if (selected2)
                    {
                        if (!UI->HasWindow<CUIThemeManager>())
                        {
                            // static-lifetime singleton, never delete it through the UI array
                            UI->Push(&ThemeInstance, false);
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
