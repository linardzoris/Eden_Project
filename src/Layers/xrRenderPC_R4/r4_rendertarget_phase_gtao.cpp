#include "stdafx.h"

#include "r4_rendertarget.h"

// Defined in r4_rendertarget_phase_ssao.cpp
void set_viewport(ID3DDeviceContext* dev, float w, float h);

void CRenderTarget::phase_gtao()
{
	GPU_EVENT(phase_gtao);

	u32 Offset = 0;
    constexpr u32 vertex_color = color_rgba(0, 0, 0, 255);

	//Calculate projection factor, to transform world radius to screen space
	float p_scale = RCache.get_height() / (tan(deg2rad(Device.fFOV) * 0.5f) * 2.0f);
	p_scale *= 0.5;
	FVF::TL* pv = nullptr;

	// r4_gtao_resolution: AO buffers (rt_gtao_0/rt_gtao_filtered) may be half-res.
	// Shaders stay resolution-agnostic: gtao_render offsets are normalized UVs
	// (p_scale is derived from the full-res target), the filter gets explicit dims.
	const bool gtao_half = rt_gtao_filtered._get() != nullptr;
	if (gtao_half)
		set_viewport(RContext, float(rt_gtao_0->dwWidth), float(rt_gtao_0->dwHeight));

	{
		GPU_EVENT(gtao_render);
		//Render the AO and view-z into new rendertarget
		u_setrt(rt_gtao_0, nullptr, nullptr, nullptr);
		RCache.set_CullMode(CULL_NONE);
		RCache.set_Stencil(FALSE);

		pv = (FVF::TL*)RCache.Vertex.Lock(3, g_combine->vb_stride, Offset);
		pv->set(-1.0, 1.0, 1.0, 1.0, vertex_color, 0.0, 0.0);
		pv++;
		pv->set(3.0, 1.0, 1.0, 1.0, vertex_color, 2.0, 0.0);
		pv++;
		pv->set(-1.0, -3.0, 1.0, 1.0, vertex_color, 0.0, 2.0);
		pv++;
		RCache.Vertex.Unlock(3, g_combine->vb_stride);

		//Go go power rangers
		RCache.set_Element(s_gtao->E[0]);
		RCache.set_c("gtao_parameters", p_scale);
		RCache.set_Geometry(g_combine);
		RCache.Render(D3DPT_TRIANGLELIST, Offset, 0, 3, 0, 1);
	}

	{
		GPU_EVENT(gtao_filter);
		//Blur... Half-res mode filters into rt_gtao_filtered (up-sampled on s_occ read)
		u_setrt(gtao_half ? rt_gtao_filtered : rt_ssao_temp, nullptr, nullptr, nullptr);
		RCache.set_CullMode(CULL_NONE);
		RCache.set_Stencil(FALSE);

		pv = (FVF::TL*)RCache.Vertex.Lock(3, g_combine->vb_stride, Offset);
		pv->set(-1.0, 1.0, 1.0, 1.0, vertex_color, 0.0, 0.0);
		pv++;
		pv->set(3.0, 1.0, 1.0, 1.0, vertex_color, 2.0, 0.0);
		pv++;
		pv->set(-1.0, -3.0, 1.0, 1.0, vertex_color, 0.0, 2.0);
		pv++;
		RCache.Vertex.Unlock(3, g_combine->vb_stride);

		//Go go power rangers
		RCache.set_Element(s_gtao->E[1]);
		RCache.set_c("gtao_filter_params",
			float(rt_gtao_0->dwWidth), float(rt_gtao_0->dwHeight),
			1.0f / float(rt_gtao_0->dwWidth), 1.0f / float(rt_gtao_0->dwHeight));
		RCache.set_c("gtao_intensity", ps_r4_gtao_intensity);
		RCache.set_Geometry(g_combine);
		RCache.Render(D3DPT_TRIANGLELIST, Offset, 0, 3, 0, 1);
	}

	if (gtao_half)
	{
		//Restore full-res viewport for the following passes
		set_viewport(RContext, RCache.get_width(), RCache.get_height());

		//Point s_occ (bound to r2_RT_ssao_temp) at the half-res filtered AO.
		//combine_1 samples it with bilinear at normalized UVs - free up-sampling.
		rt_ssao_temp->pTexture->surface_set(rt_gtao_filtered->pSurface);
	}
	else
	{
		//Full-res GTAO writes rt_ssao_temp directly - make sure s_occ reads it
		rt_ssao_temp->pTexture->surface_set(rt_ssao_temp->pSurface);
	}
}

// P1: ping-pong the current/history SSLR buffers instead of a full-screen
// CopyResource every frame. The named CTextures ("$user$sslr" /
// "$user$sslr_old") are re-pointed to the swapped surfaces (shader binds
// resolve through them), and the CRT raw device objects are exchanged so
// that u_setrt() keeps writing the "current" buffer by member name.
static void swap_sslr_history(ref_rt& cur, ref_rt& hist)
{
	cur->pTexture->surface_set(hist->pSurface);
	hist->pTexture->surface_set(cur->pSurface);

	ID3DTexture2D* tmpSurface = cur->pSurface;
	cur->pSurface = hist->pSurface;
	hist->pSurface = tmpSurface;

	ID3DRenderTargetView* tmpRT = cur->pRT;
	cur->pRT = hist->pRT;
	hist->pRT = tmpRT;
}

static void sslr_set_viewport(float w, float h)
{
	D3D_VIEWPORT viewport[1] =
	{
		0, 0, w, h, 0.f, 1.f
	};
	RContext->RSSetViewports(1, viewport);
}

void CRenderTarget::phase_sslr() {
	GPU_EVENT(phase_sslr);
	u32 Offset = 0;
	constexpr u32 vertex_color = color_rgba(0, 0, 0, 255);
	FVF::TL* pv = nullptr;

	// P2: isolate passes for GPU timing (r4_sslr_debug): 1 = skip filter,
	// 2 = skip temporal, 3 = skip both. Measure by difference between modes.
	const bool sslr_skip_filter = (ps_r4_sslr_debug & 1) != 0;
	const bool sslr_skip_temporal = (ps_r4_sslr_debug & 2) != 0;

	// P1: trace + filter resolution comes from the RT allocated for the tier
  // selected by r4_sslr_quality (0/1 half-res, 2 full-res). The same value is
  // exported to shaders as sslr_params (.x = tier, .y = resolution scale).
  const float sslr_res_scale = (ps_r4_sslr_quality >= 2) ? 1.0f : 0.5f;
  sslr_set_viewport(float(rt_sslr_point->dwWidth), float(rt_sslr_point->dwHeight));

	{
		GPU_EVENT(sslr_render);
		//Render the AO and view-z into new rendertarget
		u_setrt(rt_sslr_point, rt_sslr_data, nullptr, nullptr);
		RCache.set_CullMode(CULL_NONE);

		pv = (FVF::TL*)RCache.Vertex.Lock(3, g_combine->vb_stride, Offset);
		pv->set(-1.0, 1.0, 1.0, 1.0, vertex_color, 0.0, 0.0);
		pv++;
		pv->set(3.0, 1.0, 1.0, 1.0, vertex_color, 2.0, 0.0);
		pv++;
		pv->set(-1.0, -3.0, 1.0, 1.0, vertex_color, 0.0, 2.0);
		pv++;
		RCache.Vertex.Unlock(3, g_combine->vb_stride);

		//Go go power rangers
		RCache.set_Element(s_gtao->E[2]);
		// .w = r4_sslr_max_dist: distance cut-off + step-budget grading in sslr_render.ps
		RCache.set_c("sslr_params", float(ps_r4_sslr_quality), sslr_res_scale, ps_r4_sslr_intensity, ps_r4_sslr_max_dist);
		RCache.set_Geometry(g_combine);
		RCache.Render(D3DPT_TRIANGLELIST, Offset, 0, 3, 0, 1);
	}

	if(!sslr_skip_filter)
	{
		GPU_EVENT(sslr_filter);
		u_setrt(rt_sslr_temp, nullptr, nullptr, nullptr);
		RCache.set_CullMode(CULL_NONE);

		pv = (FVF::TL*)RCache.Vertex.Lock(3, g_combine->vb_stride, Offset);
		pv->set(-1.0, 1.0, 1.0, 1.0, vertex_color, 0.0, 0.0);
		pv++;
		pv->set(3.0, 1.0, 1.0, 1.0, vertex_color, 2.0, 0.0);
		pv++;
		pv->set(-1.0, -3.0, 1.0, 1.0, vertex_color, 0.0, 2.0);
		pv++;
		RCache.Vertex.Unlock(3, g_combine->vb_stride);

		//Go go power rangers
		RCache.set_Element(s_gtao->E[3]);
		// .w = r4_sslr_max_dist: distance/roughness tap grading in sslr_filter.ps
		RCache.set_c("sslr_params", float(ps_r4_sslr_quality), sslr_res_scale, 0.0f, ps_r4_sslr_max_dist);
		RCache.set_Geometry(g_combine);
		RCache.Render(D3DPT_TRIANGLELIST, Offset, 0, 3, 0, 1);
	}

	// P1: temporal runs at full resolution and performs the depth-aware
  // upsample of the half-res filtered buffer.
  sslr_set_viewport(RCache.get_width(), RCache.get_height());

  // P2: r4_sslr_debug & 2 skips the whole temporal step (history swap included).
  // The temporal block is the last thing this phase does, so a plain early-out
  // keeps the diff minimal.
  if(sslr_skip_temporal)
    return;

  // P1: ping-pong - exchange current/history BEFORE temporal so it samples
  // last frame's result through "$sslr_old" and renders the new frame into
  // the other physical buffer (same-frame readers of "$sslr" stay correct).
  // First frame skips the swap and bootstraps the history afterwards.
  static bool sslr_history_initialized = false;
  if(sslr_history_initialized)
  {
    swap_sslr_history(rt_sslr, rt_sslr_old);
  }

  {
		GPU_EVENT(sslr_temporal);
		u_setrt(rt_sslr, nullptr, nullptr, nullptr);
		RCache.set_CullMode(CULL_NONE);

		pv = (FVF::TL*)RCache.Vertex.Lock(3, g_combine->vb_stride, Offset);
		pv->set(-1.0, 1.0, 1.0, 1.0, vertex_color, 0.0, 0.0);
		pv++;
		pv->set(3.0, 1.0, 1.0, 1.0, vertex_color, 2.0, 0.0);
		pv++;
		pv->set(-1.0, -3.0, 1.0, 1.0, vertex_color, 0.0, 2.0);
		pv++;
		RCache.Vertex.Unlock(3, g_combine->vb_stride);

		//Go go power rangers
		RCache.set_Element(s_gtao->E[4]);
		RCache.set_c("sslr_params", float(ps_r4_sslr_quality), sslr_res_scale, 0.0f, 0.0f);
		RCache.set_Geometry(g_combine);
		RCache.Render(D3DPT_TRIANGLELIST, Offset, 0, 3, 0, 1);
	}

	// P1: first frame only - seed the history buffer so next frame's temporal
  // pass doesn't sample uninitialized memory.
  if(!sslr_history_initialized)
  {
    RContext->CopyResource(rt_sslr_old->pSurface, rt_sslr->pSurface);
    sslr_history_initialized = true;
  }
}