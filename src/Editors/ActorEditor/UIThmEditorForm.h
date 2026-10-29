#pragma once
#include "../xrECore/Editor/EThumbnail.h"

// Standalone .thm editor: opens any texture thumbnail file (not restricted to
// the SDK _game_textures_ tree), shows and edits the full STextureParams set
// and writes it back preserving the original THM chunk layout.
//
// Batch mode: "Load..." imports any number of .thm files into the list below
// the toolbar. Entries are selected with plain click (single), ctrl+click
// (toggle) and shift+click (range from the last clicked row). With several
// entries selected every edit is applied to ALL of them; a field whose value
// differs across the selection is tinted and shown as "(mixed)" (combo
// preview). Per-entry dirty tracking: "Save" writes the modified selected
// files, "Save All" (enabled whenever the list is not empty) writes every
// modified file in the list.
class UIThmEditorForm :
	public IEditorWnd
{
public:
	UIThmEditorForm();
	virtual ~UIThmEditorForm();
	virtual void Draw();

	static void Update();
	static void Show();

	static UIThmEditorForm* Form;

private:
	// thm kinds handled by the editor
	enum EThmKind
	{
		KindTexture = 0,   // texture thumbnail, STextureParams payload
		KindObject  = 1,   // object thumbnail, face/vertex counts + preview blob
	};

	struct ThmEntry
	{
		xr_string      path;         // absolute file path
		u32            kind = KindTexture;
		STextureParams params;       // texture kind
		u32            thm_type = 0; // THM_CHUNK_TYPE payload, kept on save
		// object kind
		u32            face_count = 0;
		u32            vertex_count = 0;
		// raw THM_CHUNK_DATA payload (preview image), round-tripped untouched;
		// data_id keeps the original chunk id including CFS_CompressMark
		bool           has_data = false;
		u32            data_id = 0;
		xr_vector<char> blob;
		// dirty flag
		bool           dirty = false;
	};

	bool ParseThm(LPCSTR path, ThmEntry& out);   // read one .thm into out
	bool WriteThm(ThmEntry& e);                  // serialize one entry to disk
	void DoLoad();
	void DoLoadObject();
	void DoSaveSelected(bool save_as);
	void DoSaveAll();
	void ResetEdits();
	void DrawFileList();
	void DrawBasic();
	void DrawFlags();
	void DrawColors();
	void DrawDetail();
	void DrawMaterialBump();
	void DrawObject();

	// ---- selection helpers ----
	bool HasSelection() const { return !m_Selection.empty(); }
	int  PrimaryIndex() const { return m_Selection.empty() ? -1 : m_Selection.front(); }
	bool Selected(int i) const;
	void SelectOnly(int i);
	void ToggleSelect(int i);
	void RangeSelect(int i);

	// applies a field change to every selected entry and marks them dirty
	template <class F> void ForSelected(F&& set)
	{
		for (int i : m_Selection)
		{
			set(m_Entries[i].params);
			m_Entries[i].dirty = true;
		}
	}

	// true when the field value differs across the selection ("mixed")
	template <class F> bool FieldMixed(F&& get) const
	{
		if (m_Selection.size() < 2)
			return false;
		const STextureParams& first = m_Entries[m_Selection.front()].params;
		for (size_t k = 1; k < m_Selection.size(); ++k)
			if (get(first) != get(m_Entries[m_Selection[k]].params))
				return true;
		return false;
	}

	// same for string (shared_str) fields: compare by content, not by pointer
	template <class F> bool FieldMixedStr(F&& get) const
	{
		if (m_Selection.size() < 2)
			return false;
		const STextureParams& first = m_Entries[m_Selection.front()].params;
		for (size_t k = 1; k < m_Selection.size(); ++k)
			if (0 != xr_strcmp(get(first).c_str(), get(m_Entries[m_Selection[k]].params).c_str()))
				return true;
		return false;
	}

	// mixed state for a whole-entry field (face_count, vertex_count, ...)
	template <class F> bool FieldMixedEntry(F&& get) const
	{
		if (m_Selection.size() < 2)
			return false;
		const ThmEntry& first = m_Entries[m_Selection.front()];
		for (size_t k = 1; k < m_Selection.size(); ++k)
			if (get(first) != get(m_Entries[m_Selection[k]]))
				return true;
		return false;
	}

	// true when every selected entry has the given kind
	bool SelectionAll(u32 kind) const
	{
		for (int i : m_Selection)
			if (m_Entries[i].kind != kind)
				return false;
		return true;
	}

	// applies a change to every selected entry of the given kind
	template <class F> void ForSelectedKind(u32 kind, F&& set)
	{
		for (int i : m_Selection)
		{
			if (m_Entries[i].kind != kind)
				continue;
			set(m_Entries[i]);
			m_Entries[i].dirty = true;
		}
	}

	xr_vector<ThmEntry> m_Entries;
	xr_vector<int>      m_Selection;   // sorted indices into m_Entries
	int                 m_Anchor = -1; // last plain/ctrl-clicked row, shift anchor

	// edit mirror of the primary selection (ImGui needs stable writable storage)
	STextureParams m_Params;
	int m_FaceCount = 0;     // object kind mirror
	int m_VertexCount = 0;
	char m_DetailName[256] = "";
	char m_BumpName[256] = "";
	char m_ExtNormalMap[256] = "";
	char m_BorderColor[16] = "";
	char m_FadeColor[16] = "";
};
