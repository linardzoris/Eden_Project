#include "stdafx.h"

#include "blender_overdraw.h"

CBlender_overdraw::CBlender_overdraw() {
	description.CLS = 0;
}
CBlender_overdraw::~CBlender_overdraw() {}

void	CBlender_overdraw::Compile(CBlender_Compile& C) {
	IBlender::Compile(C);

	switch (C.iElement) {
		case 0: // overdraw counting (rs_overdraw): opaque-like depth state,
				// no blend/alpha in the recorded state; PS and blend state are
				// overridden at draw time by the replay pass hooks.
		C.r_Pass("dumb", "overdraw", false, TRUE, TRUE, FALSE);
		C.r_End();
		break;
	}
}
