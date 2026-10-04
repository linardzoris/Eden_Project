#include "stdafx.h"
#include "dx12ShaderResourceStateCache.h"

dx12ShaderResourceStateCache	SRVSManager;

dx12ShaderResourceStateCache::dx12ShaderResourceStateCache() {}
dx12ShaderResourceStateCache::~dx12ShaderResourceStateCache() {}

void dx12ShaderResourceStateCache::ResetDeviceState()
{
	ZeroMemory(m_ps, sizeof(m_ps));
	ZeroMemory(m_vs, sizeof(m_vs));
	ZeroMemory(m_gs, sizeof(m_gs));
	ZeroMemory(m_hs, sizeof(m_hs));
	ZeroMemory(m_ds, sizeof(m_ds));
	ZeroMemory(m_cs, sizeof(m_cs));
}

void dx12ShaderResourceStateCache::Apply()
{
	// DX12：描述符在 Draw 时绑定，这里无需提交。
}

void dx12ShaderResourceStateCache::SetPSResource(UINT uiSlot, ID3DShaderResourceView* pRes)
{
	if (uiSlot < kSlots) m_ps[uiSlot] = pRes;
}
void dx12ShaderResourceStateCache::SetVSResource(UINT uiSlot, ID3DShaderResourceView* pRes)
{
	if (uiSlot < kSlots) m_vs[uiSlot] = pRes;
}
void dx12ShaderResourceStateCache::SetGSResource(UINT uiSlot, ID3DShaderResourceView* pRes)
{
	if (uiSlot < kSlots) m_gs[uiSlot] = pRes;
}
void dx12ShaderResourceStateCache::SetHSResource(UINT uiSlot, ID3DShaderResourceView* pRes)
{
	if (uiSlot < kSlots) m_hs[uiSlot] = pRes;
}
void dx12ShaderResourceStateCache::SetDSResource(UINT uiSlot, ID3DShaderResourceView* pRes)
{
	if (uiSlot < kSlots) m_ds[uiSlot] = pRes;
}
void dx12ShaderResourceStateCache::SetCSResource(UINT uiSlot, ID3DShaderResourceView* pRes)
{
	if (uiSlot < kSlots) m_cs[uiSlot] = pRes;
}
