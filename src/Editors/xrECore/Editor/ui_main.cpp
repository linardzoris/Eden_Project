#include "stdafx.h"
#include "../xrEngine/xr_input.h"
#include "UI_ToolsCustom.h"

#include "ui_main.h"
#include "D3DUtils.h"
#include "SoundManager.h"
#include "../Layers/xrRender/PSLibrary.h"
#include "../Layers/xrRender/dxRenderDeviceRender.h"

#include "UIIConPicker.h"
#include "UIEditLightAnim.h"
#include "UIImageEditorForm.h"
#include "UISoundEditorForm.h"
#include "UIMinimapEditorForm.h"
#include "UIWeatherPropForm.h"
#include "../utils/ETools/ETools.h"
#include "UILogForm.h"
#include "../xrEngine/gamefont.h"
#include "../XrEngine/XR_IOConsole.h"

#define TRelease(x) if (x) x->pSurface->Release()

ECORE_API extern bool bIsLevelEditor;
namespace ImGui
{
	XREUI_API ImFont* LightFont;
	XREUI_API ImFont* RegularFont;
	XREUI_API ImFont* MediumFont;
	XREUI_API ImFont* BoldFont;
}

TUI* UI = nullptr;

// Overdraw debug layer (rs_overdraw): dx9 editor implementation, shared by all
// editors through xrECore. While g_overdraw_active is TRUE the CBackend hooks
// (dx9R_Backend_Runtime.h) substitute every pass pixel shader with the flat
// counting shader and force additive blending, so the count pass accumulates
// 13/255 per shaded fragment into RTOverdraw (20 layers reach the red band).
ECORE_API BOOL g_overdraw_active = FALSE;
ECORE_API ID3DPixelShader* g_overdraw_flat_ps = nullptr;
ECORE_API extern int rs_overdraw;
extern void xrRender_initconsole();

#include <d3dx9.h>

namespace
{
	// Flat counting pixel shader: no inputs, no clipping - every shaded
	// fragment adds one fixed step (13/255) to the accumulation target.
	LPCSTR overdraw_count_ps_src =
		"float4 main() : COLOR\n"
		"{\n"
		"	return float4(13.0f / 255.0f, 13.0f / 255.0f, 13.0f / 255.0f, 13.0f / 255.0f);\n"
		"}\n";

	// Banding display shader: maps the accumulated R channel (13/255 per
	// counted layer, 20 layers = 100%) onto the overdraw color bands. Pixels
	// with no counted geometry get alpha 0 and keep the background scene.
	LPCSTR overdraw_band_ps_src =
		"sampler2D s_count;\n"
		"float4 main(float2 tc : TEXCOORD0) : COLOR\n"
		"{\n"
		"	float r = tex2D(s_count, tc).r;\n"
		"	float f = r * (255.0f / 13.0f) / 20.0f;\n"
		"	float3 c = float3(0.0f, 1.0f, 0.0f);\n"                     // <= 20%
		"	c = lerp(c, float3(0.0f, 0.0f, 1.0f), step(0.2f, f));\n"    // 20-40%
		"	c = lerp(c, float3(1.0f, 1.0f, 0.0f), step(0.4f, f));\n"    // 40-60%
		"	c = lerp(c, float3(1.0f, 0.5f, 0.0f), step(0.6f, f));\n"    // 60-80%
		"	c = lerp(c, float3(1.0f, 0.0f, 0.0f), step(0.8f, f));\n"    // 80-100%
		"	return float4(c, step(0.02f, r));\n"
		"}\n";

	ID3DPixelShader* Overdraw_CompilePS(LPCSTR source)
	{
		ID3DXBuffer* code = nullptr;
		ID3DXBuffer* err = nullptr;
		HRESULT hr = D3DXCompileShader(source, xr_strlen(source), nullptr, nullptr, "main", "ps_2_0", 0, &code, &err, nullptr);
		if (err)
		{
			Msg("! overdraw pixel shader compile failed: %s", (LPCSTR)err->GetBufferPointer());
			err->Release();
		}
		if (FAILED(hr) || !code)
			return nullptr;

		ID3DPixelShader* ps = nullptr;
		R_CHK(REDevice->CreatePixelShader((DWORD*)code->GetBufferPointer(), &ps));
		code->Release();
		return ps;
	}
}

TUI::TUI()
{
	m_HConsole = 0;
	UI				= this;
	m_AppClosed = false;
	m_bAppActive 	= false;
	m_bReady 		= false;
	bNeedAbort   	= false;

	m_CurrentRStart.set(0,0,0);
	m_CurrentRDir.set(0,0,0);

	m_Flags.assign	(flResize);

	m_Pivot.set		( 0, 0, 0 );

	m_MouseCaptured = false;
	m_MouseMultiClickCaptured = false;
	m_SelectionRect = false;
	bMouseInUse		= false;

	m_bHintShowing	= false;
	m_LastHint		= "";

	int DisplayX = GetSystemMetrics(SM_CXFULLSCREEN);
	int DisplayY = GetSystemMetrics(SM_CYFULLSCREEN);

	Viewport& MainView = Views.emplace_back();
	ViewID = 0;

	m_Size.set(DisplayX, DisplayY);

	GUIManager = this;

	// Overdraw debug layer (rs_overdraw)
	OverdrawQuadVB		= nullptr;
	OverdrawCountPS		= nullptr;
	OverdrawBandPS		= nullptr;
}
//---------------------------------------------------------------------------
TUI::~TUI()
{
	VERIFY(m_ProgressItems.size()==0);
	VERIFY(m_EditorState.size()==0);

	TRelease(m_HeaderLogo);
	TRelease(m_WinMin);
	TRelease(m_WinRes);
	TRelease(m_WinMax);
	TRelease(m_WinClose);
}

ImTextureID TUI::LoadTexture(const char* Texture) const
{
	if (EDevice == nullptr || EDevice->Resources == nullptr)
	{
		return nullptr;
	}

	if (TextureStack.contains(Texture))
	{
		return TextureStack[Texture]->pSurface;
	}

	TextureStack[Texture] = EDevice->Resources->_CreateTexture(Texture);
	ref_texture& Tex = TextureStack[Texture];

	if (Tex->pSurface == nullptr)
	{
		Tex->apply_load(0);
	}

	return (void*)Tex->pSurface;
}

void TUI::OnDeviceCreate()
{
	DU_impl.OnDeviceCreate();

	// Overdraw debug layer (rs_overdraw)
	OverdrawCountPS = Overdraw_CompilePS(overdraw_count_ps_src);
	OverdrawBandPS = Overdraw_CompilePS(overdraw_band_ps_src);
	g_overdraw_flat_ps = OverdrawCountPS;
}

void TUI::OnDeviceDestroy()
{
	DU_impl.OnDeviceDestroy();
	TextureStack.clear();

	// Overdraw debug layer (rs_overdraw)
	g_overdraw_flat_ps = nullptr;
	if (OverdrawBandPS)		{ OverdrawBandPS->Release();	OverdrawBandPS = nullptr; }
	if (OverdrawCountPS)	{ OverdrawCountPS->Release();	OverdrawCountPS = nullptr; }
	OverdrawTexList = nullptr;
	OverdrawQuad.destroy();
	if (OverdrawQuadVB)		{ OverdrawQuadVB->Release();	OverdrawQuadVB = nullptr; }
	RTOverdraw.destroy();
}

bool TUI::IsModified()
{
	return ExecCommand(COMMAND_CHECK_MODIFIED);
}
//---------------------------------------------------------------------------

void TUI::EnableSelectionRect( bool flag ){
	m_SelectionRect = flag;
	m_SelEnd.x = m_SelStart.x = 0;
	m_SelEnd.y = m_SelStart.y = 0;
}

void TUI::UpdateSelectionRect( const Ivector2& from, const Ivector2& to ){
	m_SelStart.set(from);
	m_SelEnd.set(to);
}

bool  TUI::KeyDown (WORD Key, TShiftState Shift)
{
	if (!m_bReady) return false;
	if (Console->bVisible)
	{
		if (Key == 0xC0)
		{
			Console->Hide();
		}
		return true;
	}
   
	if (Key == 0xC0)
	{
		Console->Show();
		return true;
	}
//	m_ShiftState = Shift;
//	Log("Dn  ",Shift.Contains(ssShift)?"1":"0");
	if (UI->CurrentView().m_Camera.KeyDown(Key,Shift)) return true;
	return Tools->KeyDown(Key, Shift);
}

bool  TUI::KeyUp   (WORD Key, TShiftState Shift)
{
	if (!m_bReady) return false;
//	m_ShiftState = Shift;
	if (UI->CurrentView().m_Camera.KeyUp(Key,Shift)) return true;
	return Tools->KeyUp(Key, Shift);
}

bool  TUI::KeyPress(WORD Key, TShiftState Shift)
{
	if (!m_bReady) return false;
	return Tools->KeyPress(Key, Shift);
}
//----------------------------------------------------

void TUI::MousePress(TShiftState Shift, int X, int Y)
{
	if (!m_bReady) return;
	if (m_MouseCaptured) return;

	bMouseInUse = true;

	m_ShiftState = Shift;

	// camera activate
	if(!UI->CurrentView().m_Camera.MoveStart(m_ShiftState))
	{
		if (Tools->Pick(Shift)) 
			return;

		if( !m_MouseCaptured )
		{
			if (Tools->HiddenMode())
			{
				IR_GetMousePosScreen(m_StartCpH);
				m_DeltaCpH.set(0, 0);
			}
			else
			{
				m_CurrentCp = GetRenderMousePosition();
				m_StartCp = m_CurrentCp;
				UI->CurrentView().m_Camera.MouseRayFromPoint(m_CurrentRStart, m_CurrentRDir, m_CurrentCp );
				m_StartRStart = m_CurrentRStart;
				m_StartRDir = m_CurrentRDir;
			}
		   
			if(Tools->MouseStart(m_ShiftState))
			{
				if(Tools->HiddenMode()) ShowCursor( FALSE );
				m_MouseCaptured = true;
			}
		}
	}
	RedrawScene();
}

void TUI::MouseRelease(TShiftState Shift, int X, int Y)
{
	if (!m_bReady) return;

	m_ShiftState = Shift;

	if( UI->CurrentView().m_Camera.IsMoving() ){
		if (UI->CurrentView().m_Camera.MoveEnd(m_ShiftState)) bMouseInUse = false;
	}else{
		bMouseInUse = false;
		if( m_MouseCaptured ){
			if( !Tools->HiddenMode() ){
				m_CurrentCp = GetRenderMousePosition();
				UI->CurrentView().m_Camera.MouseRayFromPoint(m_CurrentRStart,m_CurrentRDir,m_CurrentCp );
			}
			bool bIsHiddenMode = Tools->HiddenMode();
			if( Tools->MouseEnd(m_ShiftState) ){
				if(bIsHiddenMode){
					SetCursorPos(m_StartCpH.x,m_StartCpH.y);
					ShowCursor( TRUE );
				}
				m_MouseCaptured = false;
			}
		}
	}
	// update tools (change action)
	Tools->OnFrame	();
	RedrawScene		();
}
//----------------------------------------------------
void TUI::MouseMove(TShiftState Shift, int X, int Y)
{
	if (!m_bReady) return;
	m_ShiftState = Shift;
}
//----------------------------------------------------
void TUI::IR_OnMouseMove(int x, int y)
{
	if (!m_bReady) 
		return;

	bool bRayUpdated = false;

	if (!UI->CurrentView().m_Camera.Process(m_ShiftState,x,y))
	{
		if( m_MouseCaptured || m_MouseMultiClickCaptured )
		{
			if( Tools->HiddenMode() )
			{
				m_DeltaCpH.set(x,y);
				if( m_DeltaCpH.x || m_DeltaCpH.y )
				{
					Tools->MouseMove(m_ShiftState);
				}
			}
			else
			{
				m_CurrentCp = GetRenderMousePosition();
				UI->CurrentView().m_Camera.MouseRayFromPoint(m_CurrentRStart,m_CurrentRDir,m_CurrentCp);
				Tools->MouseMove(m_ShiftState);
			}

			RedrawScene();
			bRayUpdated = true;
		}
	}

	if (!bRayUpdated)
	{
		m_CurrentCp = GetRenderMousePosition();
		UI->CurrentView().m_Camera.MouseRayFromPoint(m_CurrentRStart, m_CurrentRDir, m_CurrentCp);
	}
}
//---------------------------------------------------------------------------

void TUI::OnAppActivate()
{
	m_bAppActive = true;
	if (!m_bReady)return;
	if (pInput){
		m_ShiftState = ssNone;
		pInput->OnAppActivate();
		EDevice->seqAppActivate.Process	(rp_AppActivate);
	}
}
//---------------------------------------------------------------------------

void TUI::OnAppDeactivate()
{
	m_bAppActive = false;
	if (!m_bReady)return;
	if (pInput){
		pInput->OnAppDeactivate();
		m_ShiftState = ssNone;
		EDevice->seqAppDeactivate.Process(rp_AppDeactivate);
	}
	HideHint();
}
//---------------------------------------------------------------------------

bool TUI::ShowHint(const AStringVec& SS)
{
	VERIFY(m_bReady);

	if (!SS.empty() && ImGui::BeginTooltip())
	{
		for (const xr_string& Hint : SS)
		{
			ImGui::Text(Hint.c_str());
		}
		ImGui::EndTooltip();
	}

	//not_implemented();
	return m_bHintShowing;
}
//---------------------------------------------------------------------------

void TUI::HideHint()
{
	VERIFY(m_bReady);
	m_bHintShowing = false;
}
//---------------------------------------------------------------------------

void TUI::ShowHint()
{
	VERIFY(m_bReady);
	GetCursorPos(&m_HintPoint);
	AStringVec SS;
	Tools->OnShowHint(SS);

	if (!ShowHint(SS)) 
		HideHint();
}

//---------------------------------------------------------------------------

#include "..\xrEngine\IGame_Persistent.h"
void TUI::PrepareRedraw()
{
	VERIFY(m_bReady);
	if (m_Flags.is(flResize)) 			RealResize();
// set render state
	EDevice->SetRS(D3DRS_TEXTUREFACTOR,	0xffffffff);
	// fog
	u32 fog_color;
	float fog_start, fog_end;
	Tools->GetCurrentFog	(fog_color, fog_start, fog_end);

	EDevice->SetRS( D3DRS_FOGCOLOR,		fog_color			);
	EDevice->SetRS( D3DRS_RANGEFOGENABLE,	FALSE				);
	if (Caps.bTableFog)	{
		EDevice->SetRS( D3DRS_FOGTABLEMODE,	D3DFOG_LINEAR 	);
		EDevice->SetRS( D3DRS_FOGVERTEXMODE,	D3DFOG_NONE	 	);
	} else {
		EDevice->SetRS( D3DRS_FOGTABLEMODE,	D3DFOG_NONE	 	);
		EDevice->SetRS( D3DRS_FOGVERTEXMODE,	D3DFOG_LINEAR	);
	}
	EDevice->SetRS( D3DRS_FOGSTART,	*(DWORD *)(&fog_start)	);
	EDevice->SetRS( D3DRS_FOGEND,		*(DWORD *)(&fog_end)	);
	// filter
	for (u32 k=0; k<Caps.raster.dwStages; k++){
		if( psDeviceFlags.is(rsFilterLinear)){
			EDevice->SetSS(k,D3DSAMP_MAGFILTER,D3DTEXF_LINEAR);
			EDevice->SetSS(k,D3DSAMP_MINFILTER,D3DTEXF_LINEAR);
			EDevice->SetSS(k,D3DSAMP_MIPFILTER,D3DTEXF_LINEAR);
		} else {
			EDevice->SetSS(k,D3DSAMP_MAGFILTER,D3DTEXF_POINT);
			EDevice->SetSS(k,D3DSAMP_MINFILTER,D3DTEXF_POINT);
			EDevice->SetSS(k,D3DSAMP_MIPFILTER,D3DTEXF_POINT);
		}
	}
	// ligthing
	EDevice->SetRS(D3DRS_AMBIENT,0xFFFFFFFF);

	EDevice->SetRS			(D3DRS_FILLMODE, EDevice->dwFillMode);
	EDevice->SetRS			(D3DRS_SHADEMODE,EDevice->dwShadeMode);

	RCache.set_xform_world	(Fidentity);
}

void TUI::Invalidate()
{
	UI->RT.destroy();
	UI->RT.create("$user$rt_color", UI->GetRenderWidth(), UI->GetRenderHeight(), D3DFMT_X8R8G8B8);
}

//---------------------------------------------------------------------------
// Overdraw debug layer (rs_overdraw)
//---------------------------------------------------------------------------

BOOL TUI::OverdrawEnsureResources()
{
	if (!OverdrawCountPS || !OverdrawBandPS)
		return FALSE;

	const u32 W = GetRenderWidth();
	const u32 H = GetRenderHeight();
	if (!RTOverdraw || !RTOverdraw->valid() || RTOverdraw->dwWidth != W || RTOverdraw->dwHeight != H)
	{
		RTOverdraw.destroy();
		RTOverdraw.create("$user$overdraw", W, H, D3DFMT_A8R8G8B8);
		if (!RTOverdraw || !RTOverdraw->valid())
			return FALSE;
		// Single-entry texture list for the banding pass: stage 0 binding
		// goes through RCache so the backend texture cache stays consistent.
		OverdrawTexList = ref_texture_list(new STextureList());
		OverdrawTexList->push_back(std::make_pair(u32(0), RTOverdraw->pTexture));
	}

	if (!OverdrawQuad)
	{
		// Fullscreen quad in NDC space, drawn with identity transforms.
		struct OverdrawQuadVert { float x, y, z; float u, v; };
		static const OverdrawQuadVert Verts[6] =
		{
			{-1.f, -1.f, 0.f, 0.f, 1.f}, {1.f, -1.f, 0.f, 1.f, 1.f}, {-1.f, 1.f, 0.f, 0.f, 0.f},
			{1.f, -1.f, 0.f, 1.f, 1.f}, {1.f, 1.f, 0.f, 1.f, 0.f}, {-1.f, 1.f, 0.f, 0.f, 0.f},
		};
		REDevice->CreateVertexBuffer(sizeof(Verts), 0, D3DFVF_XYZ | D3DFVF_TEX1, D3DPOOL_MANAGED, &OverdrawQuadVB, nullptr);
		if (!OverdrawQuadVB)
			return FALSE;
		void* ptr = nullptr;
		R_CHK(OverdrawQuadVB->Lock(0, 0, &ptr, 0));
		CopyMemory(ptr, Verts, sizeof(Verts));
		OverdrawQuadVB->Unlock();
		OverdrawQuad.create(D3DFVF_XYZ | D3DFVF_TEX1, OverdrawQuadVB, nullptr);
	}
	return TRUE;
}

BOOL TUI::OverdrawCountPass()
{
	if (!OverdrawEnsureResources())
		return FALSE;

	// Count target: A8R8G8B8 additive accumulation over the SAME depth buffer
	// the main pass uses, cleared first so every frame restarts from zero.
	RCache.set_RT(RTOverdraw->pRT, 0);
	RCache.set_RT(0, 1);
	RCache.set_RT(0, 2);
	RCache.set_RT(0, 3);
	RCache.set_ZB(ZB->pRT);
	REDevice->Clear(0, 0, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0x0, 1, 0);

	g_overdraw_active = TRUE;
	Tools->Render();
	g_overdraw_active = FALSE;

	// Restore the main-pass target chain exactly as the normal frame setup.
	RCache.set_RT(RT->pRT, 0);
	RCache.set_RT(RTDiffuse->pRT, 1);
	RCache.set_RT(RTNormal->pRT, 2);
	RCache.set_RT(RTPostion->pRT, 3);
	// Neutral blend defaults (device-direct states, no backend cache behind them).
	EDevice->SetRS(D3DRS_ALPHABLENDENABLE, FALSE);
	EDevice->SetRS(D3DRS_BLENDOP, D3DBLENDOP_ADD);
	EDevice->SetRS(D3DRS_SRCBLEND, D3DBLEND_ONE);
	EDevice->SetRS(D3DRS_DESTBLEND, D3DBLEND_ZERO);
	EDevice->SetRS(D3DRS_ALPHATESTENABLE, FALSE);
	// Same stencil marker state the normal frame setup installs.
	RCache.set_Stencil(TRUE, D3DCMP_ALWAYS, 0x01, 0xff, 0xff, D3DSTENCILOP_KEEP, D3DSTENCILOP_REPLACE, D3DSTENCILOP_KEEP);
	// Fresh depth for the normal scene pass.
	REDevice->Clear(0, 0, D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0x0, 1, 0);
	return TRUE;
}

void TUI::OverdrawDrawBands()
{
	// Screen-space overlay: identity transforms, NDC quad, no depth usage.
	RCache.set_xform_world(Fidentity);
	RCache.set_xform_view(Fidentity);
	RCache.set_xform_project(Fidentity);
	RCache.set_Z(FALSE);
	RCache.set_CullMode(CULL_NONE);
	// Stencil stays enabled with the frame marker state so it can never
	// reject the overlay (device stencil state is unknown after pass states).
	RCache.set_Stencil(TRUE, D3DCMP_ALWAYS, 0x01, 0xff, 0xff, D3DSTENCILOP_KEEP, D3DSTENCILOP_REPLACE, D3DSTENCILOP_KEEP);
	REDevice->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
	// Normal alpha blending; alpha 0 keeps pixels without counted geometry
	// untouched (the background scene stays visible).
	EDevice->SetRS(D3DRS_ALPHABLENDENABLE, TRUE);
	EDevice->SetRS(D3DRS_BLENDOP, D3DBLENDOP_ADD);
	EDevice->SetRS(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
	EDevice->SetRS(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
	EDevice->SetRS(D3DRS_ALPHATESTENABLE, FALSE);
	// Bind the count texture through RCache so the backend cache stays consistent.
	RCache.set_Textures(OverdrawTexList);
	EDevice->SetSS(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
	EDevice->SetSS(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
	EDevice->SetSS(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
	EDevice->SetSS(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
	EDevice->SetSS(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
	RCache.set_VS((ID3DVertexShader*)nullptr, "overdraw_band_vs");
	RCache.set_PS(OverdrawBandPS, "overdraw_band_ps");
	RCache.set_Geometry(OverdrawQuad);
	RCache.Render(D3DPT_TRIANGLELIST, 0, 2);

	// Restore the states the rest of the frame relies on.
	RCache.set_Z(TRUE);
	RCache.set_CullMode(CULL_CCW);
	REDevice->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
	EDevice->SetRS(D3DRS_ALPHABLENDENABLE, FALSE);
	EDevice->SetRS(D3DRS_BLENDOP, D3DBLENDOP_ADD);
	EDevice->SetRS(D3DRS_SRCBLEND, D3DBLEND_ONE);
	EDevice->SetRS(D3DRS_DESTBLEND, D3DBLEND_ZERO);
	const u32 Filter = psDeviceFlags.is(rsFilterLinear) ? D3DTEXF_LINEAR : D3DTEXF_POINT;
	EDevice->SetSS(0, D3DSAMP_MAGFILTER, Filter);
	EDevice->SetSS(0, D3DSAMP_MINFILTER, Filter);
	EDevice->SetSS(0, D3DSAMP_MIPFILTER, Filter);
	EDevice->SetSS(0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
	EDevice->SetSS(0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
	// Unbind the count texture at device level; the backend list cache set
	// above makes any other texture list rebind its entries on demand.
	REDevice->SetTexture(0, nullptr);
}

extern ENGINE_API xr_atomic_bool g_bRendering;
void TUI::Redraw()
{
	PrepareRedraw();

	Viewport& View = CurrentView();

	if
		(
			u32(View.RTSize.x * EDevice->m_ScreenQuality) != EDevice->TargetWidth ||
			u32(View.RTSize.y * EDevice->m_ScreenQuality) != EDevice->TargetHeight ||
			!RT->pSurface
			)

		if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
		{
			EDevice->TargetWidth = View.RTSize.x * EDevice->m_ScreenQuality;
			EDevice->TargetHeight = View.RTSize.y * EDevice->m_ScreenQuality;

			RT.destroy();
			RTCopy.destroy();
			ZB.destroy();
			View.RTFreez.destroy();

			RTPostion.destroy();
			RTNormal.destroy();
			RTDiffuse.destroy();

			RTPostion.create("$user$position", GetRenderWidth(), GetRenderHeight(), D3DFMT_A16B16G16R16F);
			RTNormal.create("$user$normal", GetRenderWidth(), GetRenderHeight(), D3DFMT_A16B16G16R16F);
			RTDiffuse.create("$user$diffuse", GetRenderWidth(), GetRenderHeight(), D3DFMT_A8R8G8B8);

			RT.create("$user$rt_color", GetRenderWidth(), GetRenderHeight(), D3DFMT_X8R8G8B8);
			View.RTFreez.create(("$user$rt_freez" + xr_string::ToString((u32)UI->ViewID)).c_str(), GetRenderWidth() * EDevice->m_ScreenQuality, GetRenderHeight() * EDevice->m_ScreenQuality, D3DFMT_X8R8G8B8);
			RTCopy.create("$user$rt_color_copy", GetRenderWidth(), GetRenderHeight(), D3DFMT_X8R8G8B8);

			ZB.create("$user$rt_depth", GetRenderWidth(), GetRenderHeight(), D3DFMT_D24S8);

			m_Flags.set(flRedraw, TRUE);

			EDevice->m_fNearer = EDevice->mProject._43;
			EDevice->HalfTargetWidth = float(View.RTSize.x) * 0.5f;
			EDevice->HalfTargetHeight = float(View.RTSize.y) * 0.5f;
			EDevice->fASPECT = EDevice->HalfTargetHeight / EDevice->HalfTargetWidth;

			EDevice->seqDeviceReset.Process(rp_DeviceReset);
			EDevice->seqResolutionChanged.Process(rp_ScreenResolutionChanged);
			RCache.set_xform_project(EDevice->mProject);
			RCache.set_xform_world(Fidentity);
		}
		else
		{
			// Soft render update when resizing window
			EDevice->HalfTargetWidth = float(View.RTSize.x) * 0.5f;
			EDevice->HalfTargetHeight = float(View.RTSize.y) * 0.5f;
			EDevice->fASPECT = EDevice->HalfTargetHeight / EDevice->HalfTargetWidth;
			m_Flags.set(flRedraw, TRUE);
		}

	if (!UI->IsPlayInEditor())
	{
		EDevice->mProject.build_projection(deg2rad(EDevice->fFOV), EDevice->fASPECT, UI->CurrentView().m_Camera.m_Znear, UI->CurrentView().m_Camera.m_Zfar);
	}

	if (EDevice->Begin())
	{
		if (psDeviceFlags.is(rsRenderRealTime))
			m_Flags.set(flRedraw, TRUE);
		if (m_Flags.is(flRedraw) || UI->IsPlayInEditor())
		{
			m_Flags.set(flRedraw, FALSE);

			RCache.set_RT(RTNormal->pRT, 0);
			RCache.set_RT(RTDiffuse->pRT, 1);
			RCache.set_RT(RTPostion->pRT, 2);

			RCache.set_ZB(0);

			CHK_DX(REDevice->Clear(0, 0, D3DCLEAR_TARGET, 0x0, 1, 0));

			RCache.set_RT(RT->pRT);
			RCache.set_ZB(ZB->pRT);

			EDevice->Clear();

			RCache.set_RT(RTDiffuse->pRT, 1);
			RCache.set_RT(RTNormal->pRT, 2);
			RCache.set_RT(RTPostion->pRT, 3);

			RCache.set_Stencil(TRUE, D3DCMP_ALWAYS, 0x01, 0xff, 0xff, D3DSTENCILOP_KEEP, D3DSTENCILOP_REPLACE, D3DSTENCILOP_KEEP);
			EDevice->UpdateView();
			EDevice->ResetMaterial();

			Tools->RenderEnvironment();

			for (u32 k = 0; k < Caps.raster.dwStages; k++)
			{
				if (psDeviceFlags.is(rsFilterLinear)) {
					EDevice->SetSS(k, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
					EDevice->SetSS(k, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
					EDevice->SetSS(k, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
				}
				else {
					EDevice->SetSS(k, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
					EDevice->SetSS(k, D3DSAMP_MINFILTER, D3DTEXF_POINT);
					EDevice->SetSS(k, D3DSAMP_MIPFILTER, D3DTEXF_POINT);
				}
			}

			// draw grid
			if (psDeviceFlags.is(rsDrawGrid))
			{
				DU_impl.DrawGrid();
				DU_impl.DrawPivot(m_Pivot);
			}

			// Overdraw debug layer (rs_overdraw): count pass first, drawing
			// the scene a second time into the accumulation target.
			BOOL overdraw_counted = FALSE;
			if (rs_overdraw)
				overdraw_counted = OverdrawCountPass();

			Tools->Render();

			// Overdraw debug layer: band the accumulated counts over the scene.
			if (overdraw_counted)
				OverdrawDrawBands();

			// draw selection rect
			if (m_SelectionRect)
			{
				DU_impl.DrawSelectionRect(m_SelStart, m_SelEnd);
			}

			// draw axis
			if (psDeviceFlags.test(rsDrawAxis) && !psDeviceFlags.test(rsDisableAxisCube))
				DU_impl.DrawAxis(UI->CurrentView().m_Camera.GetTransform());


			EDevice->Statistic->RenderDUMP_RT.End();
			EDevice->Statistic->Show();
			EDevice->SetRS(D3DRS_FILLMODE, D3DFILL_SOLID);

			g_FontManager->Render();

			EDevice->SetRS(D3DRS_FILLMODE, EDevice->dwFillMode);
			EDevice->seqRender.Process(rp_Render);

			if (g_pGamePersistent->OnRenderPPUI_query())
			{
				g_pGamePersistent->OnRenderPPUI_main();
			}

			RCache.set_RT(0, 1);
			RCache.set_RT(0, 2);
			RCache.set_RT(0, 3);

			RCache.set_RT(RSwapchainTarget);
			RCache.set_ZB(RDepth);

			RDevice->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, 0);
			RDevice->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
			RDevice->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
			RDevice->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
		}

		EDevice->SetRS(D3DRS_FILLMODE, D3DFILL_SOLID);
		g_bRendering = FALSE;

		// end draw
		UI->BeginFrame();

		Draw();

		UI->EndFrame();
		EDevice->End();
		UI->MDIUpdate();
	}

	for (auto& Callback : CommandList[TUI::ECommandListID::CurrentFrame])
	{
		Callback();
	}

	CommandList[TUI::ECommandListID::CurrentFrame].clear();
	std::swap(CommandList[TUI::ECommandListID::CurrentFrame], CommandList[TUI::ECommandListID::NextFrame]);
}

void TUI::RealResize()
{
	m_Flags.set			(flResize,FALSE);
	if(m_Size.x&& m_Size.y)
	EDevice->Resize(m_Size.x, m_Size.y,m_Size_Maximize);
	ExecCommand			(COMMAND_UPDATE_PROPERTIES);
}
void TUI::RealUpdateScene()
{
	Tools->UpdateProperties	(false);
	m_Flags.set			(flUpdateScene,FALSE);
}
void TUI::RealRedrawScene()
{

	Redraw				();         
}
void TUI::OnFrame()
{
	EDevice->FrameMove	();
	SndLib->OnFrame		();
	// tools on frame
	if (m_Flags.is(flUpdateScene)) RealUpdateScene();
	Tools->OnFrame		();

	// show hint
	ResetBreak			();

	// Progress
	ProgressDraw		();
}

bool TUI::Idle()
{
	VERIFY(m_bReady);

	MSG msg;
	do
	{
		ZeroMemory(&msg, sizeof(msg));
		if (::PeekMessage(&msg, NULL, 0U, 0U, PM_REMOVE))
		{
			::TranslateMessage(&msg);
			::DispatchMessage(&msg);
			if (msg.message == WM_QUIT)
			{
				UI->Quit();
			}
			continue;
		}

	} while (msg.message);

	if (m_Flags.is(flResetUI))
		RealResetUI();

	Sleep(1);

	OnFrame();

	Device.secondary_tasks.run([]()
	{
		PROF_THREAD("Secondary async")
		{
			PROF_EVENT("Sheduler")
			Engine.Sheduler.Update();
		}

		{
			PROF_EVENT("seqParallel")
			for (u32 pit = 0; pit < EDevice->seqParallel.size(); pit++)
				EDevice->seqParallel[pit]();
			EDevice->seqParallel.clear();
		}

		{
			PROF_EVENT("seqFrameMT")
			EDevice->seqFrameMT.Process(rp_Frame);
		}
	});

	if (EDevice->b_is_Active && !m_Flags.is(flNeedQuit) && !m_AppClosed)
		RealRedrawScene();

	// test quit
	if (m_Flags.is(flNeedQuit))	
		RealQuit();

	Device.secondary_tasks.wait();

	return !m_AppClosed;
}

void ResetActionToSelect()
{
	ExecCommand(COMMAND_CHANGE_ACTION, etaSelect);
}

bool TUI::OnCreate()
{
// create base class
	EDevice->InitTimer();

	EDevice->Initialize();
	// Creation
	ETOOLS::ray_options	(CDB::OPT_ONLYNEAREST | CDB::OPT_CULL);

	pInput			= new CInput(FALSE, all_device_key);

	Console = new CConsole();
	Console->Initialize();

	// Register the xrRender console commands (rs_overdraw etc.). Engine
	// renderer DLLs do this from their DllMain (EntryPoint.cpp), xrECore
	// compiles the same sources without that entry, so register here.
	xrRender_initconsole();

	UI->IR_Capture	();

	m_bReady		= true;

	string_path log_path;
	if (!FS.exist(log_path,_temp_,""))
	{
		VerifyPath(log_path);
	}
	if (!FS.path_exist(_local_root_)){
		ELog.DlgMsg	(mtError,"Undefined Editor local directory.");
		return 		false;
	}

	BeginEState(esEditScene);

	GetRenderWidth() = 128;
	GetRenderHeight() = 128;

	int Iter = 0;
	for (Viewport& View : Views)
	{
		View.RTSize = { (int)GetRenderWidth(), (int)GetRenderHeight() };
		View.RTFreez.create(("$user$rt_freez" + xr_string::ToString((u32)UI->ViewID)).c_str(), GetRenderWidth() * EDevice->m_ScreenQuality, GetRenderHeight() * EDevice->m_ScreenQuality, D3DFMT_X8R8G8B8);
	}
	EDevice->fASPECT = (float)GetRenderWidth() / (float)GetRenderHeight();

	EDevice->mProject.build_projection(deg2rad(EDevice->fFOV), EDevice->fASPECT, UI->CurrentView().m_Camera.m_Znear, UI->CurrentView().m_Camera.m_Zfar);
	EDevice->m_fNearer = EDevice->mProject._43;

	RCache.set_xform_project(EDevice->mProject);
	RCache.set_xform_world(Fidentity);

	RTPostion.create("$user$position", GetRenderWidth() * EDevice->m_ScreenQuality, GetRenderHeight() * EDevice->m_ScreenQuality, D3DFMT_A16B16G16R16F);
	RTNormal.create("$user$normal", GetRenderWidth() * EDevice->m_ScreenQuality, GetRenderHeight() * EDevice->m_ScreenQuality, D3DFMT_A16B16G16R16F);
	RTDiffuse.create("$user$diffuse", GetRenderWidth() * EDevice->m_ScreenQuality, GetRenderHeight() * EDevice->m_ScreenQuality, D3DFMT_A8R8G8B8);

	RT.create("$user$rt_color", GetRenderWidth() * EDevice->m_ScreenQuality, GetRenderHeight() * EDevice->m_ScreenQuality, D3DFMT_X8R8G8B8);
	RTCopy.create("$user$rt_color_copy", GetRenderWidth() * EDevice->m_ScreenQuality, GetRenderHeight() * EDevice->m_ScreenQuality, D3DFMT_X8R8G8B8);

	ZB.create("$user$rt_depth", GetRenderWidth() * EDevice->m_ScreenQuality, GetRenderHeight() * EDevice->m_ScreenQuality, D3DFMT_D24S8);

	return true;
}

void TUI::OnDestroy()
{
	Console->Destroy();
	xr_delete(Console);

	for (Viewport& View : Views)
	{
		View.RTFreez.destroy();
	}

	TextureStack.clear();

	RT.destroy();
	RTCopy.destroy();
	ZB.destroy();

	RTPostion.destroy();
	RTNormal.destroy();
	RTDiffuse.destroy();

	VERIFY(m_bReady);
	m_bReady		= false;
	UI->IR_Release	();
	xr_delete		(pInput);
	EndEState		();

	EDevice->ShutDown();    
}

SPBItem* TUI::ProgressStart(float max_val, LPCSTR text)
{
	VERIFY(m_bReady);
	SPBItem* item = new SPBItem(text, "", max_val);
	m_ProgressItems.push_back(item);
	ELog.Msg(mtInformation, text);
	ProgressDraw();

	IsLoading = true;

	return item;
}

void TUI::ProgressEnd(SPBItem*& pbi)
{
	VERIFY(m_bReady);
	if (pbi) 
	{
		PBVecIt it = std::find(m_ProgressItems.begin(), m_ProgressItems.end(), pbi); VERIFY(it != m_ProgressItems.end());
		m_ProgressItems.erase(it);
		xr_delete(pbi);
		ProgressDraw();

		IsLoading = false;
	}
}

void TUI::ProgressDraw()
{
	SPBItem* pbi = UI->ProgressLast();
	if (pbi)
	{
		xr_string txt;
		float 		p, m;
		pbi->GetInfo(txt, p, m);
		// progress
		ProgressStatus = fis_zero(m) ? 0 : (int)((p / m) * 100);
	}
}

TUI::Viewport& TUI::CurrentView()
{
	return Views[ViewID];
}

void TUI::CreateViewport(int ID, UIRenderForm* Form)
{
	Viewport& MainView = Views.emplace_back();
	MainView.m_Camera.SetViewport(EPrefs->view_np, EPrefs->view_fp, EPrefs->view_fov, true);
	MainView.m_Camera.SetSensitivity(EPrefs->cam_sens_move, EPrefs->cam_sens_rot);
	MainView.m_Camera.SetFlyParams(EPrefs->cam_fly_speed, EPrefs->cam_fly_alt);
	MainView.m_Camera.Reset();

	MainView.ViewportForm = Form;
	MainView.ViewGlobalIDX = ID;

	MainView.RTSize = { (int)GetRenderWidth(), (int)GetRenderHeight() };
	MainView.RTFreez.create(("$user$rt_freez" + xr_string::ToString(ID)).c_str(), GetRenderWidth() * EDevice->m_ScreenQuality, GetRenderHeight() * EDevice->m_ScreenQuality, D3DFMT_X8R8G8B8);
}

void TUI::DestroyViewport(int ID)
{
	auto Iter = std::find(Views.begin(), Views.end(), Viewport{ .ViewGlobalIDX = ID });
	if (Iter != Views.end())
	{
		Views.erase(Iter);
		return;
	}

	VERIFY(!"Viewport not found!");
}

void TUI::InitWindowIcons()
{
	m_HeaderLogo	= EDevice->Resources->_CreateTexture("ed\\bar\\win_header_logo");
	m_WinMin		= EDevice->Resources->_CreateTexture("ed\\bar\\win_header_min");
	m_WinMax		= EDevice->Resources->_CreateTexture("ed\\bar\\win_header_max");
	m_WinRes		= EDevice->Resources->_CreateTexture("ed\\bar\\win_header_restore");
	m_WinClose		= EDevice->Resources->_CreateTexture("ed\\bar\\win_header_close");
}

void TUI::OnDrawUI()
{
	UIKeyPressForm::Update(EDevice->fTimeGlobal);
	UIEditLightAnim::Update();
	UIImageEditorForm::Update();
	UISoundEditorForm::Update();
	UIMinimapEditorForm::Update();
	UIWeatherPropForm::Update();
	UIIconPicker::Update();
	UILogForm::Update();
	EDevice->seqDrawUI.Process(rp_DrawUI);
}

void TUI::RealResetUI()
{
	m_Flags.set(flResetUI, FALSE);
	string_path 		ini_path;
	if (FS.exist(ini_path, "$server_data_root$", UI->EditorName(), "_imgui_default.ini"))
	{
		UI->Resize(1280, 800);
		ImGui::LoadIniSettingsFromDisk(ini_path);
	}
}

void SPBItem::GetInfo(xr_string& txt, float& p, float& m)
{
	string256 temp_buff = {};

	if (info.size())sprintf(temp_buff, "%s (%s)", text.c_str(), info.c_str());
	else			sprintf(temp_buff, "%s", text.c_str());

	txt = temp_buff;

	p = progress;
	m = max;
}

void SPBItem::Inc(LPCSTR info, bool bWarn)
{
	Info(info, bWarn);
	Update(progress + 1.f);
}

void SPBItem::Update(float val)
{
	progress = val;
	UI->ProgressDraw();
}

void SPBItem::Info(LPCSTR text, bool bWarn)
{
	if (text && text[0])
	{
		info = text;
		xr_string 				txt;
		float 					p, m;
		GetInfo(txt, p, m);
		ELog.Msg(bWarn ? mtError : mtInformation, txt.c_str());
		UI->ProgressDraw();
	}
}