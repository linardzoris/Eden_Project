#pragma once
#include "../xrEUI/EditorWnd.h"

// Image Converter: batch-imports Stalker2 source textures (.tga/.png), pairs
// them into _D + _N + (_RMA | _RHA) groups by base name, auto-configures a
// channel-remap conversion recipe for each one and converts approved entries
// to engine DDS assets (DXT1/BC7 etc., Box mipmaps, linear color space).
//
// Import only pre-configures: every entry carries an "Approve Configuration"
// checkbox the user must tick before "Make Texture" converts it. Unapproved
// entries are left untouched (marked [U]), converted ones are marked [A].
//
// List interaction mirrors UIThmEditorForm: plain click selects one entry,
// ctrl+click toggles, shift+click selects a range; edits apply to the whole
// selection and values differing across it are tinted ("mixed").
class UIImageConverterForm :
	public IEditorWnd
{
public:
	UIImageConverterForm();
	virtual ~UIImageConverterForm();
	virtual void Draw();

	static void Update();
	static void Show();

	static UIImageConverterForm* Form;

	enum ERole
	{
		RoleUnknown = 0,
		RoleD,       // albedo (+ optional transparency)
		RoleN,       // normal.xy + ao + roughness packed
		RoleRHA,     // height in G (low-res roughness/AO channels are unused)
		RoleRMA,     // metallic in G (low-res roughness/AO channels are unused)
	};

	enum EFormat
	{
		FmtBC1 = 0,   // fourcc DXT1
		FmtBC1a,      // fourcc DXT1 with 1-bit alpha
		FmtBC2,       // fourcc DXT3
		FmtBC3,       // fourcc DXT5
		FmtBC5,       // DX10 header, DXGI_FORMAT_BC5_UNORM
		FmtRGBA8,     // legacy fourcc A8R8G8B8
		FmtBC7,       // DX10 header, DXGI_FORMAT_BC7_UNORM
		FmtCount,
	};

	enum EState
	{
		StUnconverted = 0, // [U]
		StConverted,       // [A]
		StFailed,          // shown as [U], tooltip carries the error
	};

private:
	struct Provenance
	{
		bool      found = false;
		xr_string text;          // e.g. "Height <RHA.g" or "NOT FOUND"
	};

	struct ConvEntry
	{
		xr_string path;                    // absolute source path
		xr_string name;                    // file name with extension
		xr_string base;                    // name without role suffix and extension
		u32       role = RoleUnknown;
		bool      approved = false;        // Approve Configuration checkbox
		u32       state = StUnconverted;
		// conversion config (target size defaults to source dims at import)
		u32       fmt = FmtBC7;
		int       target_w = 0;
		int       target_h = 0;
		int       cutout = 0;              // 0 = off, 1..255 alpha threshold (_D only)
		int       src_w = 0, src_h = 0;
		// group companions (indices into m_Entries, -1 = absent)
		int       comp_d = -1;
		int       comp_n = -1;
		int       comp_rha = -1;
		int       comp_rma = -1;
		// output channel provenance for the UI
		Provenance prov[4];
		// last conversion error (failed entries)
		xr_string error;
	};

	// import: Win32 multi-select dialog -> role parse -> grouping -> auto config
	void DoImport();
	// runs the confirmed conversion pass over approved entries
	void DoConvert();
	// rebuilds companion links after import/removal and refreshes provenance
	void RebuildGroups();
	// fills e.prov[] from the recipe and current companions
	void UpdateProvenance(ConvEntry& e);
	// approve gating: known role + all companions required by the recipe present
	bool CanApprove(const ConvEntry& e) const;
	// output file name for the entry's role (base.dds / base_bump.dds / base_bump#.dds)
	xr_string OutputName(const ConvEntry& e) const;

	// single-entry conversion pipeline (DESIGN3 5.2); returns false on failure
	bool ConvertEntry(ConvEntry& e);

	// ---- UI drawing ----
	void DrawFileList();
	void DrawSettings();
	void DrawProvenance();
	void DrawConfirmModal();
	// validates and applies a target size to every selected entry;
	// reports violations through ELog (no upscale, multiple of 4)
	void ApplyTargetSize(int w, int h);

	// ---- selection helpers (UIThmEditorForm pattern) ----
	bool HasSelection() const { return !m_Selection.empty(); }
	int  PrimaryIndex() const { return m_Selection.empty() ? -1 : m_Selection.front(); }
	bool Selected(int i) const;
	void SelectOnly(int i);
	void ToggleSelect(int i);
	void RangeSelect(int i);

	template <class F> void ForSelected(F&& set)
	{
		for (int i : m_Selection)
			set(m_Entries[i]);
	}

	// true when the field value differs across the selection ("mixed")
	template <class F> bool FieldMixed(F&& get) const
	{
		if (m_Selection.size() < 2)
			return false;
		const ConvEntry& first = m_Entries[m_Selection.front()];
		for (size_t k = 1; k < m_Selection.size(); ++k)
			if (get(first) != get(m_Entries[m_Selection[k]]))
				return true;
		return false;
	}

	xr_vector<ConvEntry> m_Entries;
	xr_vector<int>       m_Selection;    // sorted indices into m_Entries
	int                  m_Anchor = -1;  // last plain/ctrl-clicked row, shift anchor

	xr_string            m_ExportDir;    // empty = write next to each source file
	xr_string            m_LastDir;      // import dialog: remembered last directory

	bool                 m_ConfirmOpen = false; // Make Texture confirmation modal
	bool                 m_SizeError = false;   // "multiple of 4" validation popup
	bool                 m_UseGPU = true;       // DirectCompute BC compression (D3D11)
	u32                  m_LastConverted = 0;   // stats of the last Make Texture run
	u32                  m_LastFailed = 0;

	// edit mirrors of the primary selection (ImGui needs stable storage);
	// refreshed from the primary entry every frame
	int m_EdFmt = 0;
	int m_EdW = 0;
	int m_EdH = 0;
	int m_EdCutout = 0;
	bool m_EdApproved = false;
};
