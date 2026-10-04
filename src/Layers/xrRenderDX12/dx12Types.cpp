#include "stdafx.h"
#include "dx12Types.h"
#include "dx12Backend.h"
#include "StateManager/dx12StateManager.h"
#include "StateManager/dx12ShaderResourceStateCache.h"

// [fontbind] 临时诊断：atlas 身份记录定义在 dxFontRender.cpp
extern ID3DShaderResourceView*	g_r5LastAtlasSrv;
extern ID3D12Resource*			g_r5LastAtlasRes;
extern u32						g_r5LastAtlasW, g_r5LastAtlasH;
extern char						g_r5LastAtlasName[128];

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

//------------------------------------------------------------------------------
// 资源状态跟踪：D3D11 由驱动自动管理同一资源在 RTV/DSV/SRV 间的状态，
// D3D12 则需要显式 transition barrier。这里以资源指针为键记录当前状态，
// 在资源创建时注册、在 RTV/DSV/SRV 绑定点按需转换。
//------------------------------------------------------------------------------
namespace
{
	std::mutex												g_r5ResStateMutex;
	std::unordered_map<ID3D12Resource*, D3D12_RESOURCE_STATES>	g_r5ResStates;

	void R5_TrackResource(ID3D12Resource* r, D3D12_RESOURCE_STATES s)
	{
		if (!r) return;
		std::lock_guard<std::mutex> g(g_r5ResStateMutex);
		g_r5ResStates[r] = s;
	}

	void R5_TransitionResource(ID3D12GraphicsCommandList* cl, ID3D12Resource* r, D3D12_RESOURCE_STATES to)
	{
		if (!cl || !r) return;

		D3D12_RESOURCE_STATES from;
		{
			std::lock_guard<std::mutex> g(g_r5ResStateMutex);
			auto it = g_r5ResStates.find(r);
			from = (it != g_r5ResStates.end()) ? it->second : D3D12_RESOURCE_STATE_COMMON;
		}
		if (from == to) return;

		D3D12_RESOURCE_BARRIER b = {};
		b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		b.Transition.pResource = r;
		b.Transition.StateBefore = from;
		b.Transition.StateAfter = to;
		b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		cl->ResourceBarrier(1, &b);

		std::lock_guard<std::mutex> g(g_r5ResStateMutex);
		g_r5ResStates[r] = to;
	}
}

// 对外暴露的注册入口（供 dx12TextureUtils 等其他创建路径调用）
void R5RegisterResourceState(ID3D12Resource* r, D3D12_RESOURCE_STATES s)
{
	R5_TrackResource(r, s);
}

// D3D12 深度模板资源必须提供具体的 ClearValue 格式（TYPELESS 需映射到 D/S 格式）
static DXGI_FORMAT DSVFormat(DXGI_FORMAT fmt)
{
	switch (fmt)
	{
	case DXGI_FORMAT_R24G8_TYPELESS:	return DXGI_FORMAT_D24_UNORM_S8_UINT;
	case DXGI_FORMAT_R32G8X24_TYPELESS:	return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
	case DXGI_FORMAT_R32_TYPELESS:		return DXGI_FORMAT_D32_FLOAT;
	case DXGI_FORMAT_R16_TYPELESS:		return DXGI_FORMAT_D16_UNORM;
	default:							return fmt;
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

	const bool bRT = (pDesc->BindFlags & D3D_BIND_RENDER_TARGET) != 0;
	const bool bDS = (pDesc->BindFlags & D3D_BIND_DEPTH_STENCIL) != 0;
	const bool bUAV = (pDesc->BindFlags & D3D_BIND_UNORDERED_ACCESS) != 0;
	const bool bStagingRead = (pDesc->Usage == D3D_USAGE_STAGING) && ((pDesc->CPUAccessFlags & D3D_CPU_ACCESS_READ) != 0);

	// D3D12 堆选择：READBACK 堆不能承载纹理，故 STAGING+READ 纹理也放 DEFAULT 堆
	// （读回由 Map → ReadbackTextureSubresource 完成）；CPU 写的 buffer 型资源走 UPLOAD。
	// NOTE: 纹理一律 DEFAULT 堆，上传走 UploadTextureSubresource。
	D3D12_HEAP_PROPERTIES heap = {};
	heap.Type = D3D12_HEAP_TYPE_DEFAULT;
	tex->stagingRead = bStagingRead;

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
	if (bRT) rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
	if (bDS) rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
	if (bUAV) rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

	D3D12_RESOURCE_STATES initState;
	if (bStagingRead)		initState = D3D12_RESOURCE_STATE_COPY_DEST;
	else if (bRT)			initState = D3D12_RESOURCE_STATE_RENDER_TARGET;
	else if (bUAV)			initState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	else if (bDS)			initState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
	else					initState = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;

	// 深度模板资源在 D3D12 下必须带具体格式的 ClearValue；RT 也传 ClearValue 以消除性能告警
	D3D12_CLEAR_VALUE clear = {};
	const D3D12_CLEAR_VALUE* pClear = nullptr;
	if (bDS)
	{
		clear.Format = DSVFormat(pDesc->Format);
		clear.DepthStencil.Depth = 1.0f;
		clear.DepthStencil.Stencil = 0;
		pClear = &clear;
	}
	else if (bRT)
	{
		clear.Format = pDesc->Format;
		clear.Color[0] = 0.f; clear.Color[1] = 0.f; clear.Color[2] = 0.f; clear.Color[3] = 0.f;
		pClear = &clear;
	}

	HRESULT hr = dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rd,
		initState, pClear, IID_PPV_ARGS(&tex->resource));
	if (FAILED(hr))
	{
		Msg("! DX12: CreateTexture2D failed 0x%08x (fmt=%d %ux%u mip=%u ss=%u bind=%u flags=%u)",
			hr, (int)pDesc->Format, pDesc->Width, pDesc->Height, pDesc->MipLevels,
			pDesc->SampleDesc.Count, pDesc->BindFlags, (unsigned)rd.Flags);
		dx12::DumpDeviceErrors("create_tex2d");
		tex->Release();
		return hr;
	}

	tex->state = initState;

	if (pInit && pInit->pSysMem && !bStagingRead)
	{
		// 纹理一律 DEFAULT 堆：走一次性上传通道（内部完成 COPY_DEST↔ALL_SHADER_RESOURCE 转换）
		dx12::UploadTextureSubresource(tex->resource.Get(), 0, pInit->pSysMem, pInit->SysMemPitch,
			pDesc->Width, pDesc->Height, 1, pDesc->Format, pDesc->Format);
		tex->state = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
	}

	R5_TrackResource(tex->resource.Get(), tex->state);
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
	heap.Type = D3D12_HEAP_TYPE_DEFAULT;

	const UINT depth = pDesc->Depth ? pDesc->Depth : 1;

	D3D12_RESOURCE_DESC rd = {};
	rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
	rd.Width = pDesc->Width;
	rd.Height = pDesc->Height;
	rd.DepthOrArraySize = (UINT16)depth;
	rd.MipLevels = (UINT16)(pDesc->MipLevels ? pDesc->MipLevels : 1);
	rd.Format = pDesc->Format;
	rd.SampleDesc.Count = 1;
	rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

	const D3D12_RESOURCE_STATES initState = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;

	HRESULT hr = dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rd,
		initState, nullptr, IID_PPV_ARGS(&tex->resource));
	if (FAILED(hr)) { Msg("! DX12: CreateTexture3D failed 0x%08x", hr); tex->Release(); return hr; }

	tex->desc.Width = pDesc->Width;
	tex->desc.Height = pDesc->Height;
	tex->desc.Format = pDesc->Format;
	tex->desc.MipLevels = rd.MipLevels;
	tex->state = initState;

	if (pInit && pInit->pSysMem)
	{
		dx12::UploadTextureSubresource(tex->resource.Get(), 0, pInit->pSysMem, pInit->SysMemPitch,
			pDesc->Width, pDesc->Height, depth, pDesc->Format, pDesc->Format);
		tex->state = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
	}

	R5_TrackResource(tex->resource.Get(), tex->state);
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
		// D3D11 语义：SRV desc 的 Format=UNKNOWN 表示继承资源格式。
		// D3D12 不会自动继承（纹理 SRV 传 UNKNOWN 会创建无效视图，
		// NVIDIA 驱动对部分格式容忍、对 B8G8R8A8/R8G8B8A8 不宽容 → 字体白块/theora 黑屏）。
		d.Format = pDesc->Format;
		if (d.Format == DXGI_FORMAT_UNKNOWN && pResource->resource)
			d.Format = pResource->resource->GetDesc().Format;
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
		case D3D12_SRV_DIMENSION_TEXTURE3D:
			d.Texture3D.MostDetailedMip = pDesc->Texture3D.MostDetailedMip;
			d.Texture3D.MipLevels = pDesc->Texture3D.MipLevels;
			break;
		case D3D12_SRV_DIMENSION_TEXTURE2DARRAY:
			d.Texture2DArray.MostDetailedMip = pDesc->Texture2DArray.MostDetailedMip;
			d.Texture2DArray.MipLevels = pDesc->Texture2DArray.MipLevels;
			d.Texture2DArray.FirstArraySlice = pDesc->Texture2DArray.FirstArraySlice;
			d.Texture2DArray.ArraySize = pDesc->Texture2DArray.ArraySize;
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
		else if (rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D)
		{
			d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
			d.Texture3D.MostDetailedMip = 0;
			d.Texture3D.MipLevels = rd.MipLevels;
		}
		else
		{
			// 默认视图语义：2D 纹理若含多个 array slice，默认覆盖整个数组
			if (rd.DepthOrArraySize > 1)
			{
				d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
				d.Texture2DArray.MostDetailedMip = 0;
				d.Texture2DArray.MipLevels = rd.MipLevels;
				d.Texture2DArray.FirstArraySlice = 0;
				d.Texture2DArray.ArraySize = rd.DepthOrArraySize;
			}
			else
			{
				d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
				d.Texture2D.MostDetailedMip = 0;
				d.Texture2D.MipLevels = rd.MipLevels;
			}
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
		d.Texture2D.MipSlice = pDesc->Texture2D.MipSlice;
		d.Texture2D.PlaneSlice = 0;
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
		d.Texture2D.MipSlice = pDesc->Texture2D.MipSlice;
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

HRESULT dx12Device::CheckFormatSupport(DXGI_FORMAT Format, UINT* pSupport)
{
	if (!pSupport) return E_INVALIDARG;
	D3D12_FEATURE_DATA_FORMAT_SUPPORT fs = {};
	fs.Format = Format;
	ID3D12Device* dev = Get();
	if (!dev) return E_FAIL;
	if (FAILED(dev->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof(fs))))
	{
		*pSupport = 0;
		return E_FAIL;
	}
	*pSupport = fs.Support1;
	return S_OK;
}

//------------------------------------------------------------------------------
// dx12Context
//------------------------------------------------------------------------------
HRESULT dx12Context::Map(ID3DResource* pResource, UINT, D3D_MAP, UINT, D3D_MAPPED_TEXTURE2D* pMapped)
{
	if (!pResource || !pMapped) return E_INVALIDARG;
	pMapped->pData = nullptr;
	pMapped->RowPitch = pMapped->DepthPitch = 0;

	// STAGING+READ 纹理：内部拷回 READBACK 缓冲后返回映射指针；
	// 普通默认堆纹理的 CPU 写入（Theora/AVI，WRITE_DISCARD）：从上传环暂存，Unmap 时拷贝回默认堆。
	if (auto* t = dynamic_cast<dx12Texture*>(pResource))
	{
		if (t->stagingRead && t->resource)
		{
			UINT pitch = 0;
			void* p = dx12::ReadbackTextureSubresource(t->resource.Get(), 0,
				t->desc.Width, t->desc.Height, t->desc.Format, pitch);
			pMapped->pData = p;
			pMapped->RowPitch = pitch;
			pMapped->DepthPitch = pitch * t->desc.Height;
			return p ? S_OK : E_FAIL;
		}
		else if (t->resource)
		{
			const D3D12_RESOURCE_DESC td = t->resource->GetDesc();
			D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
			UINT numRows = 0;
			UINT64 rowSize = 0, totalBytes = 0;
			dx12::GetD3D12Device()->GetCopyableFootprints(&td, 0, 1, 0, &fp, &numRows, &rowSize, &totalBytes);

			ID3D12Resource* ringRes = dx12::g_backend.ring.Resource();
			D3D12_GPU_VIRTUAL_ADDRESS ringStartVa = ringRes->GetGPUVirtualAddress();
			D3D12_GPU_VIRTUAL_ADDRESS baseGpu = 0;
			void* baseCpu = dx12::g_backend.ring.Alloc(totalBytes,
				D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, baseGpu);
			if (!baseCpu) return E_OUTOFMEMORY;

			const UINT64 fpOffset0 = fp.Offset;
			pMapped->pData = (u8*)baseCpu + fpOffset0;
			pMapped->RowPitch = (UINT)fp.Footprint.RowPitch;
			pMapped->DepthPitch = (UINT)(fp.Footprint.RowPitch * numRows);

			// 拷贝源用 ring 缓冲，须把已分配的 ring 偏移并入 footprint.Offset
			fp.Offset = (UINT64)(baseGpu - ringStartVa) + fpOffset0;
			dynTex = pResource;
			dynFootprint = fp;
			dynUploadGpuVA = baseGpu;
			return S_OK;
		}
	}

	if (auto* b = dynamic_cast<dx12Buffer*>(pResource))
	{
		pMapped->pData = b->mapped;
		pMapped->RowPitch = b->size;
		pMapped->DepthPitch = b->size;
		return S_OK;
	}

	if (pResource->resource)
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
	if (auto* t = dynamic_cast<dx12Texture*>(pResource))
	{
		if (t->stagingRead) return;	// 读回缓冲由后端持有，这里不解除

		// 动态写入：把上传环暂存数据拷回默认堆纹理
		if (dynTex == pResource && t->resource)
		{
			ID3D12GraphicsCommandList* cl = Get();
			if (cl)
			{
				static bool s_theoraDiagOnce = false;	// [theoradiag] 临时诊断
				if (!s_theoraDiagOnce)
				{
					s_theoraDiagOnce = true;
					Msg("* [theoradiag] Unmap copy on main cl: %ux%u fmt=%d ringOff=%llu",
						t->desc.Width, t->desc.Height, (int)t->desc.Format,
						(unsigned long long)(dynFootprint.Offset));
				}
				// 用纹理真实状态做转换：SRV 纹理是 ALL_SHADER_RESOURCE，
				// 之前写死 GENERIC_READ 属非法 barrier，驱动可导致 device removed
				const D3D12_RESOURCE_STATES cur = t->state;
				D3D12_RESOURCE_BARRIER b = {};
				b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
				b.Transition.pResource = t->resource.Get();
				b.Transition.Subresource = 0;
				b.Transition.StateBefore = cur;
				b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
				cl->ResourceBarrier(1, &b);

				D3D12_TEXTURE_COPY_LOCATION src = {};
				src.pResource = dx12::g_backend.ring.Resource();
				src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
				src.PlacedFootprint = dynFootprint;

				D3D12_TEXTURE_COPY_LOCATION dst = {};
				dst.pResource = t->resource.Get();
				dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
				dst.SubresourceIndex = 0;

				cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

				std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
				cl->ResourceBarrier(1, &b);
			}
			dynTex = nullptr;
			return;
		}
	}
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
	if (!dx12::g_backend.frameActive)
	{
		// 设备初始化期（主命令列表未打开）：立即通道，避免 closed-list 调用
		dx12::ClearRTVImmediate(pRTV->cpu, color);
		return;
	}
	ID3D12GraphicsCommandList* cl = Get();
	if (cl)
	{
		auto* rv = dynamic_cast<dx12RenderTargetView*>(pRTV);
		if (rv && rv->resource)
			R5_TransitionResource(cl, rv->resource.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
		cl->ClearRenderTargetView(pRTV->cpu, color, 0, nullptr);
	}
}

void dx12Context::ClearDepthStencilView(ID3DDepthStencilView* pDSV, UINT flags, FLOAT depth, UINT8 stencil)
{
	if (!pDSV || !pDSV->valid) return;
	ID3D12GraphicsCommandList* cl = Get();
	if (cl)
	{
		auto* dv = dynamic_cast<dx12DepthStencilView*>(pDSV);
		if (dv && dv->resource)
			R5_TransitionResource(cl, dv->resource.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE);
		cl->ClearDepthStencilView(pDSV->cpu, (D3D12_CLEAR_FLAGS)flags, depth, stencil, 0, nullptr);
	}
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
	LONG scW = 0, scH = 0;
	for (UINT i = 0; i < num; ++i)
	{
		if (ppRTV[i] && ppRTV[i]->valid)
		{
			handles[n++] = ppRTV[i]->cpu;
			auto* rv = dynamic_cast<dx12RenderTargetView*>(ppRTV[i]);
			if (rv && rv->resource)
			{
				R5_TransitionResource(cl, rv->resource.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
				if (scW == 0)
				{
					D3D12_RESOURCE_DESC rd0 = rv->resource->GetDesc();
					scW = (LONG)rd0.Width;
					scH = (LONG)rd0.Height;
				}
			}
		}
	}
	D3D12_CPU_DESCRIPTOR_HANDLE dsv = (pDSV && pDSV->valid) ? pDSV->cpu : D3D12_CPU_DESCRIPTOR_HANDLE{ 0 };
	if (pDSV && pDSV->valid)
	{
		auto* dv = dynamic_cast<dx12DepthStencilView*>(pDSV);
		if (dv && dv->resource)
			R5_TransitionResource(cl, dv->resource.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE);
	}
	cl->OMSetRenderTargets(n, n ? handles : nullptr, FALSE, (pDSV && pDSV->valid) ? &dsv : nullptr);

	// 模拟 D3D11 默认 ScissorEnable=FALSE（不裁剪）：绑定 RT 后给一个覆盖该 RT 的
	// 默认全屏 scissor。命令分配器每帧重置后 scissor 为空，而 D3D12 scissor test
	// 始终开启——若不补，从未显式 set_Scissor 的 Draw 会被空矩形裁掉全部像素。
	// 引擎需要裁剪时会在 Draw 前调用 set_Scissor(R) 覆盖此默认值。
	if (scW > 0)
	{
		D3D12_RECT fullR = { 0, 0, scW, scH };
		cl->RSSetScissorRects(1, &fullR);
	}
}

void dx12Context::BindBackbufferRTV(D3D12_CPU_DESCRIPTOR_HANDLE rtv, UINT width, UINT height)
{
	ID3D12GraphicsCommandList* cl = Get();
	if (!cl || !width || !height) return;

	D3D12_VIEWPORT vp = { 0.f, 0.f, float(width), float(height), 0.f, 1.f };
	D3D12_RECT	 sr = { 0, 0, LONG(width), LONG(height) };
	cl->RSSetViewports(1, &vp);
	cl->RSSetScissorRects(1, &sr);
	cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
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
// 调试层在参数错误时会同步 RaiseException 触发断点且不自行恢复；
// 用 SEH 包住，使调用无论返回错误还是抛异常都能继续读取 InfoQueue。
static HRESULT SafeCreateGraphicsPSO(ID3D12Device* dev,
	const D3D12_GRAPHICS_PIPELINE_STATE_DESC* d, ID3D12PipelineState** out)
{
	__try
	{
		return dev->CreateGraphicsPipelineState(d, IID_PPV_ARGS(out));
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return E_FAIL;
	}
}

void dx12Context::FlushPipeline()
{
	ID3D12GraphicsCommandList* cl = Get();
	if (!cl) return;
	dx12::Ensure();
	if (!dx12::g_backend.valid) return;

	// [fontbind] Draw 时 slot0 SRV 身份/描述诊断（身份变化或每500 draw，上限80条）
	{
		static ID3DShaderResourceView* s_lastS0 = reinterpret_cast<ID3DShaderResourceView*>(1);
		static int s_logCount = 0;
		static int s_drawCount = 0;
		++s_drawCount;
		ID3DShaderResourceView* s0 = SRVSManager.GetPS(0);
		if (((s0 != s_lastS0) || (s_drawCount % 500) == 0) && s_logCount < 80)
		{
			s_lastS0 = s0;
			++s_logCount;
			auto* dv = dynamic_cast<dx12ShaderResourceView*>(s0);
			ID3D12Resource* rr0 = (dv && dv->resource) ? dv->resource.Get() : nullptr;
			const auto& bd0 = StateManager.m_BDesc.RenderTarget[0];
			Msg("* [fontbind] draw#%d vs=%p ps=%p s0=%p valid=%d cpu=%llu fmt=%d dim=%d res=%p matchSrv=%d matchRes=%d blend[en=%d src=%d dst=%d] zEn=%d zWr=%d cull=%d | atlas '%s' %ux%u srv=%p res=%p",
				s_drawCount, vs, ps, s0, dv ? (int)dv->valid : -1,
				dv ? (unsigned long long)dv->cpu.ptr : 0ull,
				dv ? (int)dv->desc.Format : -1,
				dv ? (int)dv->desc.ViewDimension : -1,
				rr0,
				(int)(s0 == g_r5LastAtlasSrv),
				(int)(rr0 == g_r5LastAtlasRes),
				(int)bd0.BlendEnable, (int)bd0.SrcBlend, (int)bd0.DestBlend,
				(int)StateManager.m_DSDesc.DepthEnable,
				(int)StateManager.m_DSDesc.DepthWriteMask,
				(int)StateManager.m_RDesc.CullMode,
				g_r5LastAtlasName[0] ? g_r5LastAtlasName : "?",
				g_r5LastAtlasW, g_r5LastAtlasH, g_r5LastAtlasSrv, g_r5LastAtlasRes);
			char slotMap[128]; slotMap[0] = 0; int used = 0;
			for (UINT q = 0; q < 16 && used < 110; ++q)
			{
				if (SRVSManager.GetPS(q))
					used += xr_sprintf(slotMap + used, sizeof(slotMap) - used, "%u ", q);
			}
			Msg("* [fontbind]   non-null PS slots: [%s]", slotMap);
		}
	}

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

		ID3D12PipelineState* rawPso = nullptr;
		HRESULT hr = SafeCreateGraphicsPSO(dx12::GetD3D12Device(), &d, &rawPso);
		if (FAILED(hr))
		{
			Msg("! DX12: CreateGraphicsPipelineState failed 0x%08x (vs=%p ps=%p lay=%p)", hr, vs, ps, layout);
			Msg("    PSO rtCount=%u NumRT=%u topoType=%d topo=%d DSV=%d RTV=[%d,%d,%d,%d] vsLen=%u psLen=%u layElems=%u",
				rtCount, d.NumRenderTargets, (int)d.PrimitiveTopologyType, (int)topology, (int)d.DSVFormat,
				(int)d.RTVFormats[0], (int)d.RTVFormats[1], (int)d.RTVFormats[2], (int)d.RTVFormats[3],
				(unsigned)d.VS.BytecodeLength, (unsigned)d.PS.BytecodeLength,
				layout ? layout->Desc().NumElements : 0u);
			dx12::DumpDeviceErrors("pso");
			if (rawPso) rawPso->Release();
			return;
		}
		ComPtr<ID3D12PipelineState> newPso = rawPso;
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
				auto* dv = dynamic_cast<dx12ShaderResourceView*>(srv);
				if (dv && dv->resource)
					R5_TransitionResource(cl, dv->resource.Get(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
				dx12::GetD3D12Device()->CopyDescriptorsSimple(1, dst, srv->cpu, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
			}
			else if (dx12::g_backend.dummySrvCpu.ptr)
			{
				// 填充无效槽：防止描述符堆残留上一 Draw 的纹理（避免 shniaga 采样错纹理）
				dx12::GetD3D12Device()->CopyDescriptorsSimple(1, dst, dx12::g_backend.dummySrvCpu, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
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
