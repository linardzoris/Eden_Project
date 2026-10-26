#pragma once
#include "../xrECore/Editor/EThumbnail.h"

// Standalone .thm editor: opens any texture thumbnail file (not restricted to
// the SDK _game_textures_ tree), shows and edits the full STextureParams set
// and writes it back preserving the original THM chunk layout.
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
	bool Load(LPCSTR path);
	bool Save(LPCSTR path);
	void DoLoad();
	void DoSave(bool save_as);
	void ResetEdits();
	void DrawBasic();
	void DrawFlags();
	void DrawColors();
	void DrawDetail();
	void DrawMaterialBump();

	STextureParams m_Params;
	u32        m_ThmType = 0;        // THM_CHUNK_TYPE payload (ETTexture expected)
	xr_string  m_FilePath;           // absolute path of the loaded file
	bool       m_Dirty = false;

	// input buffers (ImGui needs stable writable storage)
	char m_DetailName[256] = "";
	char m_BumpName[256] = "";
	char m_ExtNormalMap[256] = "";
	char m_BorderColor[16] = "";
	char m_FadeColor[16] = "";
};
