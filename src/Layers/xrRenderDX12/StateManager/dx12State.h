#pragma once

//==============================================================================
// dx12State.h  (对应 xrRenderDX10/StateManager/dx10State.h)
//
// DX12 没有独立的状态对象：这里保存「模拟状态」的描述，Apply() 时交给
// dx12StateManager 累积，最终在 Draw 前合成 PSO。
//==============================================================================

class SimulatorStates;

class dx12State
{
public:
	dx12State();
	~dx12State();

	static dx12State* Create(SimulatorStates& state_code);

	HRESULT	Apply();
	void	Release();

	void	UpdateStencilRef(UINT Ref) { m_uiStencilRef = Ref; }
	void	UpdateAlphaRef(UINT Ref) { m_uiAlphaRef = Ref; }

public:
	// 描述（供 PSO 合成）
	D3D_RASTERIZER_DESC		m_RDesc = {};
	D3D_DEPTH_STENCIL_DESC	m_DSDesc = {};
	D3D_BLEND_DESC			m_BDesc = {};

	UINT					m_uiStencilRef = 0;
	UINT					m_uiAlphaRef = 0;
	UINT					m_uiSampleMask = 0xFFFFFFFF;

private:
	bool					m_bRDValid = false;
	bool					m_bDSDValid = false;
	bool					m_bBDValid = false;
};
