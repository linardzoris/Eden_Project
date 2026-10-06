// HUDRecon.h: Distance and identification readout
//
//////////////////////////////////////////////////////////////////////

#pragma once

#define HUD_CURSOR_SECTION "hud_cursor"

#include "../../xrUI/ui_defs.h"

struct SPickParam;

class CHUDRecon
{
private:
	Fmatrix transform;
	u32 color;
	float dist;
	float power;
	u32 pass;

	LPCSTR line1;
	LPCSTR line2;

	float fuzzyShowInfo;

public:
	CHUDRecon();
	~CHUDRecon();

	u32 GetColor() { return color; };

	void SetOpacity(float a);
	void SetTransform(const Fmatrix& m);

	void Update(const SPickParam& pp);
	void Render() const;
};
