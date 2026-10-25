#include "stdafx.h"
#include "EditorLocalization.h"
#include "../../xrCore/EngineExternal.h"
#include "../../xrCore/FormatParsers/XML/xrXMLParser.h"
 
static xr_map<xr_string, xr_string>* EditorStringTable = nullptr;
static xr_string EditorLanguage;
 
static void LoadEditorLanguageTable(const char* Language, bool IsFallback)
{
	FS_FileSet Files;
	string_path Mask = {};
	xr_sprintf(Mask, "text\\%s\\editor_*.xml", Language);
	FS.file_list(Files, "$game_config$", FS_ListFiles, Mask);
	Msg("* Editor localization: file_list '%s' -> %d files", Mask, (int)Files.size());

	for (const FS_File& File : Files)
	{
		string_path Name = {}, Ext = {}, FileName = {};
		_splitpath(File.name.c_str(), 0, 0, Name, Ext);
		xr_strconcat(FileName, Name, Ext);

		string_path Dir = {};
		xr_sprintf(Dir, "text\\%s", Language);

		CXml Xml;
		if (!Xml.Load(CONFIG_PATH, Dir, FileName))
		{
			Msg("* Editor localization: CXml load FAILED: text\\%s\\%s", Language, FileName);
			continue;
		}

		int StringCount = Xml.GetNodesNum(Xml.GetRoot(), "string");
		Msg("* Editor localization: %s -> %d strings", FileName, StringCount);
		for (int i = 0; i < StringCount; ++i)
		{
			LPCSTR Id = Xml.ReadAttrib(Xml.GetRoot(), "string", i, "id", nullptr);
			LPCSTR Text = Xml.Read(Xml.GetRoot(), "string:text", i, nullptr);

			if (!Id || !Text)
			{
				Msg("* Editor localization: entry %d skipped (id=%s, text=%s)", i, Id ? Id : "<null>", Text ? Text : "<null>");
				continue;
			}

			xr_string Key = Id;
			if (IsFallback && EditorStringTable->contains(Key))
				continue;

			(*EditorStringTable)[Key] = Text;
		}
	}
}
 
void InitEditorLocalization()
{
	if (EditorStringTable != nullptr)
		return;
 
	EditorStringTable = new xr_map<xr_string, xr_string>();
 
	// Language from engine_external.ltx [localization] EditorsLanguage, "eng" by default
	EditorLanguage = EngineExternal().GetEditorsLanguage();
	if (EditorLanguage.empty())
	{
		EditorLanguage = "eng";
	}
 
	LoadEditorLanguageTable(EditorLanguage.c_str(), false);
 
	const char* Fallback = EngineExternal().GetPreferredFallbackLanguage();
	if (Fallback && *Fallback && EditorLanguage != Fallback)
	{
		LoadEditorLanguageTable(Fallback, true);
	}
 
	Msg("* Editor localization: language '%s', %u entries", EditorLanguage.c_str(), (u32)EditorStringTable->size());
}
 
void ShutdownEditorLocalization()
{
	xr_delete(EditorStringTable);
	EditorLanguage.clear();
}
 
const char* GetEditorLanguage()
{
	return EditorLanguage.c_str();
}
 
const char* Loc(const char* Key)
{
	if (Key == nullptr || Key[0] == '\0' || Key[0] == '#')
		return Key;
 
	InitEditorLocalization();
 
	if (EditorStringTable->empty())
		return Key;
 
	const char* Sep = strstr(Key, "##");
	xr_string Id = Sep ? xr_string(Key, Sep - Key) : xr_string(Key);
 
	auto It = EditorStringTable->find(Id);
	if (It == EditorStringTable->end())
		return Key;
 
	if (Sep == nullptr)
		return It->second.c_str();
 
	// Keep the "##id" suffix so ImGui widget IDs stay stable,
	// use a small ring buffer so several Loc() calls in one
	// expression don't overwrite each other
	static thread_local xr_vector<xr_string> Ring = []()
	{
		xr_vector<xr_string> R;
		R.resize(64);
		return R;
	}();
	static thread_local u32 RingIndex = 0;
 
	xr_string& Result = Ring[RingIndex];
	RingIndex = (RingIndex + 1) % Ring.size();
	Result = It->second;
	Result += Sep;
	return Result.c_str();
}