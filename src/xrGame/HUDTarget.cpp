#include "StdAfx.h"
#include "HUDTarget.h"

#include "player_hud.h"
#include "HUDManager.h"
#include "HUDItem.h"
#include "Actor.h"
#include "Weapon.h"
#include "../xrEngine/GameMtlLib.h"

#include "../xrEngine/Environment.h"
#include "../xrEngine/CustomHUD.h"
#include "Entity.h"
#include "Level.h"
#include "game_cl_base.h"
#include "../xrEngine/IGame_Persistent.h"

#include "../../xrUI/ui_base.h"

Flags32 psCrosshair_Flags = {};

extern ENGINE_API xr_atomic_bool g_bRendering;
u32 g_crosshair_color = C_WHITE;

// Only the primary hand weapon carries a ballistics crosshair. The near one is
// anchored on the hit point of the muzzle ray, so it always sits where the bullets
// actually go; the far one is the very same ray evaluated at the far plane.
// Textures follow what Eden's gamedata actually ships: the hud\\crosshair shader and
// the single ui\\cursor texture (Monolith's ui\\cursor_cross/plus do not exist here).
// Both sizes match so the near crosshair does not shrink with distance.
CrosshairSettings g_crosshair_weapon_near = CrosshairSettings(
	{
		CROSSHAIR_SHOW |
		CROSSHAIR_RECON
	},
	"hud\\crosshair",
	"ui\\cursor",
	40.f,
	16.f,
	0.f,
	C_WHITE,
	.25f,
	40.f,
	.5f
);
CrosshairSettings g_crosshair_weapon_far = CrosshairSettings(
	{
		CROSSHAIR_RECON
	},
	"hud\\crosshair",
	"ui\\cursor",
	40.f,
	16.f,
	25.f,
	C_WHITE,
	.25f,
	40.f,
	.5f
);

#define NEAR_LIM	0.5f

static float lerp(float a, float b, float t)
{
	clamp(t, 0.f, 1.f);
	return a * (1 - t) + b * t;
}

static float remap(float value, float from1, float to1, float from2, float to2) {
	return (value - from1) / (to1 - from1) * (to2 - from2) + from2;
}

static bool is_occluded(const Fvector& pos)
{
	Fvector dir = Fvector().sub(pos, Device.vCameraPosition);
	float dist = dir.magnitude();
	dir.normalize();
	SPickParam op = SPickParam(CDB::OPT_CULL | CDB::OPT_ONLYFIRST);
	op.defs.start = Device.vCameraPosition;
	op.defs.dir = dir;
	op.defs.range = dist * 0.99f;
	return HUD().DoPick(op);
}

void TargetCrosshair::IntegratePosition(const SPickParam& pp, float dist, bool is_far)
{
	// Transform ray start and direction into camera space
	// Eden: barrel matrices are already in world space
	Fvector p, d;

	Fmatrix mat = pp.barrel_matrix;

	// Ensure no NaNs creep into interpolation
	if (!_valid(mat))
		return;

	p = mat.c;
	d = mat.k;
	Device.mView.transform_tiny(p);
	Device.mView.transform_dir(d);

	clamp(dist, 0.f, dist);

	Fvector target = Fvector().add(p, Fvector().mul(d, dist));

	// Interpolate crosshair position toward target
	if (Is(CROSSHAIR_DISTANCE_LERP))
	{
		float fac = 1 - (target.z / pp.defs.range);
		float t = Device.fTimeDelta * (fac + settings.distance_lerp_rate);
		clamp(t, 0.f, 1.f);
		pos.lerp(pos, target, t);
	}
	else
		pos = target;
}

void TargetCrosshair::IntegrateOpacity(const SPickParam& pp, float opacity_target)
{
	// Interpolate opacity offset toward target
	opacity = lerp(opacity, opacity_target, Device.fTimeDelta * settings.occlusion_fade_rate);
}

void TargetCrosshair::Update(const SPickParam& pp, bool is_far)
{
	float zFar = g_pGamePersistent->Environment().CurrentEnv->far_plane;
	float dist = is_far ? zFar : pp.result.range;

	IntegratePosition(pp, dist, is_far);

	// Construct aim point matrix
	Fmatrix mInvView = Fmatrix().invert(Device.mView);
	Fmatrix mat_aim = Fmatrix().identity();
	mat_aim.mulB_43(mInvView);
	mat_aim.mulB_43(Fmatrix().translate(pos));

	// Readout color
	recon.SetTransform(mat_aim);
	recon.Update(pp);

	float recon_opacity = opacity;
	clamp(recon_opacity, 0.f, settings.recon_max_opacity);
	recon.SetOpacity(recon_opacity);

	// Use the crosshair color unless the readout color is non-default
	u32 color_readout = recon.GetColor();
	u32 color_crosshair = (color_readout & color_rgba(0xff, 0xff, 0xff, 0)) == (C_WHITE & color_rgba(0xff, 0xff, 0xff, 0)) ? settings.color : color_readout;

	// Modulate by global crosshair color
	color_crosshair = color_argb(
		(u8)(((color_get_A(color_crosshair) / 255.f) * (color_get_A(g_crosshair_color) / 255.f)) * 255.f),
		(u8)(((color_get_R(color_crosshair) / 255.f) * (color_get_R(g_crosshair_color) / 255.f)) * 255.f),
		(u8)(((color_get_G(color_crosshair) / 255.f) * (color_get_G(g_crosshair_color) / 255.f)) * 255.f),
		(u8)(((color_get_B(color_crosshair) / 255.f) * (color_get_B(g_crosshair_color) / 255.f)) * 255.f)
	);

	// Modulate color alpha
	color_crosshair = subst_alpha(color_crosshair, u8(iFloor(255.f * opacity)));
	crosshair.SetColor(color_crosshair);

	crosshair.SetShader(&settings.shader);
	crosshair.SetTexture(&settings.texture);

	// If aimpos is active
	if (HUD().AimposActive())
	{
		// Rotate the crosshair
		Fvector hpb_barrel, hpb_cam;
		pp.barrel_matrix.getHPB(hpb_barrel);
		mInvView.getHPB(hpb_cam);
		mat_aim.mulB_43(Fmatrix().setHPB(0, 0, hpb_barrel.z - hpb_cam.z));
	}
	crosshair.SetTransform(mat_aim);

	// Occlusion only ever affects the rendered near crosshair, so don't trace for
	// the far one or for crosshairs that are not drawn at all.
	float opacity_target = 1.f;
	if (!is_far && Is(CROSSHAIR_SHOW) && !pp.barrel_blocked)
	{
		if (is_occluded(Fvector().add(pp.defs.start, Fvector().mul(pp.defs.dir, dist))))
			opacity_target = settings.occluded_opacity;
	}

	IntegrateOpacity(pp, opacity_target);
}

void TargetCrosshair::Render(const SPickParam& pp)
{
	if (Is(CROSSHAIR_RECON))
		recon.Render();

	// Update the crosshair's transform and color, and draw it
	crosshair.OnRender(Is(CROSSHAIR_USE_SHADER));
}

void CrosshairPair::Load()
{
	crosshair_near.Load();
	crosshair_far.Load();
}

void CrosshairPair::Update(const SPickParam& pp)
{
	// Crosshairs that are neither drawn nor used for the aim line would only burn
	// pick traces and readout updates, so leave them alone. This is by far the
	// most expensive part of the hud: every Update may run an occlusion trace.
	if (crosshair_near.Is(CROSSHAIR_SHOW) || crosshair_near.Is(CROSSHAIR_LINE))
		crosshair_near.Update(pp, false);

	if (crosshair_far.Is(CROSSHAIR_SHOW) || crosshair_far.Is(CROSSHAIR_LINE))
		crosshair_far.Update(pp, true);

	float zFar = g_pGamePersistent->Environment().CurrentEnv->far_plane;

	// Scale near crosshair
	Fmatrix mat_aim = crosshair_near.crosshair.GetTransform();
	Fmatrix mat = Fmatrix().mul(Device.mFullTransform, mat_aim);
	Fvector4 pos = Fvector4().set(mat._41, mat._42, mat._43, mat._44);
	// Size blend: past the near depth the crosshair drifts toward the far size.
	// remap() does not clamp, so an aim point deeper than the far depth would
	// extrapolate into a negative scale and flip the quad.
	float near_depth = crosshair_near.settings.depth / zFar;
	float far_depth = crosshair_far.settings.depth / zFar;
	float t = 1.f;
	if (!fis_zero(far_depth - near_depth))
	{
		t = remap(pos.w / zFar, near_depth, far_depth, 0.f, 1.f);
		clamp(t, 0.f, 1.f);
	}
	float near_size = pos.w * lerp(crosshair_near.settings.size, crosshair_far.settings.size, t) * (Device.fFOV / 90.f);
	crosshair_near.crosshair.SetScale(near_size);

	// Scale far crosshair
	mat_aim = crosshair_far.crosshair.GetTransform();
	mat = Fmatrix().mul(Device.mFullTransform, mat_aim);
	float far_size = mat._44 * crosshair_far.settings.size * (Device.fFOV / 90.f);
	crosshair_far.crosshair.SetScale(far_size);
}

void CrosshairPair::RenderAimLine(
	Fvector va,
	Fvector vb
) const
{
	Fvector2 scr_size = {
		float(::Render->getTarget()->get_width()),
		float(::Render->getTarget()->get_height())
	};

	int verts = 2;
	if (crosshair_near.Is(CROSSHAIR_LINE))
		verts++;
	if (crosshair_far.Is(CROSSHAIR_LINE))
		verts++;

	UIRender->StartPrimitive(verts, IUIRender::ptLineStrip, UI().m_currentPointType);

	// Eden: va/vb are already in world space
	Device.mFullTransform.transform(va);
	va.x = (va.x + 1.f) * 0.5f * scr_size.x;
	va.y = (-va.y + 1.f) * 0.5f * scr_size.y;

	Device.mFullTransform.transform(vb);
	vb.x = (vb.x + 1.f) * 0.5f * scr_size.x;
	vb.y = (-vb.y + 1.f) * 0.5f * scr_size.y;

	Fvector vc = crosshair_near.pos;
	Device.mProject.transform(vc);
	vc.x = (vc.x + 1.f) * 0.5f * scr_size.x;
	vc.y = (-vc.y + 1.f) * 0.5f * scr_size.y;

	Fvector vd = crosshair_far.pos;
	Device.mProject.transform(vd);
	vd.x = (vd.x + 1.f) * 0.5f * scr_size.x;
	vd.y = (-vd.y + 1.f) * 0.5f * scr_size.y;

	u32 near_color = crosshair_near.crosshair.GetColor();
	u32 far_color = crosshair_far.crosshair.GetColor();

	UIRender->PushPoint(va.x, va.y, 0, subst_alpha(near_color, 0), 0, 0);
	UIRender->PushPoint(vb.x, vb.y, 0, near_color, 0, 0);

	if (crosshair_near.Is(CROSSHAIR_LINE))
		UIRender->PushPoint(vc.x, vc.y, 0, near_color, 0, 0);

	if (crosshair_far.Is(CROSSHAIR_LINE))
		UIRender->PushPoint(vd.x, vd.y, 0, far_color, 0, 0);

	UIRender->SetShader(*shaderWire);
	UIRender->FlushPrimitive();
}

void CrosshairPair::Render(const SPickParam& pp)
{
	BOOL b_do_rendering = (psHUD_Flags.is(HUD_CROSSHAIR | HUD_CROSSHAIR_RT | HUD_CROSSHAIR_RT2));
	if (!b_do_rendering)
		return;

	VERIFY(g_bRendering);

	if (crosshair_near.Is(CROSSHAIR_SHOW))
		crosshair_near.Render(pp);

	if (crosshair_far.Is(CROSSHAIR_SHOW))
		crosshair_far.Render(pp);

	if (crosshair_near.Is(CROSSHAIR_LINE) || crosshair_far.Is(CROSSHAIR_LINE))
		RenderAimLine(
			Fvector().sub(pp.barrel_matrix.c, Fvector().mul(pp.barrel_matrix.k, .05f)),
			pp.barrel_matrix.c
		);
}

CHUDTarget::CHUDTarget() :
	m_weapon(CrosshairPair(g_crosshair_weapon_near, g_crosshair_weapon_far))
{
	m_bShowCrosshair = false;
	Load();
}

CHUDTarget::~CHUDTarget()
{
}

void CHUDTarget::Load()
{
	m_weapon.Load();
}

void CHUDTarget::ShowCrosshair(bool b)
{
	m_bShowCrosshair = b;
}

void CHUDTarget::Render()
{
	// The crosshair pairs are the only consumers of the per-frame occlusion
	// traces and readout updates, so bail out before doing that work when the
	// crosshair is disabled.
	if (!psHUD_Flags.is(HUD_CROSSHAIR | HUD_CROSSHAIR_RT | HUD_CROSSHAIR_RT2))
		return;

	const static bool noHUDonMaster = EngineExternal()[EEngineExternalUI::DisableHudRenderingOnMaster];
	bool renderHUDTarget = noHUDonMaster ? g_SingleGameDifficulty < egdVeteran : true;

	if (!renderHUDTarget)
		return;

	if (load_screen_renderer.IsActive())
		return;

	CActor* pActor = Actor();

	if (!pActor)
		return;

	if (!m_bShowCrosshair)
		return;

	bool get_motions = pActor->active_cam() == eacLookAt && ((pActor->GetMovementState(eReal) & ACTOR_DEFS::EMoveCommand::mcSprint) || (pActor->GetMovementState(eReal) & ACTOR_DEFS::EMoveCommand::mcJumpSeq));

	if (get_motions)
		return;

	// Only the primary hand weapon is picked and drawn
	attachable_hud_item* pAttach = g_player_hud ? g_player_hud->attached_item(0) : nullptr;
	if (!pAttach)
		return;

	CHudItem* pItem = pAttach->m_parent_hud_item;
	if (!pItem)
		return;

	const SPickParam* pick = &pItem->GetPick();
	m_weapon.Update(*pick);
	m_weapon.Render(*pick);
}
