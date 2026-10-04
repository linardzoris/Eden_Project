#include "stdafx.h"
#include "dx12StateManager.h"

dx12StateManager StateManager;

dx12StateManager::dx12StateManager()
{
	memset(&m_RDesc, 0, sizeof(m_RDesc));
	memset(&m_DSDesc, 0, sizeof(m_DSDesc));
	memset(&m_BDesc, 0, sizeof(m_BDesc));
	Reset();
}

dx12StateManager::~dx12StateManager()
{
}

void dx12StateManager::Reset()
{
	m_bRSChanged = true;
	m_bDSSChanged = true;
	m_bBSChanged = true;
	m_bRDInvalid = true;
	m_bDSDInvalid = true;
	m_bBDInvalid = true;

	// D3D11 默认状态
	m_RDesc.FillMode = D3D_FILL_SOLID;
	m_RDesc.CullMode = D3D_CULL_BACK;
	m_RDesc.FrontCounterClockwise = FALSE;
	m_RDesc.DepthBias = 0;
	m_RDesc.DepthBiasClamp = 0.0f;
	m_RDesc.SlopeScaledDepthBias = 0.0f;
	m_RDesc.DepthClipEnable = TRUE;
	m_RDesc.ScissorEnable = FALSE;
	m_RDesc.MultisampleEnable = FALSE;
	m_RDesc.AntialiasedLineEnable = FALSE;

	m_DSDesc.DepthEnable = TRUE;
	m_DSDesc.DepthWriteMask = D3D_DEPTH_WRITE_MASK_ALL;
	m_DSDesc.DepthFunc = D3D_COMPARISON_LESS;
	m_DSDesc.StencilEnable = FALSE;
	m_DSDesc.StencilReadMask = 0xFF;
	m_DSDesc.StencilWriteMask = 0xFF;
	m_DSDesc.FrontFace.StencilFailOp = m_DSDesc.FrontFace.StencilDepthFailOp = m_DSDesc.FrontFace.StencilPassOp = D3D_STENCIL_OP_KEEP;
	m_DSDesc.FrontFace.StencilFunc = D3D_COMPARISON_ALWAYS;
	m_DSDesc.BackFace = m_DSDesc.FrontFace;

	ZeroMemory(&m_BDesc, sizeof(m_BDesc));
	for (int i = 0; i < 8; ++i)
	{
		m_BDesc.RenderTarget[i].BlendEnable = FALSE;
		m_BDesc.RenderTarget[i].SrcBlend = D3D_BLEND_ONE;
		m_BDesc.RenderTarget[i].DestBlend = D3D_BLEND_ZERO;
		m_BDesc.RenderTarget[i].BlendOp = D3D_BLEND_OP_ADD;
		m_BDesc.RenderTarget[i].SrcBlendAlpha = D3D_BLEND_ONE;
		m_BDesc.RenderTarget[i].DestBlendAlpha = D3D_BLEND_ZERO;
		m_BDesc.RenderTarget[i].BlendOpAlpha = D3D_BLEND_OP_ADD;
		m_BDesc.RenderTarget[i].RenderTargetWriteMask = D3D_COLOR_WRITE_ENABLE_ALL;
	}
	m_BDesc.AlphaToCoverageEnable = FALSE;
	m_BDesc.IndependentBlendEnable = FALSE;

	m_uiStencilRef = 0;
	m_uiAlphaRef = 0;
	m_uiSampleMask = 0xFFFFFFFF;
}

void dx12StateManager::Apply()
{
	// DX12：状态在 Draw 时由 dx12Context::FlushPipeline 合成 PSO，这里无需提交。
	// 仅清除 need-apply 标记。
	m_bRSNeedApply = m_bDSSNeedApply = m_bBSNeedApply = false;
}

void dx12StateManager::UnmapConstants()
{
	m_cAlphaRef = nullptr;
}

void dx12StateManager::ValidateRDesc()
{
	if (m_bRDInvalid)
	{
		// 若外部未设置，填入默认值
		m_RDesc.FillMode = D3D_FILL_SOLID;
		m_RDesc.CullMode = D3D_CULL_BACK;
		m_RDesc.DepthClipEnable = TRUE;
		m_bRDInvalid = false;
	}
}

void dx12StateManager::ValidateDSDesc()
{
	if (m_bDSDInvalid)
	{
		m_DSDesc.DepthEnable = TRUE;
		m_DSDesc.DepthWriteMask = D3D_DEPTH_WRITE_MASK_ALL;
		m_DSDesc.DepthFunc = D3D_COMPARISON_LESS;
		m_bDSDInvalid = false;
	}
}

void dx12StateManager::ValidateBDesc()
{
	if (m_bBDInvalid)
	{
		for (int i = 0; i < 8; ++i)
		{
			m_BDesc.RenderTarget[i].RenderTargetWriteMask = D3D_COLOR_WRITE_ENABLE_ALL;
		}
		m_bBDInvalid = false;
	}
}

void dx12StateManager::SetRasterizerState(ID3DRasterizerState* pRState)
{
	if (!pRState) return;
	m_RDesc = pRState->desc;
	m_bRDInvalid = false;
	m_bRSChanged = true;
}

void dx12StateManager::SetDepthStencilState(ID3DDepthStencilState* pDSState)
{
	if (!pDSState) return;
	m_DSDesc = pDSState->desc;
	m_uiStencilRef = pDSState->stencilRef;
	m_bDSDInvalid = false;
	m_bDSSChanged = true;
}

void dx12StateManager::SetBlendState(ID3DBlendState* pBlendState)
{
	if (!pBlendState) return;
	m_BDesc = pBlendState->desc;
	m_uiSampleMask = pBlendState->mask;
	m_bBDInvalid = false;
	m_bBSChanged = true;
}

void dx12StateManager::SetStencilRef(UINT uiStencilRef)
{
	if (m_uiStencilRef != uiStencilRef) { m_uiStencilRef = uiStencilRef; m_bDSSChanged = true; }
}

void dx12StateManager::SetAlphaRef(UINT uiAlphaRef)
{
	if (m_uiAlphaRef != uiAlphaRef) { m_uiAlphaRef = uiAlphaRef; }
}

void dx12StateManager::BindAlphaRef(R_constant* C)
{
	m_cAlphaRef = C;
}

// D3D9 与 D3D11 的这些枚举数值一致，直接使用。
void dx12StateManager::SetStencil(u32 Enable, u32 Func, u32 Ref, u32 Mask, u32 WriteMask, u32 Fail, u32 Pass, u32 ZFail)
{
	ValidateDSDesc();
	m_DSDesc.StencilEnable = (BOOL)Enable;
	m_DSDesc.StencilReadMask = (UINT8)Mask;
	m_DSDesc.StencilWriteMask = (UINT8)WriteMask;
	m_DSDesc.FrontFace.StencilFailOp = (D3D_STENCIL_OP)Fail;
	m_DSDesc.FrontFace.StencilDepthFailOp = (D3D_STENCIL_OP)ZFail;
	m_DSDesc.FrontFace.StencilPassOp = (D3D_STENCIL_OP)Pass;
	m_DSDesc.FrontFace.StencilFunc = (D3D_COMPARISON_FUNC)Func;
	m_DSDesc.BackFace = m_DSDesc.FrontFace;
	m_uiStencilRef = Ref;
	m_bDSSChanged = true;
}

void dx12StateManager::SetDepthFunc(u32 Func)
{
	ValidateDSDesc();
	m_DSDesc.DepthFunc = (D3D_COMPARISON_FUNC)Func;
	m_bDSSChanged = true;
}

void dx12StateManager::SetDepthEnable(u32 Enable)
{
	ValidateDSDesc();
	m_DSDesc.DepthEnable = (BOOL)Enable;
	m_bDSSChanged = true;
}

void dx12StateManager::SetColorWriteEnable(u32 WriteMask)
{
	ValidateBDesc();
	for (int i = 0; i < 8; ++i)
		m_BDesc.RenderTarget[i].RenderTargetWriteMask = (UINT8)WriteMask;
	m_bBSChanged = true;
}

void dx12StateManager::SetCullMode(u32 Mode)
{
	ValidateRDesc();
	// D3D9: NONE=1/CW=2/CCW=3  ↔  D3D11: NONE=1/FRONT=2/BACK=3
	m_RDesc.CullMode = (D3D_CULL_MODE)Mode;
	m_bRSChanged = true;
}

void dx12StateManager::SetMultisample(u32 Enable)
{
	ValidateRDesc();
	m_RDesc.MultisampleEnable = (BOOL)Enable;
	m_bRSChanged = true;
}

void dx12StateManager::SetSampleMask(u32 Mask)
{
	m_uiSampleMask = Mask;
}

void dx12StateManager::EnableScissoring(BOOL bEnable)
{
	if (m_bOverrideScissoring)
		bEnable = m_bOverrideScissoringValue;
	ValidateRDesc();
	m_RDesc.ScissorEnable = bEnable;
	m_bRSChanged = true;
}

void dx12StateManager::OverrideScissoring(bool bOverride, BOOL bValue)
{
	m_bOverrideScissoring = bOverride;
	m_bOverrideScissoringValue = bValue;
}
