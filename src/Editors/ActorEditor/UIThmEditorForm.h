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
		KindObject  = 1,   // object thumbnail, preview + companion .object
	};

	// CEditableObject::eo* values (EditObject.h), duplicated locally
	enum EObjectFlags
	{
		eofDynamic       = (1 << 0),
		eofProgressive   = (1 << 1),
		eofUsingLOD      = (1 << 2),
		eofHOM           = (1 << 3),
		eofMultipleUsage = (1 << 4),
		eofSoundOccluder = (1 << 5),
		eofHQExport      = (1 << 6),
		eofSkipOpt       = (1 << 7),
	};

	// CSurface::EFlags value (EditObject.h)
	enum ESurfaceFlags
	{
		esf2Sided = (1 << 0),
	};

	// one surface of a .object file (EOBJ_CHUNK_SURFACES3 record)
	struct ObjSurface
	{
		xr_string name;
		xr_string engine_shader;
		xr_string compile_shader;
		xr_string game_mtl;
		xr_string texture;
		xr_string vmap;
		u32       flags = 0;
		u32       fvf = 0;
	};

	// raw body chunk of a .object file, round-tripped untouched (meshes,
	// bones, motions etc.); id keeps the original value incl. CFS_CompressMark
	struct ObjRawChunk
	{
		u32             id = 0;
		xr_vector<char> data;
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
		// decompressed preview (THUMB_SIZE ARGB pixels), shown in the UI
		xr_vector<u32> pixels;
		// companion .object metadata (object kind): flags + surfaces are
		// editable, every other body chunk is round-tripped untouched
		bool           has_object = false;
		u32            object_flags = 0;
		xr_vector<ObjSurface> surfaces;
		xr_vector<ObjRawChunk> object_chunks;
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
	// companion .object metadata: flags + surfaces editable, all other body
	// chunks (meshes, bones, motions...) round-tripped untouched
	static bool ParseObjectFile(LPCSTR path, ThmEntry& out);
	static bool WriteObjectFile(LPCSTR path, const ThmEntry& e);
	static bool ObjReadStringZ(IReader& S, xr_string& out);
	static bool ObjParseSurfaces3(IReader& S, xr_vector<ObjSurface>& out);
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
	void DrawObjectPreview();
	void UpdatePreviewTexture(const ThmEntry& e);

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
	int m_FaceCount = 0;     // object kind mirror (read-only display)
	int m_VertexCount = 0;
	int m_SurfaceIndex = -1; // object kind: selected surface in the list
	char m_DetailName[256] = "";
	char m_BumpName[256] = "";
	char m_ExtNormalMap[256] = "";
	char m_BorderColor[16] = "";
	char m_FadeColor[16] = "";
	// object kind surface edit buffers
	char m_SurfShader[256] = "";
	char m_SurfCompiler[256] = "";
	char m_SurfMtl[256] = "";
	char m_SurfTexture[256] = "";
	// object kind preview texture (built from m_PreviewPath pixels)
	ID3DBaseTexture* m_PreviewTexture = nullptr;
	xr_string        m_PreviewPath;
};
