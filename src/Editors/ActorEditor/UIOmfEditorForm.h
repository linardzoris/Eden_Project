#pragma once

// Standalone .omf editor. Ported from the in-game OMF editor
// (src/xrGame/ImUtils/OMFEditor.cpp) with a working Save/Save As.
// Layout authority: editor export (xrECore/Editor/ExportSkeleton.cpp
// ExportMotionKeys/ExportMotionDefs) and engine load (xrEngine/SkeletonMotions.cpp).
// Animation key data and notify chunks are passed through as raw bytes.
class UIOmfEditorForm :
	public IEditorWnd
{
public:
	struct OmfAnim
	{
		xr_string       name;
		xr_vector<char> data;      // raw key payload after the name (incl. dwLen)
	};
	struct OmfNotify
	{
		bool            present = false;
		xr_vector<char> data;      // raw notify chunk payload (id = count+2+motion)
	};
	struct OmfBone
	{
		u32      id = 0;
		xr_string name;
	};
	struct OmfPart
	{
		xr_string          name;
		xr_vector<OmfBone> bones;
	};
	struct OmfMarkInterval
	{
		float t0 = 0.0f;
		float t1 = 0.0f;
	};
	struct OmfMark
	{
		xr_string                name;
		xr_vector<OmfMarkInterval> intervals;
	};
	struct OmfMotion
	{
		xr_string name;
		u32       flags = 0;
		u16       bone_or_part = 0;
		u16       motion_id = 0;
		float     speed = 0.0f;
		float     power = 0.0f;
		float     accrue = 0.0f;
		float     falloff = 0.0f;
		xr_vector<OmfMark> marks;
	};

public:
	UIOmfEditorForm();
	virtual ~UIOmfEditorForm();
	virtual void Draw();

	static void Update();
	static void Show();

	static UIOmfEditorForm* Form;

private:
	struct OmfData
	{
		u32                 animations_count = 0;
		xr_vector<OmfAnim>  anims;
		xr_vector<OmfNotify> notifies;
		s16                 ogf_version = 0;
		xr_vector<OmfPart>  parts;
		u16                 motions_count = 0;
		xr_vector<OmfMotion> motions;

		void Clear();
	};

	bool Load(LPCSTR path);
	bool Save(LPCSTR path);
	void DoLoad();
	void DoSave(bool save_as);
	void DrawLeftPanel();
	void DrawRightPanel();
	void DrawMarks();
	bool RenameMotion(int index, const char* new_name);
	bool RenameBone(int global_index, const char* new_name);
	void CollectBoneNames();
	bool NameExists(const char* name, int skip_motion = -1);

	OmfData   m_Omf;
	xr_string m_FilePath;
	xr_string m_Error;
	bool      m_Dirty = false;
	int       m_SelectedMotion = -1;
	int       m_SelectedBone = 0;
	xr_vector<xr_string> m_BoneNames;   // flattened part\bone for the combo

	// input buffers (stable storage for ImGui widgets)
	char m_AnimRename[128] = "";
	char m_BoneRename[128] = "";
	char m_MarkRename[128] = "";
	char m_NewMark[128] = "";
};
