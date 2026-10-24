#pragma once

// Editor UI localization: translates hardcoded English UI labels through
// gamedata\configs\text\<lang>\editor_strings.xml, where each <string id="...">]
// carries the original source label as the id. The language comes from
// engine_external.ltx [localization] EditorsLanguage; an empty value disables
// translation and returns the original label.

#include "../../xrCore/EngineExternal.h"
#include "../../xrCore/FormatParsers/XML/xrXMLParser.h"

#include <map>

namespace EditorLocalization
{
	inline std::map<xr_string, xr_string>& string_table()
	{
		static std::map<xr_string, xr_string> Table;
		return Table;
	}

	inline bool& table_ready()
	{
		static bool Ready = false;
		return Ready;
	}

	inline void InitEditorLocalization()
	{
		if (table_ready())
			return;
		table_ready() = true;

		const xr_string lang = EngineExternal().GetEditorsLanguage();
		if (lang.empty())
			return;

		string_path rel;
		xr_sprintf(rel, sizeof(rel), "text\\%s", lang.c_str());

		CXml xml;
		if (!xml.Load(CONFIG_PATH, rel, "editor_strings.xml"))
			return;

		const int count = xml.GetNodesNum(xml.GetRoot(), "string");
		for (int i = 0; i < count; i++)
		{
			XML_NODE* node = xml.NavigateToNode(xml.GetRoot(), "string", i);
			if (!node)
				continue;
			LPCSTR id = xml.ReadAttrib(node, "id", "");
			LPCSTR value = xml.Read(node, "");
			if (id && *id && value && *value)
				string_table()[xr_string(id)] = xr_string(value);
		}
	}

	inline const char* Loc(const char* key)
	{
		InitEditorLocalization();
		const auto it = string_table().find(key);
		return it == string_table().end() ? key : it->second.c_str();
	}

	inline void ShutdownEditorLocalization()
	{
		string_table().clear();
		table_ready() = false;
	}
}

using EditorLocalization::Loc;
using EditorLocalization::InitEditorLocalization;
using EditorLocalization::ShutdownEditorLocalization;
