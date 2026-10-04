#include "stdafx.h"

// R4 的 FSR / DLSS / XESS 上采样走 DX11 的 OverlayAPI，DX12 阶段先提供空实现，
// 保证链接可通（后续可接入 FSR3/DLSS-DX12/XeSS-DX12）。
#ifdef USE_DX12

void CRenderTarget::init_fsr() {}
bool CRenderTarget::phase_fsr() { return false; }

void CRenderTarget::init_dlss() {}
bool CRenderTarget::phase_dlss() { return false; }

void CRenderTarget::init_xess() {}
bool CRenderTarget::phase_xess() { return false; }

#endif // USE_DX12
