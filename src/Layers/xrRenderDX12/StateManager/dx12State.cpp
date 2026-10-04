#include "stdafx.h"
#include "dx12State.h"
#include "dx12StateManager.h"
#include "dx12SamplerStateCache.h"
#include "../../xrRender/tss_def.h"

dx12State::dx12State()
{
	memset(&m_RDesc, 0, sizeof(m_RDesc));
	memset(&m_DSDesc, 0, sizeof(m_DSDesc));
	memset(&m_BDesc, 0, sizeof(m_BDesc));
}

dx12State::~dx12State()
{
}

void dx12State::Release()
{
	// DX12：无独立 D3D 状态对象可释放
}

HRESULT dx12State::Apply()
{
	StateManager.m_RDesc = m_RDesc;
	StateManager.m_DSDesc = m_DSDesc;
	StateManager.m_BDesc = m_BDesc;
	StateManager.m_uiStencilRef = m_uiStencilRef;
	StateManager.m_uiAlphaRef = m_uiAlphaRef;
	StateManager.m_uiSampleMask = m_uiSampleMask;
	return S_OK;
}

// 把 SimulatorStates 的 DX9 风格状态翻译为 D3D11 描述（阶段 1 起逐步完善）。
dx12State* dx12State::Create(SimulatorStates& state_code)
{
	dx12State* pState = new dx12State();

	// 默认值
	pState->m_RDesc.FillMode = D3D_FILL_SOLID;
	pState->m_RDesc.CullMode = D3D_CULL_BACK;
	pState->m_RDesc.DepthClipEnable = TRUE;

	pState->m_DSDesc.DepthEnable = TRUE;
	pState->m_DSDesc.DepthWriteMask = D3D_DEPTH_WRITE_MASK_ALL;
	pState->m_DSDesc.DepthFunc = D3D_COMPARISON_LESS;
	pState->m_DSDesc.StencilEnable = FALSE;
	pState->m_DSDesc.StencilReadMask = 0xFF;
	pState->m_DSDesc.StencilWriteMask = 0xFF;
	pState->m_DSDesc.FrontFace.StencilFailOp = pState->m_DSDesc.FrontFace.StencilDepthFailOp = pState->m_DSDesc.FrontFace.StencilPassOp = D3D_STENCIL_OP_KEEP;
	pState->m_DSDesc.FrontFace.StencilFunc = D3D_COMPARISON_ALWAYS;
	pState->m_DSDesc.BackFace = pState->m_DSDesc.FrontFace;

	for (int i = 0; i < 8; ++i)
	{
		pState->m_BDesc.RenderTarget[i].BlendEnable = FALSE;
		pState->m_BDesc.RenderTarget[i].SrcBlend = D3D_BLEND_ONE;
		pState->m_BDesc.RenderTarget[i].DestBlend = D3D_BLEND_ZERO;
		pState->m_BDesc.RenderTarget[i].BlendOp = D3D_BLEND_OP_ADD;
		pState->m_BDesc.RenderTarget[i].SrcBlendAlpha = D3D_BLEND_ONE;
		pState->m_BDesc.RenderTarget[i].DestBlendAlpha = D3D_BLEND_ZERO;
		pState->m_BDesc.RenderTarget[i].BlendOpAlpha = D3D_BLEND_OP_ADD;
		pState->m_BDesc.RenderTarget[i].RenderTargetWriteMask = D3D_COLOR_WRITE_ENABLE_ALL;
	}
	pState->m_BDesc.AlphaToCoverageEnable = FALSE;
	pState->m_BDesc.IndependentBlendEnable = FALSE;

	// 解析 state_code 中烘焙的 D3DRS_* 状态（对齐 DX11 dx10StateCache 路径：
	// 先设 D3D11 默认值，再用 SimulatorStates 覆盖混合/深度/光栅描述）。
	// 之前完全忽略 state_code 导致 BlendEnable 恒为 FALSE（ONE/ZERO），
	// UI/字体的 alpha 混合全部失效 → 字体四边形渲染为实心白块。
	state_code.UpdateDesc(pState->m_RDesc);
	state_code.UpdateDesc(pState->m_DSDesc);
	state_code.UpdateDesc(pState->m_BDesc);

	return pState;
}
