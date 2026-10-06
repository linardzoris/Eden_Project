#include "StdAfx.h"
#include "HUDManager.h"
#include "HUDTarget.h"
#include "Actor.h"
#include "../xrEngine/IGame_Level.h"
#include "../xrEngine/xr_input.h"
#include "GamePersistent.h"
#include "MainMenu.h"
#include "Grenade.h"
#include "Spectator.h"
#include "Car.h"
#include "UIGameCustom.h"
#include "../../xrUI/UICursor.h"
#include "../xrEngine/string_table.h"
#include "game_cl_base.h"
#ifdef	DEBUG
#include "PHDebug.h"
#endif
#include "../../xrUI/UIFontDefines.h"
#include "player_hud.h"

extern CUIGameCustom* CurrentGameUI() {return HUD().GetGameUI();}

//--------------------------------------------------------------------
CHUDManager::CHUDManager() : pUIGame(nullptr), m_pHUDTarget(new CHUDTarget())
{
	m_firepos_frame = u32(-1);
	m_firepos_active = false;
	m_aimpos_frame = u32(-1);
	m_aimpos_active = false;
}
//--------------------------------------------------------------------
CHUDManager::~CHUDManager()
{
	OnDisconnected();

	if(pUIGame)
		pUIGame->UnLoad	();

	xr_delete		(pUIGame);
	xr_delete		(m_pHUDTarget);
}
#include "GametaskManager.h"
//--------------------------------------------------------------------
void CHUDManager::OnFrame()
{
	if(!b_online)
		return;

	PROF_EVENT("CHUDManager::OnFrame");

	PP.CameraPick();
	if (g_player_hud)
		g_player_hud->OnFrame();
	DoPick(PP);

	if (Device.IsEditorMode())
	{
		OnFrameMT();
	}
}

xrCriticalSection ui_lock;
void CHUDManager::OnFrameMT()
{
	if(!b_online)						
		return;
	PROF_EVENT("CHUDManager::OnFrameMT");

	if (Device.dwPrecacheFrame == 0)
		Level().GameTaskManager()->UpdateTasks();

	xrCriticalSectionGuard guard(&ui_lock);
	if (pUIGame) 
		pUIGame->OnFrame();
}
//--------------------------------------------------------------------

ENGINE_API extern float psHUD_FOV;

void CHUDManager::Render_First()
{
	if (!psHUD_Flags.is(HUD_WEAPON|HUD_WEAPON_RT|HUD_WEAPON_RT2|HUD_DRAW_RT2))
	{
		return;
	}

	if (pUIGame == nullptr)
	{
		return;
	}

	CObject* O = g_pGameLevel->CurrentViewEntity();
	if (O == nullptr)
	{
		return;
	}

	CActor* A = O->cast_actor();
	if (A == nullptr)
	{
		return;
	}

	if (!A->HUDview())
	{
		return;
	}

	// only shadow 
	::Render->set_Invisible			(TRUE);
	::Render->set_Object			(O->H_Root());
	O->renderable_Render			();
	::Render->set_Invisible			(FALSE);
}

bool need_render_hud()
{
	CObject* O = g_pGameLevel != nullptr ? g_pGameLevel->CurrentViewEntity() : nullptr;
	if (O == nullptr)
		return false;

	if (CActor* A = O->cast_actor())
	{
		if (!A->HUDview() || !A->g_Alive())
		{
			return false;
		}
	}

	if (O->cast_car() || O->cast_spectator())
	{
		return false;
	}

	return true;
}

void CHUDManager::Render_Last()
{
	if (!psHUD_Flags.is(HUD_WEAPON|HUD_WEAPON_RT|HUD_WEAPON_RT2|HUD_DRAW_RT2))return;
	if (0==pUIGame)					return;

	if(!need_render_hud())			return;

	CObject*	O					= g_pGameLevel->CurrentViewEntity();
	// hud itself
	::Render->set_HUD				(TRUE);
	::Render->set_Object			(O->H_Root());
	O->OnHUDDraw					(this);
	::Render->set_HUD				(FALSE);
}

#include "player_hud.h"
bool   CHUDManager::RenderActiveItemUIQuery()
{
	if (!psHUD_Flags.is(HUD_DRAW_RT2))	
		return false;

	if (!psHUD_Flags.is(HUD_WEAPON|HUD_WEAPON_RT|HUD_WEAPON_RT2))return false;

	if(!need_render_hud())			return false;

	return (g_player_hud && g_player_hud->render_item_ui_query() );
}

void   CHUDManager::RenderActiveItemUI()
{
	if (!psHUD_Flags.is(HUD_DRAW_RT2))	
		return;

	g_player_hud->render_item_ui		();
}

extern ENGINE_API BOOL bShowPauseString;
//отрисовка элементов интерфейса
void  CHUDManager::RenderUI()
{
	if (!psHUD_Flags.is(HUD_DRAW_RT2))	
		return;

	if(!b_online)					return;
	PROF_EVENT("CHUDManager::RenderUI");
	if (true /*|| psHUD_Flags.is(HUD_DRAW | HUD_DRAW_RT)*/)
	{
		HitMarker.Render			();
		if(pUIGame)
		{
			xrCriticalSectionGuard guard(&ui_lock);
			pUIGame->Render();
		}

		UI().RenderFont				();
	}

		m_pHUDTarget->Render();


	if( Device.Paused() && bShowPauseString){
		CGameFont* pFont	= UI().Font().GetFont(GRAFFITI50_FONT_NAME);
		pFont->SetColor		(0x80FF0000	);
		LPCSTR _str			= g_pStringTable->translate("st_game_paused").c_str();
		
		Fvector2			_pos;
		_pos.set			(UI_BASE_WIDTH/2.0f, UI_BASE_HEIGHT/2.0f);
		UI().ClientToScreenScaled(_pos);
		pFont->SetAligment	(CGameFont::alCenter);
		pFont->Out			(_pos.x, _pos.y, _str);
		pFont->OnRender		();
	}

}

void CHUDManager::OnEvent(EVENT E, u64 P1, u64 P2)
{
}

collide::rq_result&	CHUDManager::GetCurrentRayQuery	()
{
	return GetPick().result;
}

bool CHUDManager::FireposActive()
{
	if (m_firepos_frame != Device.dwFrame)
	{
		m_firepos_frame = Device.dwFrame;
		m_firepos_active = ComputeFireposActive();
	}

	return m_firepos_active;
}

bool CHUDManager::ComputeFireposActive()
{
	// If we have an actor...
	CActor* pActor = smart_cast<CActor*>(Level().CurrentEntity());
	if (!pActor)
		return psActorFlags.test(AF_FIREPOS);

	// And a weapon...
	CWeapon* pWeapon = smart_cast<CWeapon*>(pActor->inventory().ActiveItem());
	if (!pWeapon)
		return psActorFlags.test(AF_FIREPOS);

	if (!pWeapon->GetFirepos())
		return false;

	// Firepos is active if a setting matches its respective zoom state
	float zFac = pWeapon->GetZRotatingFactor();
	return (psActorFlags.test(AF_FIREPOS) && zFac < 1.f)
		|| (psActorFlags.test(AF_FIREPOS_ZOOM) && zFac >= 1.f);
}

bool CHUDManager::AimposActive()
{
	if (m_aimpos_frame != Device.dwFrame)
	{
		m_aimpos_frame = Device.dwFrame;
		m_aimpos_active = ComputeAimposActive();
	}

	return m_aimpos_active;
}

bool CHUDManager::ComputeAimposActive()
{
	// If we have an actor...
	CActor* pActor = smart_cast<CActor*>(Level().CurrentEntity());
	if (!pActor)
		return psActorFlags.test(AF_AIMPOS);

	// And a weapon...
	CWeapon* pWeapon = smart_cast<CWeapon*>(pActor->inventory().ActiveItem());
	if (!pWeapon)
		return psActorFlags.test(AF_AIMPOS);

	if (!pWeapon->GetAimpos())
		return false;

	// Aimpos is active if a setting matches its respective zoom state
	float zFac = pWeapon->GetZRotatingFactor();
	return (psActorFlags.test(AF_AIMPOS) && zFac < 1.f)
		|| (psActorFlags.test(AF_AIMPOS_ZOOM) && zFac >= 1.f);
}

ICF static BOOL pick_trace_callback(collide::rq_result& result, LPVOID params)
{
	SPickParam*	pp			= (SPickParam*)params;
	++pp->pass;

	if(result.O)
	{
		pp->result			= result;
		return FALSE;
	}else
	{
		//получить треугольник и узнать его материал
		CDB::TRI* T		= Level().ObjectSpace.GetStaticTris()+result.element;

		SGameMtl* mtl = GMLib.GetMaterialByIdx(T->material);
		pp->power		*= mtl->fVisTransparencyFactor;
		if(pp->power>0.34f)
		{
			return TRUE;
		}
	}
	pp->result				= result;
	return					FALSE;
}

bool CHUDManager::DoPick(SPickParam& pp)
{
	VERIFY(!fis_zero(pp.defs.dir.square_magnitude()));

	pp.result.set(NULL, pp.defs.range, -1);
	pp.power = 1.0f;
	pp.pass = 0;

	// Reuse the scratch buffer: the pick runs several times per frame (camera,
	// every attached hud item, crosshair occlusion), so allocating a fresh
	// result buffer for each call is pure overhead.
	RQR.r_clear();
	return Level().ObjectSpace.RayQuery(
		RQR,
		pp.defs,
		pick_trace_callback,
		&pp,
		nullptr,
		Level().CurrentEntity()
	);
}

void CHUDManager::SetCrosshairDisp	(float dispf, float disps)
{
	m_pHUDTarget->SetDispersion(psHUD_Flags.test(HUD_CROSSHAIR_DYNAMIC) ? dispf : disps);
}

#ifdef DEBUG
void CHUDManager::SetFirstBulletCrosshairDisp(float fbdispf)
{
	// The reworked crosshair system has no first-bullet debug crosshair
}
#endif

void  CHUDManager::ShowCrosshair(bool show)
{
	m_pHUDTarget->ShowCrosshair	(show);
}

void CHUDManager::HitMarked( int idx, float power, const Fvector& dir )
{
	HitMarker.Hit			(dir);
	clamp					(power,0.0f,1.0f);
	pInput->feedback		(u16(iFloor(u16(-1)*power)), u16(iFloor(u16(-1)*power)), 0.5f);
}

bool CHUDManager::AddGrenade_ForMark( CGrenade* grn )
{
	return HitMarker.AddGrenade_ForMark( grn );
}

void CHUDManager::Update_GrenadeView( Fvector& pos_actor )
{
	HitMarker.Update_GrenadeView( pos_actor );
}

void CHUDManager::SetHitmarkType( LPCSTR tex_name )
{
	HitMarker.InitShader( tex_name );
}

void CHUDManager::SetGrenadeMarkType( LPCSTR tex_name )
{
	HitMarker.InitShader_Grenade( tex_name );
}

// ------------------------------------------------------------------------------------

#include "ui/UIMainIngameWnd.h"
extern CUIXml*			pWpnScopeXml;

void CHUDManager::Load()
{
	if (!pUIGame)
	{
		pUIGame				= Game().createGameUI();
	} else
	{
		pUIGame->SetClGame	(&Game());
	}
}

void CHUDManager::OnScreenResolutionChanged()
{
	pUIGame->HideShownDialogs			();

	xr_delete							(pWpnScopeXml);

	pUIGame->UnLoad						();
	pUIGame->Load						();

	pUIGame->OnConnected				();

	if (pUIGame)
		Game().OnScreenResolutionChanged();
}

void CHUDManager::OnDisconnected()
{
	b_online				= false;
}

void CHUDManager::OnConnected()
{
	if(b_online)			return;
	b_online				= true;
}

void CHUDManager::net_Relcase( CObject* obj )
{
	HitMarker.net_Relcase		( obj );

	if(PP.result.O == obj)
		PP.result.O				= nullptr;
#ifdef	DEBUG
	DBG_PH_NetRelcase( obj );
#endif
}