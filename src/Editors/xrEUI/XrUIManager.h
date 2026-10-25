#pragma once
#include <d3d9.h>

enum TShiftState_
{
	ssNone = 0,
	ssShift = 1,
	ssLeft = 2,
	ssRight = 4,
	ssCtrl = 8,
	ssAlt = 16,
};
using TShiftState = int;
inline int UIToolBarSize = 24;

// Non-blocking editor notification (replaces native modal boxes for
// information/warning/error messages). Type matches TMsgDlgType values.
struct XREUI_API EditorToast
{
	int Type;
	xr_string Text;
	float Life;
	float TotalLife;
};

class XREUI_API XrUIManager
{
public:
	XrUIManager();
	void PushBegin(IEditorWnd*ui,bool need_deleted =true);
	void Push(IEditorWnd*ui,bool need_deleted =true);
	void Remove(IEditorWnd*ui);
	void Draw();
		
	virtual ~XrUIManager();

	void Initialize(HWND hWnd, IDirect3DDevice9* device,const char*ini_path);
	void Destroy();

	bool ProcessEvent(void* Event);

	void BeginFrame();
	void EndFrame();
	void MDIUpdate();

	void ResetBegin();
	void ResetEnd(void* NewDevice);
	virtual bool 	ApplyShortCut(DWORD Key, TShiftState Shift)=0;

	inline float GetMenuBarHeight()const { return m_MenuBarHeight; }
	inline TShiftState GetShiftState()const { return m_ShiftState; };
	virtual bool IsPlayInEditor() { return false; }
	void ApplyShortCutInput(DWORD Key);

	void AddToast(int Type, const char* Text);
	void DrawToasts();

	// Queued ImGui modal message boxes (one shown per frame until confirmed)
	void AddPopup(int Type, const char* Text);
	void DrawPopups();

protected:
	virtual void OnDrawUI();

private:
	float m_MenuBarHeight;
	TShiftState m_ShiftState;
	xr_vector<IEditorWnd*> m_UIArray;
	string_path m_name_ini;
	float m_ScaleDpi;
	xr_vector<EditorToast> m_Toasts;
	xr_vector<EditorToast> m_Popups;

public: 
	template<typename T> 
	IC bool HasWindow() const
	{
		return std::any_of
		(
			m_UIArray.begin(), m_UIArray.end(),
			[](IEditorWnd* Form)
			{
				return smart_cast<T*>(Form);
			}
		);
	}

	bool IsEnableInput = true;
	EDragDropType DnDType = EDragDropType::None;
	virtual void* LoadTexture(const char*) const { return nullptr; };
	void* SearchIcon = nullptr;
};

extern XREUI_API XrUIManager* GUIManager;