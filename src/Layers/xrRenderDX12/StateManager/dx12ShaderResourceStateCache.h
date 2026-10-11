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

	// 诊断：记录各 PS 槽位最近绑定的纹理名（拷贝存储，避免源串生命周期问题）。
	// FlushPipeline 检测到 SRV 维度错配时打印，用来定位"哪张纹理被绑到了 cube 槽位"。
	void	SetPSName(UINT uiSlot, const char* name);
	const char*	PSName(UINT slot) const { return (slot < kSlots) ? m_psName[slot] : ""; }

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
	char					m_psName[kSlots][64] = {};
};

extern dx12ShaderResourceStateCache	SRVSManager;
