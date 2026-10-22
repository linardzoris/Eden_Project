//---------------------------------------------------------------------------
#pragma once

// Batch LOD tool: lists MU (multiple usage) objects from the library, lets the
// user multi-select them (flat list + folder tree, kept in sync) and toggle
// "Using LOD" / generate HQ billboards (BLOD) or LQ lod textures (FLOD) for the
// whole selection at once.
class UILodEditorForm :
	public IEditorWnd
{
private:
	struct Item
	{
		xr_string name;        // full relative path with .object extension (for CActorTools::Load)
		xr_string file_name;   // file name only, for display
		xr_string folder;      // directory relative to the objects root
		bool      UsingLOD = false;
		bool      HasHQ = false;
		bool      HasLQ = false;
		bool      selected = false;
	};
	struct FolderGroup
	{
		xr_string        folder;
		xr_vector<int>   indices;
	};

	xr_vector<Item>        items;
	xr_vector<FolderGroup> groups;

	bool     m_ScanDone = false;
	bool     m_MakeHQConfirm = false;   // which action the confirm popup is for
	bool     m_MakeLQConfirm = false;
	bool     m_ClearHQConfirm = false;
	bool     m_ClearLQConfirm = false;
	int      m_LastClickIndex = -1;     // shift+click range anchor
	float    m_EstimateHQ = 4.0f;       // measured seconds per object
	float    m_EstimateLQ = 1.0f;
	char     m_Filter[128] = "";

	void ScanObjects();
	void RescanFS();
	void BuildGroups();
	void ApplyUsingLOD(bool value);
	void RunBatch(bool hq);
	void ClearBatch(bool hq);
	void DrawObjectList();
	void DrawFolderTree();
	void DrawActions();

public:
	UILodEditorForm();
	virtual ~UILodEditorForm();
	virtual void Draw();

	static void Update();
	static void Show();

	static UILodEditorForm* Form;
};
