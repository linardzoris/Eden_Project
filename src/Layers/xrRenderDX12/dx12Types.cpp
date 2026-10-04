#include "stdafx.h"
#include "dx12Types.h"
#include "dx12Backend.h"
#include "StateManager/dx12StateManager.h"
#include "StateManager/dx12ShaderResourceStateCache.h"

dx12Device	DX12Device;
dx12Context	DX12Context;

//------------------------------------------------------------------------------
// 工具
//------------------------------------------------------------------------------
static UINT64 FNV(const void* data, size_t size, UINT64 h = 1469598103934665603ull)
{
	const u8* p = (const u8*)data;
	for (size_t i = 0; i < size; ++i) { h ^= p[i]; h *= 1099511628211ull; }
	return h;
}

UINT64 dx12SamplerState::Hash() const		{ return FNV(&desc, sizeof(desc)); }
UINT64 dx12RasterizerState::Hash() const	{ return FNV(&desc, sizeof(desc)); }
UINT64 dx12DepthStencilState::Hash() const	{ UINT64 h = FNV(&desc, sizeof(desc)); h = FNV(&stencilRef, sizeof(stencilRef), h); return h; }
UINT64 dx12BlendState::Hash() const			{ UINT64 h = FNV(&desc, sizeof(desc)); h = FNV(&mask, sizeof(mask), h); return h; }

static UINT AlignRowPitch(UINT width, UINT bpp)
{
	UINT pitch = width * bpp;
	return (pitch + (D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1)) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
}

static UINT BytesPerPixel(DXGI_FORMAT fmt)
{
	switch (fmt)
	{
	case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
	case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return 4;
	case DXGI_FORMAT_R8_UNORM: return 1;
	case DXGI_FORMAT_R16G16B16A16_FLOAT: return 8;
	case DXGI_FORMAT_R16_FLOAT: return 2;
	case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC1_UNORM_SRGB:
	case DXGI_FORMAT_BC4_UNORM: return 0;	// 压缩格式不走行拷贝
	case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC2_UNORM_SRGB:
	case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC3_UNORM_SRGB:
	case DXGI_FORMAT_BC5_UNORM: return 0;
	default: return 4;
	}
}

//------------------------------------------------------------------------------
// 状态描述转换（D3D11 → D3D12，枚举数值一致，逐字段拷贝）
//------------------------------------------------------------------------------
static D3D12_RASTERIZER_DESC ConvRaster(const D3D_RASTERIZER_DESC& d)
{
	D3D12_RASTERIZER_DESC r = {};
	r.FillMode = (D3D12_FILL_MODE)d.FillMode;
	r.CullMode = (D3D12_CULL_MODE)d.CullMode;
	r.FrontCounterClockwise = d.FrontCounterClockwise;
	r.DepthBias = d.DepthBias;
	r.DepthBiasClamp = d.DepthBiasClamp;
	r.SlopeScaledDepthBias = d.SlopeScaledDepthBias;
	r.DepthClipEnable = d.DepthClipEnable;
	r.MultisampleEnable = d.MultisampleEnable;
	r.AntialiasedLineEnable = d.AntialiasedLineEnable;
	r.ForcedSampleCount = 0;
	r.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
	return r;
}

static D3D12_DEPTH_STENCIL_DESC ConvDepthStencil(const D3D_DEPTH_STENCIL_DESC& d)
{
	D3D12_DEPTH_STENCIL_DESC r = {};
	r.DepthEnable = d.DepthEnable;
	r.DepthWriteMask = (D3D12_DEPTH_WRITE_MASK)d.DepthWriteMask;
	r.DepthFunc = (D3D12_COMPARISON_FUNC)d.DepthFunc;
	r.StencilEnable = d.StencilEnable;
	r.StencilReadMask = d.StencilReadMask;
	r.StencilWriteMask = d.StencilWriteMask;
	r.FrontFace.StencilFailOp = (D3D12_STENCIL_OP)d.FrontFace.StencilFailOp;
	r.FrontFace.StencilDepthFailOp = (D3D12_STENCIL_OP)d.FrontFace.StencilDepthFailOp;
	r.FrontFace.StencilPassOp = (D3D12_STENCIL_OP)d.FrontFace.StencilPassOp;
	r.FrontFace.StencilFunc = (D3D12_COMPARISON_FUNC)d.FrontFace.StencilFunc;
	r.BackFace.StencilFailOp = (D3D12_STENCIL_OP)d.BackFace.StencilFailOp;
	r.BackFace.StencilDepthFailOp = (D3D12_STENCIL_OP)d.BackFace.StencilDepthFailOp;
	r.BackFace.StencilPassOp = (D3D12_STENCIL_OP)d.BackFace.StencilPassOp;
	r.BackFace.StencilFunc = (D3D12_COMPARISON_FUNC)d.BackFace.StencilFunc;
	return r;
}

static D3D12_BLEND_DESC ConvBlend(const D3D_BLEND_DESC& d)
{
	D3D12_BLEND_DESC r = {};
	r.AlphaToCoverageEnable = d.AlphaToCoverageEnable;
	r.IndependentBlendEnable = d.IndependentBlendEnable;
	for (int i = 0; i < 8; ++i)
	{
		r.RenderTarget[i].BlendEnable = d.RenderTarget[i].BlendEnable;
		r.RenderTarget[i].LogicOpEnable = FALSE;
		r.RenderTarget[i].SrcBlend = (D3D12_BLEND)d.RenderTarget[i].SrcBlend;
		r.RenderTarget[i].DestBlend = (D3D12_BLEND)d.RenderTarget[i].DestBlend;
		r.RenderTarget[i].BlendOp = (D3D12_BLEND_OP)d.RenderTarget[i].BlendOp;
		r.RenderTarget[i].SrcBlendAlpha = (D3D12_BLEND)d.RenderTarget[i].SrcBlendAlpha;
		r.RenderTarget[i].DestBlendAlpha = (D3D12_BLEND)d.RenderTarget[i].DestBlendAlpha;
		r.RenderTarget[i].BlendOpAlpha = (D3D12_BLEND_OP)d.RenderTarget[i].BlendOpAlpha;
		r.RenderTarget[i].LogicOp = D3D12_LOGIC_OP_NOOP;
		r.RenderTarget[i].RenderTargetWriteMask = d.RenderTarget[i].RenderTargetWriteMask;
	}
	return r;
}

//------------------------------------------------------------------------------
// dx12Device
//------------------------------------------------------------------------------
HRESULT dx12Device::CreateBuffer(const D3D_BUFFER_DESC* pDesc, const D3D_SUBRESOURCE_DATA* pInit, ID3DBuffer** ppBuffer)
{
	if (!pDesc || !ppBuffer) return E_INVALIDARG;
	ID3D12Device* dev = Get();
	if (!dev) { Msg("! DX12: CreateBuffer with null device"); return E_FAIL; }

	dx12Buffer* buf = new dx12Buffer();
	buf->size = pDesc->ByteWidth;
	buf->immutable = (pDesc->Usage == D3D_USAGE_IMMUTABLE);
	buf->isConstant = (pDesc->BindFlags & D3D_BIND_CONSTANT_BUFFER) != 0;

	D3D12_HEAP_PROPERTIES heap = {};
	heap.Type = D3D12_HEAP_TYPE_UPLOAD;

	D3D12_RESOURCE_DESC rd = {};
	rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	rd.Width = pDesc->ByteWidth;
	rd.Height = 1;
	rd.DepthOrArraySize = 1;
	rd.MipLevels = 1;
	rd.Format = DXGI_FORMAT_UNKNOWN;
	rd.SampleDesc.Count = 1;
	rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

	HRESULT hr = dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rd,
		D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&buf->resource));
	if (FAILED(hr)) { Msg("! DX12: CreateBuffer CreateCommittedResource failed 0x%08x", hr); buf->Release(); return hr; }

	D3D12_RANGE rr = { 0, 0 };
	buf->resource->Map(0, &rr, &buf->mapped);
	buf->gpuVA = buf->resource->GetGPUVirtualAddress();
	if (pInit && pInit->pSysMem && buf->mapped)
		memcpy(buf->mapped, pInit->pSysMem, pDesc->ByteWidth);

	*ppBuffer = buf;
	return S_OK;
}

HRESULT dx12Device::CreateTexture2D(const D3D_TEXTURE2D_DESC* pDesc, const D3D_SUBRESOURCE_DATA* pInit, ID3DTexture2D** ppTexture)
{
	if (!pDesc || !ppTexture) return E_INVALIDARG;
	ID3D12Device* dev = Get();
	if (!dev) return E_FAIL;

	dx12Texture* tex = new dx12Texture();
	tex->desc = *pDesc;

	D3D12_HEAP_PROPERTIES heap = {};
	heap.Type = D3D12_HEAP_TYPE_UPLOAD;

	D3D12_RESOURCE_DESC rd = {};
	rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	rd.Width = pDesc->Width;
	rd.Height = pDesc->Height;
	rd.DepthOrArraySize = (UINT16)(pDesc->ArraySize ? pDesc->ArraySize : 1);
	rd.MipLevels = (UINT16)(pDesc->MipLevels ? pDesc->MipLevels : 1);
	rd.Format = pDesc->Format;
	rd.SampleDesc = pDesc->SampleDesc;
	rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	rd.Flags = D3D12_RESOURCE_FLAG_NONE;
	if (pDesc->BindFlags & D3D_BIND_RENDER_TARGET) rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
	if (pDesc->BindFlags & D3D_BIND_DEPTH_STENCIL) rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
	if (pDesc->BindFlags & D3D_BIND_UNORDERED_ACCESS) rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

	HRESULT hr = dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rd,
		D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&tex->resource));
	if (FAILED(hr)) { Msg("! DX12: CreateTexture2D failed 0x%08x", hr); tex->Release(); return hr; }

	tex->state = D3D12_RESOURCE_STATE_GENERIC_READ;

	if (pInit && pInit->pSysMem)
	{
		UINT bpp = BytesPerPixel(pDesc->Format);
		D3D12_RANGE rr = { 0, 0 };
		void* dst = nullptr;
		if (SUCCEEDED(tex->resource->Map(0, &rr, &dst)) && dst && bpp)
		{
			UINT dstPitch = AlignRowPitch(pDesc->Width, bpp);
			const u8* src = (const u8*)pInit->pSysMem;
			UINT srcPitch = pInit->SysMemPitch ? pInit->SysMemPitch : pDesc->Width * bpp;
			UINT rows = pDesc->Height;
			for (UINT y = 0; y < rows; ++y)
				memcpy((u8*)dst + (size_t)y * dstPitch, src + (size_t)y * srcPitch, pDesc->Width * bpp);
			tex->resource->Unmap(0, nullptr);
		}
	}

	*ppTexture = tex;
	return S_OK;
}

HRESULT dx12Device::CreateTexture3D(const D3D_TEXTURE3D_DESC* pDesc, const D3D_SUBRESOURCE_DATA* pInit, ID3DTexture3D** ppTexture)
{
	if (!pDesc || !ppTexture) return E_INVALIDARG;
	ID3D12Device* dev = Get();
	if (!dev) return E_FAIL;

	dx12Texture* tex = new dx12Texture();

	D3D12_HEAP_PROPERTIES heap = {};
	heap.Type = D3D12_HEAP_TYPE_UPLOAD;

	D3D12_RESOURCE_DESC rd = {};
	rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
	rd.Width = pDesc->Width;
	rd.Height = pDesc->Height;
	rd.DepthOrArraySize = (UINT16)(pDesc->Depth ? pDesc->Depth : 1);
	rd.MipLevels = (UINT16)(pDesc->MipLevels ? pDesc->MipLevels : 1);
	rd.Format = pDesc->Format;
	rd.SampleDesc.Count = 1;
	rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

	HRESULT hr = dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rd,
		D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&tex->resource));
	if (FAILED(hr)) { tex->Release(); return hr; }

	tex->desc.Width = pDesc->Width;
	tex->desc.Height = pDesc->Height;
	tex->desc.Format = pDesc->Format;
	(void)pInit;
	*ppTexture = tex;
	return S_OK;
}

HRESULT dx12Device::CreateShaderResourceView(ID3DResource* pResource, const D3D_SHADER_RESOURCE_VIEW_DESC* pDesc, ID3DShaderResourceView** ppSRView)
{
	if (!pResource || !ppSRView) return E_INVALIDARG;
	ID3D12Device* dev = Get();
	if (!dev) return E_FAIL;

	dx12ShaderResourceView* srv = new dx12ShaderResourceView();
	srv->resource = pResource->resource;

	D3D12_CPU_DESCRIPTOR_HANDLE cpu = {};
	if (!dx12::g_backend.persistentSrv.AllocPersistent(1, cpu))
	{
		srv->Release();
		return E_FAIL;
	}
	srv->cpu = cpu;
	srv->valid = true;

	if (pDesc)
	{
		D3D12_SHADER_RESOURCE_VIEW_DESC d = {};
		d.Format = pDesc->Format;
		d.ViewDimension = (D3D12_SRV_DIMENSION)pDesc->ViewDimension;
		d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		switch (d.ViewDimension)
		{
		case D3D12_SRV_DIMENSION_BUFFER:
			d.Buffer.FirstElement = pDesc->Buffer.FirstElement;
			d.Buffer.NumElements = pDesc->Buffer.NumElements;
			break;
		case D3D12_SRV_DIMENSION_TEXTURE2D:
			d.Texture2D.MostDetailedMip = pDesc->Texture2D.MostDetailedMip;
			d.Texture2D.MipLevels = pDesc->Texture2D.MipLevels;
			break;
		case D3D12_SRV_DIMENSION_TEXTURECUBE:
			d.TextureCube.MostDetailedMip = pDesc->TextureCube.MostDetailedMip;
			d.TextureCube.MipLevels = pDesc->TextureCube.MipLevels;
			break;
		default:
			break;
		}
		srv->desc = d;
		dev->CreateShaderResourceView(srv->resource.Get(), &d, cpu);
	}
	else
	{
		D3D12_SHADER_RESOURCE_VIEW_DESC d = {};
		d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		d.Format = pResource->resource->GetDesc().Format;
		D3D12_RESOURCE_DESC rd = srv->resource->GetDesc();
		if (rd.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
		{
			d.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
			d.Buffer.FirstElement = 0;
			d.Buffer.NumElements = (UINT)(rd.Width / 4);
		}
		else
		{
			d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			d.Texture2D.MostDetailedMip = 0;
			d.Texture2D.MipLevels = rd.MipLevels;
		}
		srv->desc = d;
		dev->CreateShaderResourceView(srv->resource.Get(), &d, cpu);
	}

	*ppSRView = srv;
	return S_OK;
}

HRESULT dx12Device::CreateRenderTargetView(ID3DResource* pResource, const D3D_RENDER_TARGET_VIEW_DESC* pDesc, ID3DRenderTargetView** ppRTView)
{
	if (!pResource || !ppRTView) return E_INVALIDARG;
	ID3D12Device* dev = Get();
	if (!dev) return E_FAIL;

	dx12RenderTargetView* rtv = new dx12RenderTargetView();
	rtv->resource = pResource->resource;

	D3D12_CPU_DESCRIPTOR_HANDLE cpu = {};
	if (!dx12::g_backend.persistentRtv.AllocPersistent(1, cpu)) { rtv->Release(); return E_FAIL; }
	rtv->cpu = cpu;
	rtv->valid = true;

	if (pDesc)
	{
		D3D12_RENDER_TARGET_VIEW_DESC d = {};
		d.Format = pDesc->Format;
		d.ViewDimension = (D3D12_RTV_DIMENSION)pDesc->ViewDimension;
		memcpy(&d.Texture2D, &pDesc->Texture2D, sizeof(D3D12_TEX2D_RTV));
		dev->CreateRenderTargetView(rtv->resource.Get(), &d, cpu);
	}
	else
	{
		dev->CreateRenderTargetView(rtv->resource.Get(), nullptr, cpu);
	}
	*ppRTView = rtv;
	return S_OK;
}

HRESULT dx12Device::CreateDepthStencilView(ID3DResource* pResource, const D3D_DEPTH_STENCIL_VIEW_DESC* pDesc, ID3DDepthStencilView** ppDSView)
{
	if (!pResource || !ppDSView) return E_INVALIDARG;
	ID3D12Device* dev = Get();
	if (!dev) return E_FAIL;

	dx12DepthStencilView* dsv = new dx12DepthStencilView();
	dsv->resource = pResource->resource;

	D3D12_CPU_DESCRIPTOR_HANDLE cpu = {};
	if (!dx12::g_backend.persistentDsv.AllocPersistent(1, cpu)) { dsv->Release(); return E_FAIL; }
	dsv->cpu = cpu;
	dsv->valid = true;

	if (pDesc)
	{
		D3D12_DEPTH_STENCIL_VIEW_DESC d = {};
		d.Format = pDesc->Format;
		d.ViewDimension = (D3D12_DSV_DIMENSION)pDesc->ViewDimension;
		d.Flags = D3D12_DSV_FLAG_NONE;
		memcpy(&d.Texture2D, &pDesc->Texture2D, sizeof(D3D12_TEX2D_DSV));
		dev->CreateDepthStencilView(dsv->resource.Get(), &d, cpu);
	}
	else
	{
		dev->CreateDepthStencilView(dsv->resource.Get(), nullptr, cpu);
	}
	*ppDSView = dsv;
	return S_OK;
}

HRESULT dx12Device::CreateUnorderedAccessView(ID3DResource* pResource, const D3D11_UNORDERED_ACCESS_VIEW_DESC* pDesc, ID3DUnorderedAccessView** ppUAView)
{
	if (!pResource || !ppUAView) return E_INVALIDARG;
	ID3D12Device* dev = Get();
	if (!dev) return E_FAIL;

	dx12UnorderedAccessView* uav = new dx12UnorderedAccessView();
	uav->resource = pResource->resource;

	D3D12_CPU_DESCRIPTOR_HANDLE cpu = {};
	if (!dx12::g_backend.persistentUav.AllocPersistent(1, cpu)) { uav->Release(); return E_FAIL; }
	uav->cpu = cpu;
	uav->valid = true;

	ID3D12Resource* pCounter = nullptr;
	const D3D12_UNORDERED_ACCESS_VIEW_DESC* pUavDesc = nullptr;
	dev->CreateUnorderedAccessView(uav->resource.Get(), pCounter, pUavDesc, cpu);
	*ppUAView = uav;
	return S_OK;
}

HRESULT dx12Device::CreateSamplerState(const D3D_SAMPLER_DESC* pDesc, ID3DSamplerState** ppSamplerState)
{
	if (!ppSamplerState) return E_INVALIDARG;
	dx12SamplerState* s = new dx12SamplerState();
	if (pDesc) s->desc = *pDesc;
	*ppSamplerState = s;
	return S_OK;
}

HRESULT dx12Device::CreateRasterizerState(const D3D_RASTERIZER_DESC* pDesc, ID3DRasterizerState** ppRasterizerState)
{
	if (!ppRasterizerState) return E_INVALIDARG;
	dx12RasterizerState* s = new dx12RasterizerState();
	if (pDesc) s->desc = *pDesc;
	*ppRasterizerState = s;
	return S_OK;
}

HRESULT dx12Device::CreateDepthStencilState(const D3D_DEPTH_STENCIL_DESC* pDesc, UINT StencilRef, ID3DDepthStencilState** ppDepthStencilState)
{
	if (!ppDepthStencilState) return E_INVALIDARG;
	dx12DepthStencilState* s = new dx12DepthStencilState();
	if (pDesc) s->desc = *pDesc;
	s->stencilRef = StencilRef;
	*ppDepthStencilState = s;
	return S_OK;
}

HRESULT dx12Device::CreateBlendState(const D3D_BLEND_DESC* pDesc, ID3DBlendState** ppBlendState)
{
	if (!ppBlendState) return E_INVALIDARG;
	dx12BlendState* s = new dx12BlendState();
	if (pDesc) s->desc = *pDesc;
	*ppBlendState = s;
	return S_OK;
}

static HRESULT MakeShader(const void* code, SIZE_T len, dx12Shader::Stage stage, dx12Shader** pp)
{
	if (!code || !len || !pp) return E_INVALIDARG;
	dx12Shader* sh = new dx12Shader();
	sh->stage = stage;
	ID3DBlob* blob = nullptr;
	if (FAILED(D3DCreateBlob(len, &blob))) { sh->Release(); return E_FAIL; }
	memcpy(blob->GetBufferPointer(), code, len);
	sh->blob.Attach(blob);
	*pp = sh;
	return S_OK;
}

HRESULT dx12Device::CreateVertexShader(const void* c, SIZE_T l, ID3D11ClassLinkage*, ID3DVertexShader** pp)	{ return MakeShader(c, l, dx12Shader::stVS, pp); }
HRESULT dx12Device::CreatePixelShader(const void* c, SIZE_T l, ID3D11ClassLinkage*, ID3DPixelShader** pp)		{ return MakeShader(c, l, dx12Shader::stPS, pp); }
HRESULT dx12Device::CreateGeometryShader(const void* c, SIZE_T l, ID3D11ClassLinkage*, ID3DGeometryShader** pp)	{ return MakeShader(c, l, dx12Shader::stGS, pp); }
HRESULT dx12Device::CreateHullShader(const void* c, SIZE_T l, ID3D11ClassLinkage*, ID3DHullShader** pp)		{ return MakeShader(c, l, dx12Shader::stHS, pp); }
HRESULT dx12Device::CreateDomainShader(const void* c, SIZE_T l, ID3D11ClassLinkage*, ID3DDomainShader** pp)	{ return MakeShader(c, l, dx12Shader::stDS, pp); }
HRESULT dx12Device::CreateComputeShader(const void* c, SIZE_T l, ID3D11ClassLinkage*, ID3DComputeShader** pp)	{ return MakeShader(c, l, dx12Shader::stCS, pp); }

HRESULT dx12Device::CreateInputLayout(const D3D_INPUT_ELEMENT_DESC* pElems, UINT Num, const void*, SIZE_T, ID3DInputLayout** pp)
{
	if (!pp) return E_INVALIDARG;
	dx12InputLayout* il = new dx12InputLayout();
	il->elements.resize(Num);
	for (UINT i = 0; i < Num; ++i)
	{
		il->elements[i].SemanticName = pElems[i].SemanticName;
		il->elements[i].SemanticIndex = pElems[i].SemanticIndex;
		il->elements[i].Format = pElems[i].Format;
		il->elements[i].InputSlot = pElems[i].InputSlot;
		il->elements[i].AlignedByteOffset = pElems[i].AlignedByteOffset;
		il->elements[i].InputSlotClass = (D3D12_INPUT_CLASSIFICATION)pElems[i].InputSlotClass;
		il->elements[i].InstanceDataStepRate = pElems[i].InstanceDataStepRate;
	}
	*pp = il;
	return S_OK;
}

HRESULT dx12Device::CreateQuery(const D3D_QUERY_DESC* pDesc, ID3DQuery** ppQuery)
{
	if (!ppQuery) return E_INVALIDARG;
	ID3D12Device* dev = Get();
	if (!dev) return E_FAIL;

	dx12Query* q = new dx12Query();
	if (pDesc) q->desc = *pDesc;

	ComPtr<ID3D12Fence> fence;
	if (SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
		q->fence = fence;

	*ppQuery = q;
	return S_OK;
}

//------------------------------------------------------------------------------
// dx12Context
//------------------------------------------------------------------------------
HRESULT dx12Context::Map(ID3DResource* pResource, UINT, D3D_MAP, UINT, D3D_MAPPED_TEXTURE2D* pMapped)
{
	if (!pResource || !pMapped) return E_INVALIDARG;
	pMapped->pData = pResource->resource ? nullptr : nullptr;
	// 上传堆资源在创建时已持久映射
	// dx12Buffer / dx12Texture 的 mapped 指针在各自创建时取得
	if (auto* b = dynamic_cast<dx12Buffer*>(pResource))
	{
		pMapped->pData = b->mapped;
		pMapped->RowPitch = b->size;
		pMapped->DepthPitch = b->size;
	}
	else if (pResource->resource)
	{
		D3D12_RANGE rr = { 0, 0 };
		void* p = nullptr;
		if (SUCCEEDED(pResource->resource->Map(0, &rr, &p)))
		{
			pMapped->pData = p;
			pMapped->RowPitch = pMapped->DepthPitch = 0;
		}
	}
	return S_OK;
}

void dx12Context::Unmap(ID3DResource* pResource, UINT)
{
	if (!pResource) return;
	if (dynamic_cast<dx12Buffer*>(pResource)) return;	// 持久映射，不解除
	if (pResource->resource) pResource->resource->Unmap(0, nullptr);
}

void dx12Context::UpdateSubresource(ID3DResource* pDst, UINT, const D3D11_BOX* pBox, const void* pSrc, UINT, UINT)
{
	if (!pDst || !pSrc || !pDst->resource) return;
	// 简化：仅支持 buffer 的整体/区间拷贝（UPLOAD 堆持久映射）
	if (auto* b = dynamic_cast<dx12Buffer*>(pDst))
	{
		void* dst = b->mapped;
		if (!dst) { D3D12_RANGE rr = { 0,0 }; b->resource->Map(0, &rr, &dst); }
		if (!dst) return;
		UINT offset = pBox ? pBox->left : 0;
		UINT bytes = b->size - offset;
		if (pBox) bytes = pBox->right - pBox->left;
		memcpy((u8*)dst + offset, pSrc, bytes);
	}
}

void dx12Context::CopyResource(ID3DResource* pDst, ID3DResource* pSrc)
{
	if (!pDst || !pSrc || !pDst->resource || !pSrc->resource) return;
	ID3D12GraphicsCommandList* cl = Get();
	if (!cl) return;
	D3D12_RESOURCE_BARRIER b = {};
	b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	b.Transition.pResource = pSrc->resource.Get();
	b.Transition.StateBefore = D3D12_RESOURCE_STATE_GENERIC_READ;
	b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
	b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	cl->ResourceBarrier(1, &b);
	cl->CopyResource(pDst->resource.Get(), pSrc->resource.Get());
	std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
	cl->ResourceBarrier(1, &b);
}

void dx12Context::ClearRenderTargetView(ID3DRenderTargetView* pRTV, const FLOAT color[4])
{
	if (!pRTV || !pRTV->valid) return;
	ID3D12GraphicsCommandList* cl = Get();
	if (cl) cl->ClearRenderTargetView(pRTV->cpu, color, 0, nullptr);
}

void dx12Context::ClearDepthStencilView(ID3DDepthStencilView* pDSV, UINT flags, FLOAT depth, UINT8 stencil)
{
	if (!pDSV || !pDSV->valid) return;
	ID3D12GraphicsCommandList* cl = Get();
	if (cl) cl->ClearDepthStencilView(pDSV->cpu, (D3D12_CLEAR_FLAGS)flags, depth, stencil, 0, nullptr);
}

void dx12Context::OMSetRenderTargets(UINT num, ID3DRenderTargetView* const* ppRTV, ID3DDepthStencilView* pDSV)
{
	if (num > 4) num = 4;
	rtCount = num ? num : 1;
	for (UINT i = 0; i < 4; ++i) rt[i] = (i < num) ? ppRTV[i] : nullptr;
	zb = pDSV;
	rtDirty = true;

	ID3D12GraphicsCommandList* cl = Get();
	if (!cl) return;
	D3D12_CPU_DESCRIPTOR_HANDLE handles[4] = {};
	UINT n = 0;
	for (UINT i = 0; i < num; ++i)
		if (ppRTV[i] && ppRTV[i]->valid) handles[n++] = ppRTV[i]->cpu;
	D3D12_CPU_DESCRIPTOR_HANDLE dsv = (pDSV && pDSV->valid) ? pDSV->cpu : D3D12_CPU_DESCRIPTOR_HANDLE{ 0 };
	cl->OMSetRenderTargets(n, n ? handles : nullptr, FALSE, (pDSV && pDSV->valid) ? &dsv : nullptr);
}

void dx12Context::OMSetBlendState(ID3DBlendState* pBlendState, const FLOAT BlendFactor[4], UINT SampleMask)
{
	if (pBlendState)
		StateManager.m_BDesc = pBlendState->desc;
	if (BlendFactor)
		for (int i = 0; i < 4; ++i) {}
	StateManager.m_uiSampleMask = SampleMask ? SampleMask : 0xFFFFFFFF;
}

void dx12Context::RSSetViewports(UINT, const D3D_VIEWPORT* pViewports)
{
	if (!pViewports) return;
	viewport.TopLeftX = pViewports[0].TopLeftX;
	viewport.TopLeftY = pViewports[0].TopLeftY;
	viewport.Width = pViewports[0].Width;
	viewport.Height = pViewports[0].Height;
	viewport.MinDepth = pViewports[0].MinDepth;
	viewport.MaxDepth = pViewports[0].MaxDepth;
	viewportSet = true;
}

void dx12Context::RSSetScissorRects(UINT num, const RECT* pRects)
{
	ID3D12GraphicsCommandList* cl = Get();
	if (cl) cl->RSSetScissorRects(num, (const D3D12_RECT*)pRects);
}

void dx12Context::VSSetShader(ID3DVertexShader* s, ID3D11ClassInstance* const*, UINT) { vs = s; }
void dx12Context::PSSetShader(ID3DPixelShader* s, ID3D11ClassInstance* const*, UINT) { ps = s; }
void dx12Context::GSSetShader(ID3DGeometryShader* s, ID3D11ClassInstance* const*, UINT) { gs = s; }
void dx12Context::HSSetShader(ID3DHullShader* s, ID3D11ClassInstance* const*, UINT) { hs = s; }
void dx12Context::DSSetShader(ID3DDomainShader* s, ID3D11ClassInstance* const*, UINT) { ds = s; }
void dx12Context::CSSetShader(ID3DComputeShader* s, ID3D11ClassInstance* const*, UINT) { cs = s; }

#define CB_SETTER(list) { for (UINT i = 0; i < NumBuffers && (StartSlot + i) < 14; ++i) list[StartSlot + i] = pp[i]; }
void dx12Context::VSSetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3DBuffer* const* pp) { CB_SETTER(cbVS); }
void dx12Context::PSSetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3DBuffer* const* pp) { CB_SETTER(cbPS); }
void dx12Context::GSSetConstantBuffers(UINT, UINT, ID3DBuffer* const*) {}
void dx12Context::HSSetConstantBuffers(UINT, UINT, ID3DBuffer* const*) {}
void dx12Context::DSSetConstantBuffers(UINT, UINT, ID3DBuffer* const*) {}
void dx12Context::CSSetConstantBuffers(UINT, UINT, ID3DBuffer* const*) {}

// DX12：CS 资源/UAV/采样器绑定暂以 no-op 占位（计算着色器路径后续接入）
void dx12Context::CSSetShaderResources(UINT, UINT, ID3DShaderResourceView* const*) {}
void dx12Context::CSSetSamplers(UINT, UINT, ID3DSamplerState* const*) {}
void dx12Context::CSSetUnorderedAccessViews(UINT, UINT, ID3DUnorderedAccessView* const*, const UINT*) {}
void dx12Context::GenerateMips(ID3DShaderResourceView*) {}
#undef CB_SETTER

void dx12Context::IASetInputLayout(ID3DInputLayout* l) { layout = l; }

void dx12Context::IASetVertexBuffers(UINT, UINT, ID3DVertexBuffer* const* pp, const UINT* pStrides, const UINT*)
{
	vb = pp ? pp[0] : nullptr;
	vbStride = pStrides ? pStrides[0] : 0;
}

void dx12Context::IASetIndexBuffer(ID3DIndexBuffer* pIB, DXGI_FORMAT fmt, UINT)
{
	ib = pIB;
	ibFormat = fmt;
	ib32 = (fmt == DXGI_FORMAT_R32_UINT);
}

void dx12Context::IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY t) { topology = t; }

void dx12Context::DrawIndexed(UINT IndexCount, UINT StartIndexLocation, INT BaseVertexLocation)
{
	FlushPipeline();
	ID3D12GraphicsCommandList* cl = Get();
	if (cl) cl->DrawIndexedInstanced(IndexCount, 1, StartIndexLocation, BaseVertexLocation, 0);
}

void dx12Context::Draw(UINT VertexCount, UINT StartVertexLocation)
{
	FlushPipeline();
	ID3D12GraphicsCommandList* cl = Get();
	if (cl) cl->DrawInstanced(VertexCount, 1, StartVertexLocation, 0);
}

void dx12Context::DrawIndexedInstanced(UINT IndexCountPerInstance, UINT InstanceCount, UINT StartIndexLocation, INT BaseVertexLocation, UINT StartInstanceLocation)
{
	FlushPipeline();
	ID3D12GraphicsCommandList* cl = Get();
	if (cl) cl->DrawIndexedInstanced(IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation, StartInstanceLocation);
}

void dx12Context::Dispatch(UINT x, UINT y, UINT z)
{
	ID3D12GraphicsCommandList* cl = Get();
	if (!cl) return;
	if (cs) cl->SetComputeRootSignature(dx12::GetRootSignature());
	cl->Dispatch(x, y, z);
}

HRESULT dx12Context::GetData(ID3DQuery* pAsync, void*, UINT, UINT)
{
	if (!pAsync) return E_INVALIDARG;
	// 阶段 0：不阻塞，返回未就绪
	return S_FALSE;
}

void dx12Context::Begin(ID3DQuery*) {}
void dx12Context::End(ID3DQuery*) {}

//------------------------------------------------------------------------------
// PSO 合成 + 描述符绑定
//------------------------------------------------------------------------------
void dx12Context::FlushPipeline()
{
	ID3D12GraphicsCommandList* cl = Get();
	if (!cl || !dx12::g_backend.valid) return;

	// ---- PSO key ----
	UINT64 key = (UINT64)(UINT_PTR)vs * 1000003ull;
	key = FNV(&ps, sizeof(ps), key);
	key = FNV(&layout, sizeof(layout), key);
	key = FNV(&StateManager.m_RDesc, sizeof(StateManager.m_RDesc), key);
	key = FNV(&StateManager.m_DSDesc, sizeof(StateManager.m_DSDesc), key);
	key = FNV(&StateManager.m_BDesc, sizeof(StateManager.m_BDesc), key);
	key = FNV(&topology, sizeof(topology), key);
	if (gs) key = FNV(&gs, sizeof(gs), key);
	if (hs) key = FNV(&hs, sizeof(hs), key);
	if (ds) key = FNV(&ds, sizeof(ds), key);
	for (UINT i = 0; i < 4; ++i)
	{
		DXGI_FORMAT f = (rt[i] && rt[i]->resource) ? rt[i]->resource->GetDesc().Format : DXGI_FORMAT_UNKNOWN;
		key = FNV(&f, sizeof(f), key);
	}
	DXGI_FORMAT dsvFmt = (zb && zb->resource) ? DXGI_FORMAT_D24_UNORM_S8_UINT : DXGI_FORMAT_UNKNOWN;
	key = FNV(&dsvFmt, sizeof(dsvFmt), key);

	auto it = dx12::g_backend.psoCache.find(key);
	ID3D12PipelineState* pso = nullptr;
	if (it != dx12::g_backend.psoCache.end())
	{
		pso = it->second.Get();
	}
	else if (vs && ps)
	{
		D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
		d.pRootSignature = dx12::GetRootSignature();
		d.VS = vs->Bytecode();
		d.PS = ps->Bytecode();
		if (gs) d.GS = gs->Bytecode();
		if (hs) d.HS = hs->Bytecode();
		if (ds) d.DS = ds->Bytecode();
		d.RasterizerState = ConvRaster(StateManager.m_RDesc);
		d.DepthStencilState = ConvDepthStencil(StateManager.m_DSDesc);
		d.BlendState = ConvBlend(StateManager.m_BDesc);
		d.SampleMask = StateManager.m_uiSampleMask;
		d.NodeMask = 0;
		d.SampleDesc.Count = 1;
		d.NumRenderTargets = rtCount;
		for (UINT i = 0; i < 4; ++i)
			d.RTVFormats[i] = (i < rtCount && rt[i] && rt[i]->resource) ? rt[i]->resource->GetDesc().Format : DXGI_FORMAT_UNKNOWN;
		d.DSVFormat = dsvFmt;

		switch (topology)
		{
		case D3D_PRIMITIVE_TOPOLOGY_POINTLIST: d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT; break;
		case D3D_PRIMITIVE_TOPOLOGY_LINELIST:
		case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP: d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE; break;
		default: d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE; break;
		}

		if (layout) d.InputLayout = layout->Desc();

		ComPtr<ID3D12PipelineState> newPso;
		HRESULT hr = dx12::GetD3D12Device()->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&newPso));
		if (FAILED(hr))
		{
			Msg("! DX12: CreateGraphicsPipelineState failed 0x%08x (vs=%p ps=%p lay=%p)", hr, vs, ps, layout);
			return;
		}
		pso = newPso.Get();
		dx12::g_backend.psoCache.emplace(key, newPso);
	}

	if (!pso) return;

	cl->SetPipelineState(pso);
	cl->SetGraphicsRootSignature(dx12::GetRootSignature());

	ID3D12DescriptorHeap* heaps[1] = { dx12::g_backend.cbvSrvUav.Heap() };
	cl->SetDescriptorHeaps(1, heaps);

	// ---- CBV 表（b0..b13） ----
	D3D12_CPU_DESCRIPTOR_HANDLE cbvCpu = {};
	D3D12_GPU_DESCRIPTOR_HANDLE cbvGpu = {};
	if (dx12::g_backend.cbvSrvUav.Alloc(14, cbvCpu, cbvGpu))
	{
		UINT size = dx12::g_backend.cbvSrvUav.DescriptorSize();
		for (UINT i = 0; i < 14; ++i)
		{
			D3D12_CPU_DESCRIPTOR_HANDLE dst = cbvCpu; dst.ptr += (SIZE_T)i * size;
			ID3DBuffer* b = cbVS[i] ? cbVS[i] : cbPS[i];
			if (b && b->resource)
			{
				D3D12_CONSTANT_BUFFER_VIEW_DESC cbd = {};
				cbd.BufferLocation = b->gpuVA;
				cbd.SizeInBytes = (b->size + 255) & ~255u;
				dx12::GetD3D12Device()->CreateConstantBufferView(&cbd, dst);
			}
		}
		cl->SetGraphicsRootDescriptorTable(0, cbvGpu);
	}

	// ---- SRV 表（t0..t15） ----
	D3D12_CPU_DESCRIPTOR_HANDLE srvCpu = {};
	D3D12_GPU_DESCRIPTOR_HANDLE srvGpu = {};
	if (dx12::g_backend.cbvSrvUav.Alloc(16, srvCpu, srvGpu))
	{
		UINT size = dx12::g_backend.cbvSrvUav.DescriptorSize();
		for (UINT i = 0; i < 16; ++i)
		{
			D3D12_CPU_DESCRIPTOR_HANDLE dst = srvCpu; dst.ptr += (SIZE_T)i * size;
			ID3DShaderResourceView* srv = SRVSManager.GetPS(i);
			if (srv && srv->valid)
			{
				dx12::GetD3D12Device()->CopyDescriptorsSimple(1, dst, srv->cpu, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
			}
		}
		cl->SetGraphicsRootDescriptorTable(1, srvGpu);
	}

	// ---- IA ----
	// DX12 的输入布局内置于 PSO，无需 IASetInputLayout
	if (vb && vb->resource)
	{
		D3D12_VERTEX_BUFFER_VIEW vbv = {};
		vbv.BufferLocation = vb->gpuVA;
		vbv.SizeInBytes = vb->size;
		vbv.StrideInBytes = vbStride;
		cl->IASetVertexBuffers(0, 1, &vbv);
	}
	if (ib && ib->resource)
	{
		D3D12_INDEX_BUFFER_VIEW ibv = {};
		ibv.BufferLocation = ib->gpuVA;
		ibv.SizeInBytes = ib->size;
		ibv.Format = ibFormat;
		cl->IASetIndexBuffer(&ibv);
	}
	cl->IASetPrimitiveTopology((D3D12_PRIMITIVE_TOPOLOGY)topology);

	if (viewportSet) cl->RSSetViewports(1, &viewport);
}

void dx12Context::BindShaderResources() {}
void dx12Context::BindConstantBuffers() {}
