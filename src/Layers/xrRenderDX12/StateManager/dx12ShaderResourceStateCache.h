#pragma once

//==============================================================================
// dx12ShaderResourceStateCache.h
// (对应 xrRenderDX10/StateManager/dx10ShaderResourceStateCache.h)
//
// 记录各阶段当前绑定的 SRV；DX12 在 Draw 时把 SRV 拷入 shader-visible 堆，
// 再通过根描述符表绑定。
//==============================================================================

class dx12ShaderResourceStateCache
{
public:
	dx12ShaderResourceStateCache();
	~dx12ShaderResourceStateCache();

	void	ResetDeviceState();
	void	Apply();

	void	SetPSResource(UINT uiSlot, ID3DShaderResourceView* pRes);
	void	SetVSResource(UINT uiSlot, ID3DShaderResourceView* pRes);
	void	SetGSResource(UINT uiSlot, ID3DShaderResourceView* pRes);
	void	SetHSResource(UINT uiSlot, ID3DShaderResourceView* pRes);
	void	SetDSResource(UINT uiSlot, ID3DShaderResourceView* pRes);
	void	SetCSResource(UINT uiSlot, ID3DShaderResourceView* pRes);

	ID3DShaderResourceView*	GetPS(UINT slot) const { return (slot < kSlots) ? m_ps[slot] : nullptr; }
	ID3DShaderResourceView*	GetVS(UINT slot) const { return (slot < kSlots) ? m_vs[slot] : nullptr; }

public:
	static const UINT	kSlots = 16;

	ID3DShaderResourceView*	m_ps[kSlots] = {};
	ID3DShaderResourceView*	m_vs[kSlots] = {};
	ID3DShaderResourceView*	m_gs[kSlots] = {};
	ID3DShaderResourceView*	m_hs[kSlots] = {};
	ID3DShaderResourceView*	m_ds[kSlots] = {};
	ID3DShaderResourceView*	m_cs[kSlots] = {};
};

extern dx12ShaderResourceStateCache	SRVSManager;
