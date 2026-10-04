#pragma once

//==============================================================================
// dx12StateManager.h  (对应 xrRenderDX10/StateManager/dx10StateManager.h)
//
// 累积 DX9/DX10 风格的零散状态调用（SetStencil/SetZ/CullMode...），
// 在 Draw 时与 shader/输入布局一起合成 PSO。DX12 无独立状态对象，故
// SetXRState(对象) 仅记录描述。
//==============================================================================

struct R_constant;

class dx12StateManager
{
public:
	dx12StateManager();
	~dx12StateManager();

	void	Reset();
	void	Apply();
	void	UnmapConstants();

	void	SetRasterizerState(ID3DRasterizerState* pRState);
	void	SetDepthStencilState(ID3DDepthStencilState* pDSState);
	void	SetBlendState(ID3DBlendState* pBlendState);
	void	SetStencilRef(UINT uiStencilRef);
	void	SetAlphaRef(UINT uiAlphaRef);
	void	BindAlphaRef(R_constant* C);

	void	SetStencil(u32 Enable, u32 Func, u32 Ref, u32 Mask, u32 WriteMask, u32 Fail, u32 Pass, u32 ZFail);
	void	SetDepthFunc(u32 Func);
	void	SetDepthEnable(u32 Enable);
	void	SetColorWriteEnable(u32 WriteMask);
	void	SetCullMode(u32 Mode);
	void	SetMultisample(u32 Enable);
	void	SetSampleMask(u32 Mask);

	void	EnableScissoring(BOOL bEnable = TRUE);
	void	OverrideScissoring(bool bOverride = true, BOOL bValue = TRUE);

public:
	// 当前累积的状态描述（供 PSO 合成）
	D3D_RASTERIZER_DESC		m_RDesc = {};
	D3D_DEPTH_STENCIL_DESC	m_DSDesc = {};
	D3D_BLEND_DESC			m_BDesc = {};

	UINT					m_uiStencilRef = 0;
	UINT					m_uiAlphaRef = 0;
	UINT					m_uiSampleMask = 0xFFFFFFFF;

	bool					m_bOverrideScissoring = false;
	BOOL					m_bOverrideScissoringValue = TRUE;

private:
	void	ValidateRDesc();
	void	ValidateDSDesc();
	void	ValidateBDesc();

private:
	R_constant*				m_cAlphaRef = nullptr;

	bool					m_bRSNeedApply = false;
	bool					m_bDSSNeedApply = false;
	bool					m_bBSNeedApply = false;
	bool					m_bRSChanged = false;
	bool					m_bDSSChanged = false;
	bool					m_bBSChanged = false;
	bool					m_bRDInvalid = true;
	bool					m_bDSDInvalid = true;
	bool					m_bBDInvalid = true;
};

extern dx12StateManager	StateManager;
