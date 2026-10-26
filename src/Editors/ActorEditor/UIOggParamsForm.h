#pragma once

// Editor for the X-Ray sound parameters stored inside the .ogg vorbis comment
// (comment[0] binary blob, version 3). Saving re-encodes the audio stream via
// XRay::Importer::Audio::ImportWav with the new comment attached.
class UIOggParamsForm :
	public IEditorWnd
{
public:
	UIOggParamsForm();
	virtual ~UIOggParamsForm();
	virtual void Draw();

	static void Update();
	static void Show();

	static UIOggParamsForm* Form;

private:
	bool Load(LPCSTR path);
	bool Save(LPCSTR path);
	void DoLoad();
	void DoSave(bool save_as);

	xr_string       m_FilePath;          // absolute path of the loaded file
	xr_vector<char> m_FileData;          // raw ogg bytes, decoded on save
	bool            m_Dirty = false;
	bool            m_HadComment = false;
	u32             m_CommentVersion = 0;

	// X-Ray sound params (ogg comment v3 layout)
	float m_MinDist = 1.0f;
	float m_MaxDist = 100.0f;
	float m_BaseVolume = 1.0f;
	float m_MaxAIDist = 100.0f;
	u32   m_GameType = 0;
	float m_Quality = 0.5f;              // vorbis VBR quality for re-encode

	// stream info (display only)
	float m_Rate = 0.0f;
	int   m_Channels = 0;
	float m_Duration = 0.0f;
};
