#pragma once

//==============================================================================
// dx12SamplerStateCache.h  (对应 xrRenderDX10/StateManager/dx10SamplerStateCache.h)
//
// DX12：采样器主要通过根签名静态采样器提供；此处仅维护全局各向异性 / MipBias
// 等设置，供 PSO 或静态采样器参数使用。
//==============================================================================

class dx12SamplerStateCache
{
public:
	dx12SamplerStateCache();
	~dx12SamplerStateCache();

	void	ResetDeviceState() {}
	void	SetMaxAnisotropy(UINT uiMaxAniso) { m_uiMaxAnisotropy = uiMaxAniso; }
	void	SetMipLodBias(FLOAT fMipLodBias) { m_fMipLodBias = fMipLodBias; }
	void	SetMinLod(FLOAT fMinLod) { m_fMinLod = fMinLod; }
	void	SetMaxLod(FLOAT fMaxLod) { m_fMaxLod = fMaxLod; }

	dx12SamplerState*	GetState(const D3D_SAMPLER_DESC& desc);

public:
	UINT	m_uiMaxAnisotropy = 16;
	FLOAT	m_fMipLodBias = 0.0f;
	FLOAT	m_fMinLod = 0.0f;
	FLOAT	m_fMaxLod = D3D11_FLOAT32_MAX;
};

extern dx12SamplerStateCache	SSManager;
