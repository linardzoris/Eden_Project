#pragma once

//==============================================================================
// dx12Types.h
//
// 把 D3D11 形态的接口（ID3DDevice/ID3DVertexBuffer/ID3DVertexShader ...）实现为
// D3D12 wrapper。共享层与 DX10 风格分支调用这些 API 时，wrapper 内部翻译为
// D3D12（资源创建、描述符、PSO 合成、上传环）。
//==============================================================================

#include "DX12CommonTypes.h"
#include "dx12Backend.h"
#include "StateManager/dx12State.h"

#include <wrl/client.h>
using Microsoft::WRL::ComPtr;

//------------------------------------------------------------------------------
// 引用计数基类（兼容共享层的 _RELEASE 宏）
//------------------------------------------------------------------------------
class dx12RefCounted
{
public:
	virtual ~dx12RefCounted() {}

	ULONG	AddRef()
	{
		return (ULONG)InterlockedIncrement(&m_ref);
	}
	ULONG	Release()
	{
		LONG r = InterlockedDecrement(&m_ref);
		if (r == 0) { delete this; return 0; }
		return (ULONG)r;
	}
private:
	volatile LONG	m_ref = 1;
};

//------------------------------------------------------------------------------
// dx12Shader：持有 DXBC 字节码（阶段 1–2 用 D3DCompile 产物）
//------------------------------------------------------------------------------
class dx12Shader : public dx12RefCounted
{
public:
	enum Stage { stVS, stPS, stGS, stHS, stDS, stCS };

	ComPtr<ID3DBlob>	blob;
	Stage				stage = stVS;

	D3D12_SHADER_BYTECODE Bytecode() const
	{
		D3D12_SHADER_BYTECODE bc = {};
		if (blob) { bc.pShaderBytecode = blob->GetBufferPointer(); bc.BytecodeLength = blob->GetBufferSize(); }
		return bc;
	}
};

//------------------------------------------------------------------------------
// dx12InputLayout：D3D12 输入布局（由 SDeclaration 的 D3D_INPUT_ELEMENT_DESC 转换）
//------------------------------------------------------------------------------
class dx12InputLayout : public dx12RefCounted
{
public:
	xr_vector<D3D12_INPUT_ELEMENT_DESC>	elements;

	D3D12_INPUT_LAYOUT_DESC Desc() const
	{
		D3D12_INPUT_LAYOUT_DESC d = {};
		if (!elements.empty())
		{
			d.pInputElementDescs = elements.data();
			d.NumElements = (UINT)elements.size();
		}
		return d;
	}
};

//------------------------------------------------------------------------------
// 资源基类：dx12Buffer / dx12Texture 的共同基类，对应 ID3DResource
//------------------------------------------------------------------------------
class dx12ResourceBase : public dx12RefCounted
{
public:
	ComPtr<ID3D12Resource>	resource;
	virtual ~dx12ResourceBase() {}

	ID3D12Resource*	GetResource() { return resource.Get(); }
};

//------------------------------------------------------------------------------
// dx12Buffer：D3D12 buffer（VB/IB/CB）+ GPU 虚拟地址 + CPU 映射指针
//------------------------------------------------------------------------------
class dx12Buffer : public dx12ResourceBase
{
public:
	D3D12_GPU_VIRTUAL_ADDRESS	gpuVA = 0;
	UINT					size = 0;
	UINT					stride = 0;
	bool					isIndex = false;
	bool					isConstant = false;
	bool					immutable = false;
	void*					mapped = nullptr;	// 上传堆/持久映射指针

	ID3D12Resource*	GetResource() { return resource.Get(); }
	void	GetDesc(D3D_BUFFER_DESC* d) const
	{
		if (!d) return;
		ZeroMemory(d, sizeof(*d));
		d->ByteWidth = size;
		d->Usage = D3D_USAGE_DEFAULT;
		d->BindFlags = isIndex ? D3D_BIND_INDEX_BUFFER : (isConstant ? D3D_BIND_CONSTANT_BUFFER : D3D_BIND_VERTEX_BUFFER);
	}
};

//------------------------------------------------------------------------------
// dx12Texture：D3D12 texture
//------------------------------------------------------------------------------
class dx12Texture : public dx12ResourceBase
{
public:
	D3D_TEXTURE2D_DESC		desc = {};
	ComPtr<ID3D12Resource>	staging;		// 上传用
	D3D12_RESOURCE_STATES	state = D3D12_RESOURCE_STATE_COMMON;

	UINT	Width() const { return desc.Width; }
	UINT	Height() const { return desc.Height; }
	DXGI_FORMAT	Format() const { return desc.Format; }
	ID3D12Resource* GetResource() { return resource.Get(); }
	void	GetDesc(D3D_TEXTURE2D_DESC* d) const { if (d) *d = desc; }
};

//------------------------------------------------------------------------------
// dx12ShaderResourceView
//------------------------------------------------------------------------------
class dx12ShaderResourceView : public dx12RefCounted
{
public:
	ComPtr<ID3D12Resource>				resource;
	D3D12_SHADER_RESOURCE_VIEW_DESC		desc = {};
	D3D12_CPU_DESCRIPTOR_HANDLE			cpu = {};
	bool								valid = false;
};

//------------------------------------------------------------------------------
// dx12RenderTargetView / dx12DepthStencilView
//------------------------------------------------------------------------------
class dx12RenderTargetView : public dx12RefCounted
{
public:
	ComPtr<ID3D12Resource>		resource;
	D3D12_CPU_DESCRIPTOR_HANDLE	cpu = {};
	bool						valid = false;
};

class dx12DepthStencilView : public dx12RefCounted
{
public:
	ComPtr<ID3D12Resource>		resource;
	D3D12_CPU_DESCRIPTOR_HANDLE	cpu = {};
	bool						valid = false;

	// 供共享层查询深度缓冲尺寸（r4 u_setrt）
	void	GetDesc(D3D_DEPTH_STENCIL_VIEW_DESC* d) const
	{
		if (!d) return;
		ZeroMemory(d, sizeof(*d));
		d->Format = resource ? resource->GetDesc().Format : DXGI_FORMAT_UNKNOWN;
		d->ViewDimension = D3D_DSV_DIMENSION_TEXTURE2D;
	}
	void	GetResource(ID3DResource** pp)
	{
		dx12Texture* t = new dx12Texture();
		t->resource = resource;
		if (resource)
		{
			D3D12_RESOURCE_DESC rd = resource->GetDesc();
			t->desc.Width = (UINT)rd.Width;
			t->desc.Height = rd.Height;
			t->desc.Format = rd.Format;
			t->desc.MipLevels = rd.MipLevels;
			t->desc.ArraySize = rd.DepthOrArraySize;
		}
		*pp = t;
	}
};

class dx12UnorderedAccessView : public dx12RefCounted
{
public:
	ComPtr<ID3D12Resource>			resource;
	D3D12_UNORDERED_ACCESS_VIEW_DESC	desc = {};
	D3D12_CPU_DESCRIPTOR_HANDLE		cpu = {};
	bool							valid = false;
};

//------------------------------------------------------------------------------
// dx12Query
//------------------------------------------------------------------------------
class dx12Query : public dx12RefCounted
{
public:
	ComPtr<ID3D12Resource>		resource;
	D3D_QUERY_DESC				desc = {};
	ComPtr<ID3D12Fence>			fence;
	UINT64						fenceValue = 0;
};

//------------------------------------------------------------------------------
// 状态对象：D3D12 无状态对象，wrapper 只保存描述 + hash，供 PSO 合成
//------------------------------------------------------------------------------
class dx12SamplerState : public dx12RefCounted
{
public:
	D3D_SAMPLER_DESC	desc = {};
	D3D12_CPU_DESCRIPTOR_HANDLE	cpuPersistent = {};
	bool				hasPersistent = false;

	UINT64	Hash() const;
};

class dx12RasterizerState : public dx12RefCounted
{
public:
	D3D_RASTERIZER_DESC	desc = {};
	UINT64	Hash() const;
};

class dx12DepthStencilState : public dx12RefCounted
{
public:
	D3D_DEPTH_STENCIL_DESC	desc = {};
	UINT		stencilRef = 0;
	UINT64	Hash() const;
};

class dx12BlendState : public dx12RefCounted
{
public:
	D3D_BLEND_DESC	desc = {};
	FLOAT			factor[4] = { 1,1,1,1 };
	UINT			mask = 0xFFFFFFFF;
	UINT64	Hash() const;
};

//------------------------------------------------------------------------------
// dx12Device：资源创建（D3D11 形态 API → D3D12）
//------------------------------------------------------------------------------
class dx12Device
{
public:
	ID3D12Device*	Get() const { return dx12::GetD3D12Device(); }

	HRESULT	CreateBuffer(const D3D_BUFFER_DESC* pDesc, const D3D_SUBRESOURCE_DATA* pInitialData, ID3DBuffer** ppBuffer);
	HRESULT	CreateTexture2D(const D3D_TEXTURE2D_DESC* pDesc, const D3D_SUBRESOURCE_DATA* pInitialData, ID3DTexture2D** ppTexture);
	HRESULT	CreateTexture3D(const D3D_TEXTURE3D_DESC* pDesc, const D3D_SUBRESOURCE_DATA* pInitialData, ID3DTexture3D** ppTexture);
	HRESULT	CreateRenderTargetView(ID3DResource* pResource, const D3D_RENDER_TARGET_VIEW_DESC* pDesc, ID3DRenderTargetView** ppRTView);
	HRESULT	CreateDepthStencilView(ID3DResource* pResource, const D3D_DEPTH_STENCIL_VIEW_DESC* pDesc, ID3DDepthStencilView** ppDSView);
	HRESULT	CreateShaderResourceView(ID3DResource* pResource, const D3D_SHADER_RESOURCE_VIEW_DESC* pDesc, ID3DShaderResourceView** ppSRView);
	HRESULT	CreateUnorderedAccessView(ID3DResource* pResource, const D3D11_UNORDERED_ACCESS_VIEW_DESC* pDesc, ID3DUnorderedAccessView** ppUAView);
	HRESULT	CreateSamplerState(const D3D_SAMPLER_DESC* pDesc, ID3DSamplerState** ppSamplerState);
	HRESULT	CreateRasterizerState(const D3D_RASTERIZER_DESC* pDesc, ID3DRasterizerState** ppRasterizerState);
	HRESULT	CreateDepthStencilState(const D3D_DEPTH_STENCIL_DESC* pDesc, UINT StencilRef, ID3DDepthStencilState** ppDepthStencilState);
	HRESULT	CreateBlendState(const D3D_BLEND_DESC* pDesc, ID3DBlendState** ppBlendState);

	HRESULT	CreateVertexShader(const void* pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage* pClassLinkage, ID3DVertexShader** ppVertexShader);
	HRESULT	CreatePixelShader(const void* pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage* pClassLinkage, ID3DPixelShader** ppPixelShader);
	HRESULT	CreateGeometryShader(const void* pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage* pClassLinkage, ID3DGeometryShader** ppGeometryShader);
	HRESULT	CreateHullShader(const void* pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage* pClassLinkage, ID3DHullShader** ppHullShader);
	HRESULT	CreateDomainShader(const void* pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage* pClassLinkage, ID3DDomainShader** ppDomainShader);
	HRESULT	CreateComputeShader(const void* pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage* pClassLinkage, ID3DComputeShader** ppComputeShader);

	HRESULT	CreateInputLayout(const D3D_INPUT_ELEMENT_DESC* pInputElementDescs, UINT NumElements,
		const void* pShaderBytecodeWithInputSignature, SIZE_T BytecodeLength, ID3DInputLayout** ppInputLayout);

	HRESULT	CreateQuery(const D3D_QUERY_DESC* pQueryDesc, ID3DQuery** ppQuery);

	void	EvictManagedResources() {}
};

//------------------------------------------------------------------------------
// dx12Context：状态累积 + PSO 合成 + 绘制（D3D11 形态 API → D3D12）
//------------------------------------------------------------------------------
class dx12Context
{
public:
	ID3D12GraphicsCommandList* Get() const { return dx12::GetD3D12CmdList(); }

	// --- 动态缓冲（D3D11 Map/Unmap → 上传环）---
	HRESULT	Map(ID3DResource* pResource, UINT Subresource, D3D_MAP MapType, UINT MapFlags, D3D_MAPPED_TEXTURE2D* pMappedResource);
	void	Unmap(ID3DResource* pResource, UINT Subresource);
	void	UpdateSubresource(ID3DResource* pDstResource, UINT DstSubresource, const D3D11_BOX* pDstBox,
		const void* pSrcData, UINT SrcRowPitch, UINT SrcDepthPitch);
	void	CopyResource(ID3DResource* pDstResource, ID3DResource* pSrcResource);

	// --- 渲染目标 ---
	void	ClearRenderTargetView(ID3DRenderTargetView* pRenderTargetView, const FLOAT ColorRGBA[4]);
	void	ClearDepthStencilView(ID3DDepthStencilView* pDepthStencilView, UINT ClearFlags, FLOAT Depth, UINT8 Stencil);
	void	OMSetRenderTargets(UINT NumViews, ID3DRenderTargetView* const* ppRenderTargetViews, ID3DDepthStencilView* pDepthStencilView);
	void	OMSetBlendState(ID3DBlendState* pBlendState, const FLOAT BlendFactor[4], UINT SampleMask);
	void	RSSetViewports(UINT NumViewports, const D3D_VIEWPORT* pViewports);
	void	RSSetScissorRects(UINT NumRects, const RECT* pRects);

	// --- 着色器绑定（DX12：记录选择，PSO 在 Draw 时合成）---
	void	VSSetShader(ID3DVertexShader* pVertexShader, ID3D11ClassInstance* const* ppClassInstances, UINT NumClassInstances);
	void	PSSetShader(ID3DPixelShader* pPixelShader, ID3D11ClassInstance* const* ppClassInstances, UINT NumClassInstances);
	void	GSSetShader(ID3DGeometryShader* pShader, ID3D11ClassInstance* const* ppClassInstances, UINT NumClassInstances);
	void	HSSetShader(ID3DHullShader* pShader, ID3D11ClassInstance* const* ppClassInstances, UINT NumClassInstances);
	void	DSSetShader(ID3DDomainShader* pShader, ID3D11ClassInstance* const* ppClassInstances, UINT NumClassInstances);
	void	CSSetShader(ID3DComputeShader* pShader, ID3D11ClassInstance* const* ppClassInstances, UINT NumClassInstances);

	void	CSSetShaderResources(UINT StartSlot, UINT NumViews, ID3DShaderResourceView* const* ppShaderResourceViews);
	void	CSSetSamplers(UINT StartSlot, UINT NumSamplers, ID3DSamplerState* const* ppSamplers);
	void	CSSetUnorderedAccessViews(UINT StartSlot, UINT NumUAVs, ID3DUnorderedAccessView* const* ppUAVs, const UINT* pUAVInitialCounts);
	void	GenerateMips(ID3DShaderResourceView* pShaderResourceView);

	void	VSSetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3DBuffer* const* ppConstantBuffers);
	void	PSSetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3DBuffer* const* ppConstantBuffers);
	void	GSSetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3DBuffer* const* ppConstantBuffers);
	void	HSSetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3DBuffer* const* ppConstantBuffers);
	void	DSSetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3DBuffer* const* ppConstantBuffers);
	void	CSSetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3DBuffer* const* ppConstantBuffers);

	// --- 输入装配 ---
	void	IASetInputLayout(ID3DInputLayout* pInputLayout);
	void	IASetVertexBuffers(UINT StartSlot, UINT NumBuffers, ID3DVertexBuffer* const* ppVertexBuffers, const UINT* pStrides, const UINT* pOffsets);
	void	IASetIndexBuffer(ID3DIndexBuffer* pIndexBuffer, DXGI_FORMAT Format, UINT Offset);
	void	IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY Topology);

	// --- 绘制 ---
	void	DrawIndexed(UINT IndexCount, UINT StartIndexLocation, INT BaseVertexLocation);
	void	Draw(UINT VertexCount, UINT StartVertexLocation);
	void	DrawIndexedInstanced(UINT IndexCountPerInstance, UINT InstanceCount, UINT StartIndexLocation, INT BaseVertexLocation, UINT StartInstanceLocation);
	void	Dispatch(UINT ThreadGroupCountX, UINT ThreadGroupCountY, UINT ThreadGroupCountZ);

	// --- 查询（D3D12 简化实现）---
	HRESULT	GetData(ID3DQuery* pAsync, void* pData, UINT DataSize, UINT GetDataFlags);
	void	Begin(ID3DQuery* pAsync);
	void	End(ID3DQuery* pAsync);

	// --- 内部：在 Draw 前合成/绑定 PSO 与描述符表 ---
	void	FlushPipeline();
	void	BindShaderResources();
	void	BindConstantBuffers();

public:
	// 当前状态（供 PSO 合成）
	ID3DVertexShader*	vs = nullptr;
	ID3DPixelShader*	ps = nullptr;
	ID3DGeometryShader*	gs = nullptr;
	ID3DHullShader*		hs = nullptr;
	ID3DDomainShader*	ds = nullptr;
	ID3DComputeShader*	cs = nullptr;
	ID3DInputLayout*	layout = nullptr;
	ID3DVertexBuffer*	vb = nullptr;
	UINT				vbStride = 0;
	ID3DIndexBuffer*	ib = nullptr;
	DXGI_FORMAT			ibFormat = DXGI_FORMAT_R16_UINT;
	bool				ib32 = false;
	D3D_PRIMITIVE_TOPOLOGY	topology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;

	ID3DBuffer*			cbVS[14] = {};
	ID3DBuffer*			cbPS[14] = {};

	bool				rtDirty = true;
	ID3DRenderTargetView*	rt[4] = {};
	ID3DDepthStencilView*	zb = nullptr;
	UINT				rtCount = 1;

	D3D12_VIEWPORT		viewport = {};
	bool				viewportSet = false;
};

extern dx12Device		DX12Device;
extern dx12Context		DX12Context;
