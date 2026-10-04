#ifndef	dx12R_Backend_Runtime_included
#define	dx12R_Backend_Runtime_included
#pragma once

#include "StateManager/dx12StateManager.h"
#include "StateManager/dx12ShaderResourceStateCache.h"
#include "StateManager/dx12SamplerStateCache.h"
#include "dx12Types.h"
#include "dx12BufferUtils.h"

//==============================================================================
// dx12R_Backend_Runtime.h  (对应 xrRenderDX10/dx10R_Backend_Runtime.h)
//
// CBackend 的 D3D12 inline 实现。状态调用被记录（PSO 在 Draw 时合成），
// 绑定/绘制转发到 dx12Context。
//==============================================================================

IC void CBackend::set_xform(u32 ID, const Fmatrix& M_)
{
	stat.xforms++;
}

IC void CBackend::set_RT(ID3DRenderTargetView* RT, u32 ID)
{
	if (RT != pRT[ID])
	{
		stat.target_rt++;
		pRT[ID] = RT;
		m_bChangedRTorZB = true;
	}
}

IC void	CBackend::set_ZB(ID3DDepthStencilView* ZB)
{
	if (ZB != pZB)
	{
		stat.target_zb++;
		pZB = ZB;
		// 允许 RT 作为输入：清空 RT
		if (!m_bChangedRTorZB)
			DX12Context.OMSetRenderTargets(0, nullptr, nullptr);
		m_bChangedRTorZB = true;
	}
}

ICF void CBackend::set_Format(SDeclaration* _decl)
{
	if (decl != _decl)
	{
#ifdef DEBUG
		stat.decl++;
#endif
		decl = _decl;
	}
}

ICF void CBackend::set_PS(ID3DPixelShader* _ps, LPCSTR _n)
{
	if (ps != _ps)
	{
		stat.ps++;
		ps = _ps;
		DX12Context.PSSetShader(ps, 0, 0);
#ifdef DEBUG
		ps_name = _n;
#endif
	}
}

ICF void CBackend::set_GS(ID3DGeometryShader* _gs, LPCSTR _n)
{
	if (gs != _gs)
	{
		gs = _gs;
		DX12Context.GSSetShader(gs, 0, 0);
#ifdef DEBUG
		gs_name = _n;
#endif
	}
}

ICF void CBackend::set_HS(ID3DHullShader* _hs, LPCSTR _n)
{
	if (hs != _hs)
	{
		hs = _hs;
		DX12Context.HSSetShader(hs, 0, 0);
#ifdef DEBUG
		hs_name = _n;
#endif
	}
}

ICF void CBackend::set_DS(ID3DDomainShader* _ds, LPCSTR _n)
{
	if (ds != _ds)
	{
		ds = _ds;
		DX12Context.DSSetShader(ds, 0, 0);
#ifdef DEBUG
		ds_name = _n;
#endif
	}
}

ICF void CBackend::set_CS(ID3DComputeShader* _cs, LPCSTR _n)
{
	if (cs != _cs)
	{
		cs = _cs;
		DX12Context.CSSetShader(cs, 0, 0);
#ifdef DEBUG
		cs_name = _n;
#endif
	}
}

ICF	bool CBackend::is_TessEnabled()
{
	return true;
}

ICF void CBackend::set_VS(ID3DVertexShader* _vs, LPCSTR _n)
{
	if (vs != _vs)
	{
		stat.vs++;
		vs = _vs;
		DX12Context.VSSetShader(vs, 0, 0);
#ifdef DEBUG
		vs_name = _n;
#endif
	}
}

ICF void CBackend::set_Vertices(ID3DVertexBuffer* _vb, u32 _vb_stride)
{
	if ((vb != _vb) || (vb_stride != _vb_stride))
	{
#ifdef DEBUG
		stat.vb++;
#endif
		vb = _vb;
		vb_stride = _vb_stride;
		DX12Context.IASetVertexBuffers(0, 1, &vb, &_vb_stride, nullptr);
	}
}

ICF void CBackend::set_Indices(ID3DIndexBuffer* _ib)
{
	if (ib != _ib)
	{
#ifdef DEBUG
		stat.ib++;
#endif
		ib = _ib;
		DX12Context.IASetIndexBuffer(ib, DXGI_FORMAT_R16_UINT, 0);
	}
}

IC D3D_PRIMITIVE_TOPOLOGY TranslateTopology(D3DPRIMITIVETYPE T)
{
	static	D3D_PRIMITIVE_TOPOLOGY translateTable[] =
	{
		D3D_PRIMITIVE_TOPOLOGY_UNDEFINED,
		D3D_PRIMITIVE_TOPOLOGY_POINTLIST,
		D3D_PRIMITIVE_TOPOLOGY_LINELIST,
		D3D_PRIMITIVE_TOPOLOGY_LINESTRIP,
		D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST,
		D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP,
		D3D_PRIMITIVE_TOPOLOGY_UNDEFINED,
	};
	VERIFY(T < sizeof(translateTable) / sizeof(translateTable[0]));
	VERIFY(T >= 0);
	D3D_PRIMITIVE_TOPOLOGY	result = translateTable[T];
	VERIFY(result != D3D_PRIMITIVE_TOPOLOGY_UNDEFINED);
	return result;
}

IC u32 GetIndexCount(D3DPRIMITIVETYPE T, u32 iPrimitiveCount)
{
	switch (T)
	{
	case D3DPT_POINTLIST:		return iPrimitiveCount;
	case D3DPT_LINELIST:		return iPrimitiveCount * 2;
	case D3DPT_LINESTRIP:		return iPrimitiveCount + 1;
	case D3DPT_TRIANGLELIST:	return iPrimitiveCount * 3;
	case D3DPT_TRIANGLESTRIP:	return iPrimitiveCount + 2;
	default: NODEFAULT;
#ifdef DEBUG
		return 0;
#endif
	}
}

IC void CBackend::ApplyPrimitieTopology(D3D_PRIMITIVE_TOPOLOGY Topology)
{
	if (m_PrimitiveTopology != Topology)
	{
		m_PrimitiveTopology = Topology;
		DX12Context.IASetPrimitiveTopology(Topology);
	}
}

IC void CBackend::Compute(UINT x, UINT y, UINT z)
{
	stat.calls++;
	SRVSManager.Apply();
	StateManager.Apply();
	constants.flush();
	DX12Context.Dispatch(x, y, z);
}

IC void CBackend::RenderInstancedIndexed(D3DPRIMITIVETYPE T_, u32 baseV, u32 startV, u32 countV, u32 startI, u32 PC, u32 instanceCount, u32 startInstanceLocation)
{
	D3D_PRIMITIVE_TOPOLOGY Topology = TranslateTopology(T_);
	u32	iIndexCount = GetIndexCount(T_, PC);
	if (hs != 0 || ds != 0)
	{
		R_ASSERT(Topology == D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		Topology = D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST;
	}
	stat.calls++;
	stat.verts += countV;
	stat.polys += PC;

	ApplyPrimitieTopology(Topology);
	SRVSManager.Apply();
	ApplyRTandZB();
	ApplyVertexLayout();
	StateManager.Apply();
	constants.flush();

	DX12Context.DrawIndexedInstanced(iIndexCount, instanceCount, startI, baseV, startInstanceLocation);
}

IC void CBackend::Render(D3DPRIMITIVETYPE T_, u32 baseV, u32 startV, u32 countV, u32 startI, u32 PC)
{
	D3D_PRIMITIVE_TOPOLOGY Topology = TranslateTopology(T_);
	u32	iIndexCount = GetIndexCount(T_, PC);
	if (hs != 0 || ds != 0)
	{
		R_ASSERT(Topology == D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		Topology = D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST;
	}
	stat.calls++;
	stat.verts += countV;
	stat.polys += PC;

	ApplyPrimitieTopology(Topology);
	SRVSManager.Apply();
	ApplyRTandZB();
	ApplyVertexLayout();
	StateManager.Apply();
	constants.flush();

	DX12Context.DrawIndexed(iIndexCount, startI, baseV);
}

IC void CBackend::Render(D3DPRIMITIVETYPE T_, u32 startV, u32 PC)
{
	if (T_ == D3DPT_TRIANGLEFAN)
		return;

	D3D_PRIMITIVE_TOPOLOGY Topology = TranslateTopology(T_);
	u32	iVertexCount = GetIndexCount(T_, PC);

	stat.calls++;
	stat.verts += 3 * PC;
	stat.polys += PC;

	ApplyPrimitieTopology(Topology);
	SRVSManager.Apply();
	ApplyRTandZB();
	ApplyVertexLayout();
	StateManager.Apply();
	constants.flush();

	DX12Context.Draw(iVertexCount, startV);
}

IC void CBackend::Render_noIA(u32 iVertexCount)
{
	stat.calls++;
	stat.verts += iVertexCount;

	SRVSManager.Apply();
	ApplyRTandZB();

	DX12Context.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	DX12Context.IASetInputLayout(nullptr);

	StateManager.Apply();
	constants.flush();

	DX12Context.Draw(iVertexCount, 0);
}

IC void CBackend::set_Geometry(SGeometry* _geom)
{
	set_Format(&*_geom->dcl);
	set_Vertices(_geom->vb, _geom->vb_stride);
	set_Indices(_geom->ib);
}

IC void	CBackend::set_Scissor(Irect* R)
{
	if (R)
	{
		StateManager.EnableScissoring();
		RECT* clip = (RECT*)R;
		DX12Context.RSSetScissorRects(1, clip);
	}
	else
	{
		StateManager.EnableScissoring(FALSE);
		// D3D12 的 scissor test 无法关闭（与 D3D11 的 ScissorEnable=FALSE 不同）：
		// 绑定 0 个矩形会使当前 scissor 退化为空矩形，Draw 的所有像素都被裁掉。
		// 用覆盖最大范围的矩形（D3D12 scissor 最大边长 16384）实现"不裁剪"语义；
		// 超出 RT 的部分 GPU 会按渲染目标边界自行裁剪。
		static const RECT fullClip = { 0, 0, 16384, 16384 };
		DX12Context.RSSetScissorRects(1, &fullClip);
	}
}

IC void CBackend::set_Stencil(u32 _enable, u32 _func, u32 _ref, u32 _mask, u32 _writemask, u32 _fail, u32 _pass, u32 _zfail)
{
	StateManager.SetStencil(_enable, _func, _ref, _mask, _writemask, _fail, _pass, _zfail);
}

IC  void CBackend::set_Z(u32 _enable) { StateManager.SetDepthEnable(_enable); }
IC  void CBackend::set_ZFunc(u32 _func) { StateManager.SetDepthFunc(_func); }
IC  void CBackend::set_AlphaRef(u32 _value) { StateManager.SetAlphaRef(_value); }
IC void	CBackend::set_ColorWriteEnable(u32 _mask) { StateManager.SetColorWriteEnable(_mask); }
ICF void CBackend::set_CullMode(u32 _mode) { StateManager.SetCullMode(_mode); }

IC void CBackend::ApplyVertexLayout()
{
	VERIFY(decl);

	xr_map<ID3DBlob*, ID3DInputLayout*>::iterator it = decl->vs_to_layout.find(m_pInputSignature);
	if (it == decl->vs_to_layout.end())
	{
		dx12InputLayout* pLayout = new dx12InputLayout();
		// dx10_dcl_code 末尾带有一个全零的 D3DDECL_END 哨兵（见 ConvertVertexDeclaration），
		// DX11 用 size()-1 创建布局；DX12 也必须排除该哨兵，否则末元素 SemanticName=NULL 导致建布局失败。
		const size_t elemCount = decl->dx10_dcl_code.empty()
			? 0 : decl->dx10_dcl_code.size() - 1;
		pLayout->elements.resize(elemCount);
		for (size_t i = 0; i < elemCount; ++i)
		{
			const D3D_INPUT_ELEMENT_DESC& s = decl->dx10_dcl_code[i];
			D3D12_INPUT_ELEMENT_DESC& d = pLayout->elements[i];
			d.SemanticName = s.SemanticName;
			d.SemanticIndex = s.SemanticIndex;
			d.Format = s.Format;
			d.InputSlot = s.InputSlot;
			d.AlignedByteOffset = s.AlignedByteOffset;
			d.InputSlotClass = (D3D12_INPUT_CLASSIFICATION)s.InputSlotClass;
			d.InstanceDataStepRate = s.InstanceDataStepRate;
		}
		it = decl->vs_to_layout.insert(std::pair<ID3DBlob*, ID3DInputLayout*>(m_pInputSignature, pLayout)).first;
	}

	if (m_pInputLayout != it->second)
	{
		m_pInputLayout = it->second;
		DX12Context.IASetInputLayout(m_pInputLayout);
	}
}

ICF void CBackend::set_VS(ref_vs& _vs)
{
	m_pInputSignature = _vs->signature->signature;
	set_VS(_vs->vs, _vs->cName.c_str());
}

ICF void CBackend::set_VS(SVS* _vs)
{
	m_pInputSignature = _vs->signature->signature;
	set_VS(_vs->vs, _vs->cName.c_str());
}

IC bool CBackend::CBuffersNeedUpdate(ref_cbuffer buf1[MaxCBuffers], ref_cbuffer buf2[MaxCBuffers], u32& uiMin, u32& uiMax)
{
	bool	bRes = false;
	int i = 0;
	while ((i < MaxCBuffers) && (buf1[i] == buf2[i]))
		++i;
	uiMin = i;
	for (; i < MaxCBuffers; ++i)
	{
		if (buf1[i] != buf2[i]) { bRes = true; uiMax = i; }
	}
	return bRes;
}

IC void CBackend::set_Constants(R_constant_table* C_)
{
	if (ctable == C_)	return;
	ctable = C_;
	xforms.unmap();
	hemi.unmap();
	tree.unmap();
	LOD.unmap();
	StateManager.UnmapConstants();
	if (0 == C_)		return;

	{
		ref_cbuffer	aPixelConstants[MaxCBuffers];
		ref_cbuffer	aVertexConstants[MaxCBuffers];
		ref_cbuffer	aGeometryConstants[MaxCBuffers];
		ref_cbuffer	aHullConstants[MaxCBuffers];
		ref_cbuffer	aDomainConstants[MaxCBuffers];
		ref_cbuffer	aComputeConstants[MaxCBuffers];

		for (int i = 0; i < MaxCBuffers; ++i)
		{
			aPixelConstants[i] = m_aPixelConstants[i];
			aVertexConstants[i] = m_aVertexConstants[i];
			aGeometryConstants[i] = m_aGeometryConstants[i];
			aHullConstants[i] = m_aHullConstants[i];
			aDomainConstants[i] = m_aDomainConstants[i];
			aComputeConstants[i] = m_aComputeConstants[i];

			m_aPixelConstants[i] = 0;
			m_aVertexConstants[i] = 0;
			m_aGeometryConstants[i] = 0;
			m_aHullConstants[i] = 0;
			m_aDomainConstants[i] = 0;
			m_aComputeConstants[i] = 0;
		}

		R_constant_table::cb_table::iterator	it = C_->m_CBTable.begin();
		R_constant_table::cb_table::iterator	end = C_->m_CBTable.end();
		for (; it != end; ++it)
		{
			u32 uiBufferIndex = it->first;
			if ((uiBufferIndex&CB_BufferTypeMask) == CB_BufferPixelShader)
			{
				VERIFY((uiBufferIndex&CB_BufferIndexMask) < MaxCBuffers);
				m_aPixelConstants[uiBufferIndex&CB_BufferIndexMask] = it->second;
			}
			else if ((uiBufferIndex&CB_BufferTypeMask) == CB_BufferVertexShader)
			{
				VERIFY((uiBufferIndex&CB_BufferIndexMask) < MaxCBuffers);
				m_aVertexConstants[uiBufferIndex&CB_BufferIndexMask] = it->second;
			}
			else if ((uiBufferIndex&CB_BufferTypeMask) == CB_BufferGeometryShader)
			{
				VERIFY((uiBufferIndex&CB_BufferIndexMask) < MaxCBuffers);
				m_aGeometryConstants[uiBufferIndex&CB_BufferIndexMask] = it->second;
			}
			else if ((uiBufferIndex&CB_BufferTypeMask) == CB_BufferHullShader)
			{
				VERIFY((uiBufferIndex&CB_BufferIndexMask) < MaxCBuffers);
				m_aHullConstants[uiBufferIndex&CB_BufferIndexMask] = it->second;
			}
			else if ((uiBufferIndex&CB_BufferTypeMask) == CB_BufferDomainShader)
			{
				VERIFY((uiBufferIndex&CB_BufferIndexMask) < MaxCBuffers);
				m_aDomainConstants[uiBufferIndex&CB_BufferIndexMask] = it->second;
			}
			else if ((uiBufferIndex&CB_BufferTypeMask) == CB_BufferComputeShader)
			{
				VERIFY((uiBufferIndex&CB_BufferIndexMask) < MaxCBuffers);
				m_aComputeConstants[uiBufferIndex&CB_BufferIndexMask] = it->second;
			}
			else
				VERIFY("Invalid enumeration");
		}

		ID3DBuffer*	tempBuffer[MaxCBuffers];
		u32 uiMin, uiMax;

		if (CBuffersNeedUpdate(m_aPixelConstants, aPixelConstants, uiMin, uiMax))
		{
			++uiMax;
			for (u32 i = uiMin; i < uiMax; ++i)
				tempBuffer[i] = m_aPixelConstants[i] ? m_aPixelConstants[i]->GetBuffer() : 0;
			DX12Context.PSSetConstantBuffers(uiMin, uiMax - uiMin, &tempBuffer[uiMin]);
		}
		if (CBuffersNeedUpdate(m_aVertexConstants, aVertexConstants, uiMin, uiMax))
		{
			++uiMax;
			for (u32 i = uiMin; i < uiMax; ++i)
				tempBuffer[i] = m_aVertexConstants[i] ? m_aVertexConstants[i]->GetBuffer() : 0;
			DX12Context.VSSetConstantBuffers(uiMin, uiMax - uiMin, &tempBuffer[uiMin]);
		}
		if (CBuffersNeedUpdate(m_aGeometryConstants, aGeometryConstants, uiMin, uiMax))
		{
			++uiMax;
			for (u32 i = uiMin; i < uiMax; ++i)
				tempBuffer[i] = m_aGeometryConstants[i] ? m_aGeometryConstants[i]->GetBuffer() : 0;
			DX12Context.GSSetConstantBuffers(uiMin, uiMax - uiMin, &tempBuffer[uiMin]);
		}
		if (CBuffersNeedUpdate(m_aHullConstants, aHullConstants, uiMin, uiMax))
		{
			++uiMax;
			for (u32 i = uiMin; i < uiMax; ++i)
				tempBuffer[i] = m_aHullConstants[i] ? m_aHullConstants[i]->GetBuffer() : 0;
			DX12Context.HSSetConstantBuffers(uiMin, uiMax - uiMin, &tempBuffer[uiMin]);
		}
		if (CBuffersNeedUpdate(m_aDomainConstants, aDomainConstants, uiMin, uiMax))
		{
			++uiMax;
			for (u32 i = uiMin; i < uiMax; ++i)
				tempBuffer[i] = m_aDomainConstants[i] ? m_aDomainConstants[i]->GetBuffer() : 0;
			DX12Context.DSSetConstantBuffers(uiMin, uiMax - uiMin, &tempBuffer[uiMin]);
		}
		if (CBuffersNeedUpdate(m_aComputeConstants, aComputeConstants, uiMin, uiMax))
		{
			++uiMax;
			for (u32 i = uiMin; i < uiMax; ++i)
				tempBuffer[i] = m_aComputeConstants[i] ? m_aComputeConstants[i]->GetBuffer() : 0;
			DX12Context.CSSetConstantBuffers(uiMin, uiMax - uiMin, &tempBuffer[uiMin]);
		}
	}

	R_constant_table::c_table::iterator	it = C_->table.begin();
	R_constant_table::c_table::iterator	end = C_->table.end();
	for (; it != end; it++)
	{
		R_constant* Cs = &**it;
		VERIFY(Cs);
		if (Cs && Cs->handler)
			Cs->handler->setup(Cs);
	}
}

ICF void CBackend::ApplyRTandZB()
{
	if (m_bChangedRTorZB)
	{
		m_bChangedRTorZB = false;
		DX12Context.OMSetRenderTargets(sizeof(pRT) / sizeof(pRT[0]), pRT, pZB);
	}
}

IC	void CBackend::get_ConstantDirect(shared_str& n, u32 DataSize, void** pVData, void** pGData, void** pPData)
{
	ref_constant C_ = get_c(n);
	if (C_)
		constants.access_direct(&*C_, DataSize, pVData, pGData, pPData);
	else
	{
		if (pVData)	*pVData = 0;
		if (pGData)	*pGData = 0;
		if (pPData)	*pPData = 0;
	}
}

IC float CBackend::get_width() { return RDEVICE.HalfTargetWidth; }
IC float CBackend::get_height() { return RDEVICE.HalfTargetHeight; }
IC float CBackend::get_target_width() { return float(RDEVICE.TargetWidth); }
IC float CBackend::get_target_height() { return float(RDEVICE.TargetHeight); }

#endif	//	dx12R_Backend_Runtime_included
