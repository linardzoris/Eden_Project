#include "stdafx.h"

void CRenderTarget::phase_smap_direct(light* L, u32 sub_phase)
{
	u_setrt(nullptr, nullptr, nullptr, rt_smap_depth->pZRT);
	RContext->ClearDepthStencilView(rt_smap_depth->pZRT, D3D_CLEAR_DEPTH, 1.0f, 0L);

	//	Prepare viewport for shadow map rendering
	if(sub_phase != SE_SUN_RAIN_SMAP) {
		// Far sun cascade: render the shadow map into the top-left quarter at
		// half resolution (r__sun_far_smap_half); accumulate remaps shadow UVs
		// to match.
		if (SE_SUN_FAR == sub_phase && RImplementation.sun_smap_halfres) {
			D3D_VIEWPORT VP = {
				0.0f, 0.0f,
				float(RImplementation.o.smapsize / 2),
				float(RImplementation.o.smapsize / 2),
				0.0f, 1.0f
			};

			RContext->RSSetViewports(1, &VP);
		}
		else {
			RImplementation.rmNormal();
		}
	}
	else {
		D3D_VIEWPORT VP = {
			(float)L->X.D.minX,(float)L->X.D.minY,
			(float)(L->X.D.maxX - L->X.D.minX),
			(float)(L->X.D.maxY - L->X.D.minY),
			0.0f, 1.0f
		};

		RContext->RSSetViewports(1, &VP);
	}

	// Stencil	- disable
	RCache.set_Stencil(FALSE);
}

void CRenderTarget::phase_smap_direct_tsh(light* L, u32 sub_phase)
{
	VERIFY(RImplementation.o.Tshadows);
	FLOAT ColorRGBA[4] = {1.0f, 1.0f, 1.0f, 1.0f};
	RCache.set_ColorWriteEnable();
	//	Prepare viewport for shadow map rendering
	RImplementation.rmNormal();
	RContext->ClearRenderTargetView(RCache.get_RT(0), ColorRGBA);
}
