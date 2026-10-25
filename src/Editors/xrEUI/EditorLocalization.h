#pragma once

#include <imgui.h>

// XREUI_API export guard: xrEUI builds as a DLL (XREUI_EXPORTS defined only
// inside the xrEUI target); consumers get dllimport. Defined here for
// compilation units that do not include xrEUI/stdafx.h.
#ifndef XREUI_API
	#ifdef XREUI_EXPORTS
		#define XREUI_API __declspec(dllexport)
	#else
		#define XREUI_API __declspec(dllimport)
	#endif
#endif
 
// Editor localization support.
//
// Works the same way as the engine CStringTable: UI strings are read from
// gamedata\configs\text\<language>\editor_*.xml files:
//
//	<string_table>
//		<string id="File">文件</string>
//	</string_table>
//
// The id is the original hardcoded English label, so existing editor code
// only needs to wrap its labels with Loc():
//
//	ImGui::BeginMenu("File")  ->  ImGui::BeginMenu(Loc("File"))
//
// or use the ED* wrappers below (EDBeginMenu, EDMenuItem, EDButton, ...).
//
// The language is taken from engine_external.ltx:
//
//	[localization]
//	EditorsLanguage = chn
//
// If a key is missing the original English text is shown, so translations
// can be filled in gradually.
 
// Translates an editor UI label. Handles "label##id" strings by translating
// only the visible part and keeping the "##id" suffix intact.
XREUI_API const char* Loc(const char* Key);
 
XREUI_API void InitEditorLocalization();
XREUI_API void ShutdownEditorLocalization();
XREUI_API const char* GetEditorLanguage();
 
// Localized ImGui wrappers (drop-in replacements for the common text widgets)
inline bool EDMenuItem(const char* label, const char* shortcut = nullptr, bool p_selected = false, bool enabled = true)
{
	return ImGui::MenuItem(Loc(label), shortcut, p_selected, enabled);
}
 
inline bool EDMenuItem(const char* label, const char* shortcut, bool* p_selected, bool enabled = true)
{
	return ImGui::MenuItem(Loc(label), shortcut, p_selected, enabled);
}
 
inline bool EDBeginMenu(const char* label, bool enabled = true)
{
	return ImGui::BeginMenu(Loc(label), enabled);
}
 
inline bool EDButton(const char* label, const ImVec2& size = ImVec2(0, 0))
{
	return ImGui::Button(Loc(label), size);
}
 
inline bool EDCheckbox(const char* label, bool* v)
{
	return ImGui::Checkbox(Loc(label), v);
}
 
inline bool EDCollapsingHeader(const char* label, ImGuiTreeNodeFlags flags = 0)
{
	return ImGui::CollapsingHeader(Loc(label), flags);
}
 
inline bool EDSelectable(const char* label, bool selected = false, ImGuiSelectableFlags flags = 0, const ImVec2& size = ImVec2(0, 0))
{
	return ImGui::Selectable(Loc(label), selected, flags, size);
}
 
inline bool EDTreeNodeEx(const char* label, ImGuiTreeNodeFlags flags = 0)
{
	return ImGui::TreeNodeEx(Loc(label), flags);
}
 
inline void EDText(const char* fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	ImGui::TextV(Loc(fmt), args);
	va_end(args);
}
 
inline void EDTextDisabled(const char* fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	ImGui::TextDisabledV(Loc(fmt), args);
	va_end(args);
}
 
inline bool EDInputTextWithHint(const char* label, const char* hint, char* buf, size_t buf_size, ImGuiInputTextFlags flags = 0)
{
	return ImGui::InputTextWithHint(Loc(label), Loc(hint), buf, buf_size, flags);
}