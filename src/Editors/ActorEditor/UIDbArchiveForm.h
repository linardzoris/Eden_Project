#pragma once

// X-Ray game archive (.db/.xdb/.pack) tool: unpacks an archive into a folder
// and packs a folder back into an archive compatible with the engine
// LocatorAPI (LZHUF-compressed desc chunk + lzo1x-compressed file data).
class UIDbArchiveForm :
	public IEditorWnd
{
public:
	UIDbArchiveForm();
	virtual ~UIDbArchiveForm();
	virtual void Draw();

	static void Update();
	static void Show();

	static UIDbArchiveForm* Form;

private:
	void DoUnpack();
	void DoPack();
	void DrawUnpackTab();
	void DrawPackTab();

	char m_ArchivePath[1024] = "";
	char m_UnpackDir[1024] = "";
	char m_PackSrcDir[1024] = "";
	char m_PackOutPath[1024] = "";
	char m_EntryPoint[128] = "gamedata";
	bool m_Compress = true;
	bool m_UnpackTab = true;
	xr_string m_LastResult;
};
