//---------------------------------------------------------------------------
#pragma once

// Batch LOD tool: lists MU (multiple usage) objects from the library, lets the
// user multi-select them and toggle "Using LOD" / generate HQ billboards (BLOD)
// or LQ lod textures (FLOD) for the whole selection at once.
class UILodEditorForm :
	public IEditorWnd
{
private:
	struct Item
	{
		xr_string name;        // full relative path with .object extension (for CActorTools::Load)
		xr_string short_name;  // path relative to the objects root, for display
		bool      UsingLOD = false;
		bool      HasHQ = false;
		bool      selected = false;
	};

	xr_vector<Item> items;
	bool            m_ScanDone = false;
	bool            m_MakeHQ = false;   // transient checkbox states
	bool            m_MakeLQ = false;
	char            m_Filter[128] = "";

	void ScanObjects();
	void ApplyUsingLOD(bool value);
	void RunBatch(bool hq);
	void DrawObjectList();
	void DrawActions();

public:
	UILodEditorForm();
	virtual ~UILodEditorForm();
	virtual void Draw();

	static void Update();
	static void Show();

	static UILodEditorForm* Form;
};
