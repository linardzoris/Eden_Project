#include "stdafx.h"
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <renderdoc/api/app/renderdoc_app.h>

using Microsoft::WRL::ComPtr;

extern D3D_FEATURE_LEVEL FeatureLevel;
extern void* HWSwapchain;

extern void* HWRenderDevice;
extern void* HWRenderContext;

extern void* RenderTexture;
extern void* RenderSRV;
extern void* RenderRTV;

extern void* RenderDSV;
extern void* SwapChainRTV;

// ---------------------------------------------------------------------------
// DX12 私有上下文：M0 骨架只包含设备创建、命令队列、双缓冲 swapchain、
// 每帧清屏所需的命令分配器/列表/围栏。后续里程碑逐步填充描述符堆等。
// ---------------------------------------------------------------------------
namespace dx12
{
	static constexpr u32 NUM_BACKBUFFERS = 2;

	ComPtr<ID3D12Device>					D3DDevice;
	ComPtr<ID3D12CommandQueue>				CmdQueue;
	ComPtr<ID3D12GraphicsCommandList>		CmdList;
	ComPtr<ID3D12CommandAllocator>			CmdAlloc[NUM_BACKBUFFERS];
	ComPtr<IDXGISwapChain3>					Swapchain;
	ComPtr<ID3D12Resource>					BackBuffer[NUM_BACKBUFFERS];
	ComPtr<ID3D12DescriptorHeap>			RTVHeap;
	UINT									RTVDescriptorSize = 0;
	UINT									FrameIndex = 0;

	ComPtr<ID3D12Fence>						Fence;
	HANDLE									FenceEvent = nullptr;
	UINT64									FenceValue[NUM_BACKBUFFERS] = {};

	bool CreateDeviceAndQueue()
	{
		// 调试层
		if (Core.ParamsData.test(ECoreParams::dxdebug))
		{
			ComPtr<ID3D12Debug> dbg;
			if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg))))
			{
				dbg->EnableDebugLayer();
			}
		}

		ComPtr<IDXGIFactory6> factory;
		HRESULT hr = CreateDXGIFactory2(
			Core.ParamsData.test(ECoreParams::dxdebug) ? DXGI_CREATE_FACTORY_DEBUG : 0,
			IID_PPV_ARGS(&factory)
		);
		if (FAILED(hr))
		{
			Msg("! DX12: CreateDXGIFactory2 failed 0x%08x", hr);
			return false;
		}

		ComPtr<IDXGIAdapter1> adapter;
		for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
		{
			DXGI_ADAPTER_DESC1 desc;
			adapter->GetDesc1(&desc);
			if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
				continue;

			hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&D3DDevice));
			if (SUCCEEDED(hr))
			{
				FeatureLevel = D3D_FEATURE_LEVEL_11_0;
				break;
			}
		}

		if (!D3DDevice)
		{
			Msg("! DX12: D3D12CreateDevice failed");
			return false;
		}

		D3D12_COMMAND_QUEUE_DESC qd = {};
		qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		hr = D3DDevice->CreateCommandQueue(&qd, IID_PPV_ARGS(&CmdQueue));
		if (FAILED(hr))
		{
			Msg("! DX12: CreateCommandQueue failed 0x%08x", hr);
			return false;
		}

		return true;
	}

	bool CreateSwapchainAndRTVs(HWND hwnd, u32 width, u32 height)
	{
		ComPtr<IDXGIFactory6> factory;
		HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
		if (FAILED(hr))
			return false;

		DXGI_SWAP_CHAIN_DESC1 sd = {};
		sd.BufferCount = NUM_BACKBUFFERS;
		sd.Width = width;
		sd.Height = height;
		sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
		sd.SampleDesc.Count = 1;

		DXGI_SWAP_CHAIN_FULLSCREEN_DESC fsd = {};
		fsd.Windowed = !psDeviceFlags.is(rsFullscreen);

		ComPtr<IDXGISwapChain1> sc1;
		hr = factory->CreateSwapChainForHwnd(CmdQueue.Get(), hwnd, &sd, &fsd, nullptr, &sc1);
		if (FAILED(hr))
		{
			Msg("! DX12: CreateSwapChainForHwnd failed 0x%08x", hr);
			return false;
		}

		hr = sc1->QueryInterface(IID_PPV_ARGS(&Swapchain));
		if (FAILED(hr))
			return false;

		FrameIndex = Swapchain->GetCurrentBackBufferIndex();

		// RTV heap
		D3D12_DESCRIPTOR_HEAP_DESC hd = {};
		hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		hd.NumDescriptors = NUM_BACKBUFFERS;
		hr = D3DDevice->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&RTVHeap));
		if (FAILED(hr))
		{
			Msg("! DX12: CreateDescriptorHeap(RTV) failed 0x%08x", hr);
			return false;
		}

		RTVDescriptorSize = D3DDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

		D3D12_CPU_DESCRIPTOR_HANDLE h = RTVHeap->GetCPUDescriptorHandleForHeapStart();
		for (u32 i = 0; i < NUM_BACKBUFFERS; ++i)
		{
			hr = Swapchain->GetBuffer(i, IID_PPV_ARGS(&BackBuffer[i]));
			if (FAILED(hr))
				return false;

			D3DDevice->CreateRenderTargetView(BackBuffer[i].Get(), nullptr, h);
			h.ptr += RTVDescriptorSize;
		}

		return true;
	}

	bool CreateCommandObjects()
	{
		HRESULT hr;
		for (u32 i = 0; i < NUM_BACKBUFFERS; ++i)
		{
			hr = D3DDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&CmdAlloc[i]));
			if (FAILED(hr))
				return false;
		}

		hr = D3DDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, CmdAlloc[0].Get(), nullptr, IID_PPV_ARGS(&CmdList));
		if (FAILED(hr))
			return false;

		CmdList->Close();
		return true;
	}

	bool CreateFence()
	{
		HRESULT hr = D3DDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&Fence));
		if (FAILED(hr))
			return false;

		FenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		return FenceEvent != nullptr;
	}

	void WaitForGpu()
	{
		const UINT64 v = FenceValue[FrameIndex];
		CmdQueue->Signal(Fence.Get(), v);
		if (Fence->GetCompletedValue() < v)
		{
			Fence->SetEventOnCompletion(v, FenceEvent);
			WaitForSingleObject(FenceEvent, INFINITE);
		}
	}

	void MoveToNextFrame()
	{
		const UINT64 current = FenceValue[FrameIndex];
		CmdQueue->Signal(Fence.Get(), current);

		FrameIndex = Swapchain->GetCurrentBackBufferIndex();

		if (Fence->GetCompletedValue() < FenceValue[FrameIndex])
		{
			Fence->SetEventOnCompletion(FenceValue[FrameIndex], FenceEvent);
			WaitForSingleObject(FenceEvent, INFINITE);
		}

		FenceValue[FrameIndex] = current + 1;
	}

	D3D12_CPU_DESCRIPTOR_HANDLE CurrentRTV()
	{
		D3D12_CPU_DESCRIPTOR_HANDLE h = RTVHeap->GetCPUDescriptorHandleForHeapStart();
		h.ptr += SIZE_T(FrameIndex) * RTVDescriptorSize;
		return h;
	}

	void DestroyAll()
	{
		if (FenceEvent)
		{
			CloseHandle(FenceEvent);
			FenceEvent = nullptr;
		}

		for (u32 i = 0; i < NUM_BACKBUFFERS; ++i)
		{
			BackBuffer[i].Reset();
			CmdAlloc[i].Reset();
		}

		RTVHeap.Reset();
		CmdList.Reset();
		Swapchain.Reset();
		CmdQueue.Reset();
		Fence.Reset();
		D3DDevice.Reset();
	}
}

// ---------------------------------------------------------------------------
// 引擎回调入口
// ---------------------------------------------------------------------------

bool UpdateBuffersD3D12()
{
	// M0: swapchain backbuffer RTV 已在 CreateSwapchainAndRTVs 中创建，
	// 这里仅同步引擎侧 void* 供旧路径查询。
	HWRenderDevice = dx12::D3DDevice.Get();
	HWRenderContext = dx12::CmdList.Get();
	HWSwapchain = dx12::Swapchain.Get();

	// 旧代码中 RenderTexture/RTV/SRV/DSV 均用于 11 的 backbuffer 拷贝链，
	// DX12 M0 不使用，置空即可；SwapChainRTV 存当前帧 CPU handle 的起始地址。
	SwapChainRTV = nullptr;
	RenderTexture = nullptr;
	RenderRTV = nullptr;
	RenderSRV = nullptr;
	RenderDSV = nullptr;

	Device.HalfTargetWidth = float(psCurrentVidMode[0]);
	Device.HalfTargetHeight = float(psCurrentVidMode[1]);

	return true;
}

bool CreateD3D12()
{
	HWND hwnd = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(g_AppInfo.Window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
	if (!hwnd)
	{
		Msg("! DX12: invalid HWND");
		return false;
	}

	if (!dx12::CreateDeviceAndQueue())
		return false;

	if (!dx12::CreateSwapchainAndRTVs(hwnd, psCurrentVidMode[0], psCurrentVidMode[1]))
		return false;

	if (!dx12::CreateCommandObjects())
		return false;

	if (!dx12::CreateFence())
		return false;

	if (!UpdateBuffersD3D12())
		return false;

	Msg("* DX12: device created, feature level 11.0, %ux%u", psCurrentVidMode[0], psCurrentVidMode[1]);
	return true;
}

void ResizeBuffersD3D12(u16 Width, u16 Height)
{
	if (!dx12::Swapchain)
		return;

	dx12::WaitForGpu();

	for (u32 i = 0; i < dx12::NUM_BACKBUFFERS; ++i)
	{
		dx12::BackBuffer[i].Reset();
		dx12::FenceValue[i] = dx12::FenceValue[dx12::FrameIndex];
	}

	DXGI_SWAP_CHAIN_DESC1 sd = {};
	dx12::Swapchain->GetDesc1(&sd);
	HRESULT hr = dx12::Swapchain->ResizeBuffers(dx12::NUM_BACKBUFFERS, Width, Height, sd.Format, sd.Flags);
	if (FAILED(hr))
	{
		Msg("! DX12: ResizeBuffers failed 0x%08x", hr);
		return;
	}

	dx12::FrameIndex = dx12::Swapchain->GetCurrentBackBufferIndex();

	D3D12_CPU_DESCRIPTOR_HANDLE h = dx12::RTVHeap->GetCPUDescriptorHandleForHeapStart();
	for (u32 i = 0; i < dx12::NUM_BACKBUFFERS; ++i)
	{
		dx12::Swapchain->GetBuffer(i, IID_PPV_ARGS(&dx12::BackBuffer[i]));
		dx12::D3DDevice->CreateRenderTargetView(dx12::BackBuffer[i].Get(), nullptr, h);
		h.ptr += dx12::RTVDescriptorSize;
	}

	UpdateBuffersD3D12();
}

void DestroyD3D12()
{
	if (dx12::CmdQueue && dx12::Fence && dx12::FenceEvent)
		dx12::WaitForGpu();

	dx12::DestroyAll();

	HWRenderDevice = nullptr;
	HWRenderContext = nullptr;
	HWSwapchain = nullptr;
	SwapChainRTV = nullptr;
	RenderTexture = nullptr;
	RenderRTV = nullptr;
	RenderSRV = nullptr;
	RenderDSV = nullptr;
}

// ---------------------------------------------------------------------------
// M0 最小每帧渲染：清屏 + Present。由 R5 模块的 CRender::Begin/End 调用。
// 为避免循环依赖，这里暴露给 xrRender_R5.dll 通过引擎全局指针间接调用。
// ---------------------------------------------------------------------------

namespace dx12
{
	extern "C" ENGINE_API void FrameClear(float r, float g, float b, float a)
	{
		CmdAlloc[FrameIndex]->Reset();
		CmdList->Reset(CmdAlloc[FrameIndex].Get(), nullptr);

		D3D12_RESOURCE_BARRIER toRT = {};
		toRT.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		toRT.Transition.pResource = BackBuffer[FrameIndex].Get();
		toRT.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
		toRT.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
		toRT.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		CmdList->ResourceBarrier(1, &toRT);

		D3D12_CPU_DESCRIPTOR_HANDLE rtv = CurrentRTV();
		const float color[4] = { r, g, b, a };
		CmdList->ClearRenderTargetView(rtv, color, 0, nullptr);

		D3D12_RESOURCE_BARRIER toPresent = {};
		toPresent.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		toPresent.Transition.pResource = BackBuffer[FrameIndex].Get();
		toPresent.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
		toPresent.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
		toPresent.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		CmdList->ResourceBarrier(1, &toPresent);

		CmdList->Close();
		ID3D12CommandList* lists[] = { CmdList.Get() };
		CmdQueue->ExecuteCommandLists(1, lists);
	}

	extern "C" ENGINE_API void FramePresent(bool vsync)
	{
		Swapchain->Present(vsync ? 1 : 0, 0);
		MoveToNextFrame();
	}
}
