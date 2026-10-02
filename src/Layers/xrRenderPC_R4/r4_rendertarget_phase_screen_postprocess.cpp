#include "stdafx.h"

void CRenderTarget::RenderEffect(ScreenPostProcessType postProcessType, bool postProcessMode) {
    u32 Offset = 0;
    float d_Z = EPS_S;
    float d_W = 1.0f;
    constexpr u32 color = color_rgba(0, 0, 0, 255);

    // Set render target
    if (postProcessMode) {
        //	Ping-pong: sample the current source buffer, render into the other one
        const ref_rt& src = m_sppSrcIsAA ? rt_Back_Buffer_AA : rt_Back_Buffer;
        const ref_rt& dst = m_sppSrcIsAA ? rt_Back_Buffer : rt_Back_Buffer_AA;

        if(rt_Back_Buffer->pTexture->pSurface != src->pSurface)
            rt_Back_Buffer->pTexture->surface_set(src->pSurface);

        u_setrt(dst, nullptr, nullptr, nullptr);
    }

    // Configure rendering settings
    RCache.set_CullMode(CULL_NONE);
    RCache.set_Stencil(false);

    // Lock and set vertices
    FVF::TL* pv = (FVF::TL*)RCache.Vertex.Lock(4, g_combine->vb_stride, Offset);
    pv->set(0.f, (float)Device.TargetHeight, d_Z, d_W, color, 0.f, 1.f); pv++;
    pv->set(0.f, 0.f, d_Z, d_W, color, 0.f, 0.f); pv++;
    pv->set((float)Device.TargetWidth, (float)Device.TargetHeight, d_Z, d_W, color, 1.f, 1.f); pv++;
    pv->set((float)Device.TargetWidth, 0.f, d_Z, d_W, color, 1.f, 0.f); pv++;
    RCache.Vertex.Unlock(4, g_combine->vb_stride);

    // Set shader and geometry
    RCache.set_Element(s_spp->E[postProcessType]);
    RCache.set_Geometry(g_combine);

    // Render
    RCache.Render(D3DPT_TRIANGLELIST, Offset, 0, 4, 0, 2);

    //	Source/destination swapped for the next effect
    if (postProcessMode) {
        m_sppSrcIsAA = !m_sppSrcIsAA;
    }
}

void CRenderTarget::PhaseAberration() {
    RenderEffect(ScreenPostProcessType::Aberration);
}

void CRenderTarget::PhaseVignette() {
    RenderEffect(ScreenPostProcessType::Vignette);
}

void CRenderTarget::PhaseSaturation() {
    RenderEffect(ScreenPostProcessType::Saturation);
}

void CRenderTarget::PhaseWinter() {
    RCache.set_xform_world(Fidentity);
    RCache.set_xform_world_old(Fidentity);
    RenderEffect(ScreenPostProcessType::Winter, false);
}
