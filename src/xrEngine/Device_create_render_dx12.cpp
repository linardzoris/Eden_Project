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
	ComPtr<ID3D12DescriptorHeap>			DSVHeap;
	ComPtr<ID3D12Resource>					DepthBuffer;
	UINT									RTVDescriptorSize = 0;
	UINT									DSVDescriptorSize = 0;
	UINT									FrameIndex = 0;

	ComPtr<ID3D12Fence>						Fence;
	HANDLE									FenceEvent = nullptr;
	UINT64									FenceValue[NUM_BACKBUFFERS] = {};
	bool									FrameInFlight = false;

	const char* VendorLabel(UINT vid)
	{
		switch (vid)
		{
		case 0x10DE: return "NVIDIA";
		case 0x8086: return "Intel";
		case 0x1002: return "AMD";
		case 0x1414: return "Microsoft";
		default:      return "Other";
		}
	}

	void LogAdapter(const char* tag, const DXGI_ADAPTER_DESC1& d)
	{
		char name[128] = {};
		::WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, (int)sizeof(name), nullptr, nullptr);
		const u32 vidMB = (u32)(d.DedicatedVideoMemory / (1024ull * 1024ull));
		Msg("* DX12: %s '%s' %s ven=0x%04x dev=0x%04x vidmem=%u MB",
			tag, name, VendorLabel(d.VendorId), d.VendorId, d.DeviceId, vidMB);
	}

	bool CreateDeviceAndQueue()
	{
		// 调试层：仅在 -dxdebug 时开启（强制开启会导致跨适配器呈现路径异常）
		const bool dxdebug = Core.ParamsData.test(ECoreParams::dxdebug);
		if (dxdebug)
		{
			ComPtr<ID3D12Debug> dbg;
			HRESULT hrDbg = D3D12GetDebugInterface(IID_PPV_ARGS(&dbg));
			if (SUCCEEDED(hrDbg) && dbg)
			{
				dbg->EnableDebugLayer();

				// GPU-Based Validation：在 GPU 上检测非法操作（失效描述符、越界访问、
				// 资源状态错误等），立即产出明确错误，而不是等到 TDR 触发 DEVICE_HUNG。
				// 必须在 D3D12CreateDevice 之前设置。
				ComPtr<ID3D12Debug1> dbg1;
				if (SUCCEEDED(dbg.As(&dbg1)) && dbg1)
				{
					dbg1->SetEnableGPUBasedValidation(TRUE);
					dbg1->SetEnableSynchronizedCommandQueueValidation(TRUE);
				}
				Msg("* DX12: debug layer enabled (GPU-based validation on)");
			}
			else
			{
				Msg("! DX12: debug layer unavailable 0x%08x", hrDbg);
			}
		}

		ComPtr<IDXGIFactory6> factory;
		HRESULT hr = CreateDXGIFactory2(
			dxdebug ? DXGI_CREATE_FACTORY_DEBUG : 0,
			IID_PPV_ARGS(&factory)
		);
		if (FAILED(hr))
		{
			Msg("! DX12: CreateDXGIFactory2 failed 0x%08x", hr);
			return false;
		}

		// 记录所有硬件适配器（跳过软件/WARP）
		for (UINT i = 0; ; ++i)
		{
			ComPtr<IDXGIAdapter1> a;
			if (factory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND)
				break;
			DXGI_ADAPTER_DESC1 d{};
			a->GetDesc1(&d);
			if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
				continue;
			LogAdapter("adapter", d);
		}

		ComPtr<IDXGIAdapter1> adapter;

		// 混合显卡笔记本：优先高性能独显（NVIDIA RTX 4060），避免误选 Intel 核显。
		// 旧版 Intel UMD 在 IGC 编译 / 多线程 DDI 路径会抛内部异常（msg_end）并崩溃。
		hr = factory->EnumAdapterByGpuPreference(
			0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
		if (SUCCEEDED(hr) && adapter)
		{
			DXGI_ADAPTER_DESC1 d{};
			adapter->GetDesc1(&d);
			if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
				adapter.Reset();
			else
				LogAdapter("prefer-high-performance", d);
		}

		auto tryCreateDevice = [&](IDXGIAdapter1* a) -> bool
		{
			const HRESULT hc = D3D12CreateDevice(a, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&D3DDevice));
			return SUCCEEDED(hc) && D3DDevice;
		};

		if (adapter)
			tryCreateDevice(adapter.Get());

		// 回退：按枚举顺序选第一个能成功创建 D3D12 设备的硬件适配器
		if (!D3DDevice)
		{
			adapter.Reset();
			for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
			{
				DXGI_ADAPTER_DESC1 d{};
				adapter->GetDesc1(&d);
				if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
					continue;
				if (tryCreateDevice(adapter.Get()))
					break;
			}
		}

		if (!D3DDevice)
		{
			Msg("! DX12: D3D12CreateDevice failed on all adapters");
			return false;
		}

		// 配置 InfoQueue：存储损坏/错误/警告（含 GBV 产出），不限制条数，
		// 由 DrainInfoQueue() 每帧转储到游戏日志。
		if (dxdebug)
		{
			ComPtr<ID3D12InfoQueue> iq;
			if (SUCCEEDED(D3DDevice->QueryInterface(IID_PPV_ARGS(&iq))) && iq)
			{
				iq->SetMessageCountLimit(-1);

				static D3D12_MESSAGE_SEVERITY sevs[] = {
					D3D12_MESSAGE_SEVERITY_CORRUPTION,
					D3D12_MESSAGE_SEVERITY_ERROR,
					D3D12_MESSAGE_SEVERITY_WARNING
				};
				D3D12_INFO_QUEUE_FILTER filter = {};
				filter.AllowList.NumSeverities = _countof(sevs);
				filter.AllowList.pSeverityList = sevs;
				iq->AddStorageFilterEntries(&filter);
				iq->SetMuteDebugOutput(FALSE);
			}
		}

		FeatureLevel = D3D_FEATURE_LEVEL_11_0;
		{
			DXGI_ADAPTER_DESC1 chosen{};
			adapter->GetDesc1(&chosen);
			LogAdapter("selected", chosen);
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
		sd.Format = DXGI_FORMAT_R10G10B10A2_UNORM;	// 与 r4 rt_BackbufferLUT 一致，避免 gamma 输出 PSO/RTV 格式不匹配
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

		// 深度缓冲 + DSV 堆
		D3D12_DESCRIPTOR_HEAP_DESC dsvHd = {};
		dsvHd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
		dsvHd.NumDescriptors = 1;
		hr = D3DDevice->CreateDescriptorHeap(&dsvHd, IID_PPV_ARGS(&DSVHeap));
		if (FAILED(hr))
			return false;

		DSVDescriptorSize = D3DDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

		D3D12_CLEAR_VALUE clearDepth = {};
		clearDepth.Format = DXGI_FORMAT_D32_FLOAT;
		clearDepth.DepthStencil.Depth = 1.0f;
		clearDepth.DepthStencil.Stencil = 0;

		D3D12_HEAP_PROPERTIES heapProps = {};
		heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

		D3D12_RESOURCE_DESC depthDesc = {};
		depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		depthDesc.Width = width;
		depthDesc.Height = height;
		depthDesc.DepthOrArraySize = 1;
		depthDesc.MipLevels = 1;
		depthDesc.Format = DXGI_FORMAT_D32_FLOAT;
		depthDesc.SampleDesc.Count = 1;
		depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

		hr = D3DDevice->CreateCommittedResource(
			&heapProps,
			D3D12_HEAP_FLAG_NONE,
			&depthDesc,
			D3D12_RESOURCE_STATE_DEPTH_WRITE,
			&clearDepth,
			IID_PPV_ARGS(&DepthBuffer)
		);
		if (FAILED(hr))
			return false;

		D3DDevice->CreateDepthStencilView(DepthBuffer.Get(), nullptr, DSVHeap->GetCPUDescriptorHandleForHeapStart());

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

		DepthBuffer.Reset();
		DSVHeap.Reset();
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
	Msg("* DX12: CreateD3D12 entry");
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

	// 重建深度缓冲
	dx12::DepthBuffer.Reset();

	D3D12_CLEAR_VALUE clearDepth = {};
	clearDepth.Format = DXGI_FORMAT_D32_FLOAT;
	clearDepth.DepthStencil.Depth = 1.0f;

	D3D12_HEAP_PROPERTIES heapProps = {};
	heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

	D3D12_RESOURCE_DESC depthDesc = {};
	depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	depthDesc.Width = Width;
	depthDesc.Height = Height;
	depthDesc.DepthOrArraySize = 1;
	depthDesc.MipLevels = 1;
	depthDesc.Format = DXGI_FORMAT_D32_FLOAT;
	depthDesc.SampleDesc.Count = 1;
	depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

	dx12::D3DDevice->CreateCommittedResource(
		&heapProps,
		D3D12_HEAP_FLAG_NONE,
		&depthDesc,
		D3D12_RESOURCE_STATE_DEPTH_WRITE,
		&clearDepth,
		IID_PPV_ARGS(&dx12::DepthBuffer)
	);

	dx12::D3DDevice->CreateDepthStencilView(dx12::DepthBuffer.Get(), nullptr, dx12::DSVHeap->GetCPUDescriptorHandleForHeapStart());

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
// M1 帧管理 + R5 模块渲染接口
// ---------------------------------------------------------------------------

namespace dx12
{
	D3D12_CPU_DESCRIPTOR_HANDLE CurrentDSV()
	{
		return DSVHeap->GetCPUDescriptorHandleForHeapStart();
	}

	// 把 InfoQueue 中缓存的调试消息（含 GBV 检测到的 GPU 非法操作）转储到游戏日志
	void DrainInfoQueue()
	{
		if (!D3DDevice) return;

		ComPtr<ID3D12InfoQueue> iq;
		if (FAILED(D3DDevice->QueryInterface(IID_PPV_ARGS(&iq))) || !iq) return;

		const UINT64 n = iq->GetNumStoredMessages();
		for (UINT64 i = 0; i < n; ++i)
		{
			SIZE_T len = 0;
			if (FAILED(iq->GetMessage(i, nullptr, &len)) || !len) continue;

			xr_vector<u8> buf(len);
			D3D12_MESSAGE* m = (D3D12_MESSAGE*)buf.data();
			if (SUCCEEDED(iq->GetMessage(i, m, &len)) && m->pDescription)
			{
				const char* tag = "warn";
				switch (m->Severity)
				{
				case D3D12_MESSAGE_SEVERITY_CORRUPTION: tag = "CORRUPT"; break;
				case D3D12_MESSAGE_SEVERITY_ERROR:      tag = "GBV-ERR"; break;
				case D3D12_MESSAGE_SEVERITY_WARNING:    tag = "warn"; break;
				default: break;
				}
				Msg("! DX12 [%s] f%u: %s", tag, Device.dwFrame, m->pDescription);
			}
		}
		if (n) iq->ClearStoredMessages();
	}

	// 开始一帧：重置命令分配器/列表，转换 backbuffer 到 RT 状态
	extern "C" ENGINE_API void BeginFrame()
	{
		DrainInfoQueue();

		if (FrameInFlight)
			return;

		CmdAlloc[FrameIndex]->Reset();
		CmdList->Reset(CmdAlloc[FrameIndex].Get(), nullptr);

		D3D12_RESOURCE_BARRIER toRT = {};
		toRT.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		toRT.Transition.pResource = BackBuffer[FrameIndex].Get();
		toRT.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
		toRT.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
		toRT.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		CmdList->ResourceBarrier(1, &toRT);

		// D3D12 scissor test 始终开启，命令列表 reset 后 scissor 为空；而 D3D11 默认
		// ScissorEnable=FALSE（不裁剪）本应无需调用者干预。每帧先绑定一个最大范围
		// scissor 作为默认"不裁剪"状态，引擎需要裁剪时再经 set_Scissor(R) 覆盖。
		// 这样即便在未绑定 RT 的早期 Draw 上也不会被空矩形裁掉像素。
		static const D3D12_RECT defaultScissor = { 0, 0, 16384, 16384 };
		CmdList->RSSetScissorRects(1, &defaultScissor);

		FrameInFlight = true;
	}

	// 结束一帧：转换到 PRESENT，关闭并执行命令列表
	extern "C" ENGINE_API void EndFrame()
	{
		if (!FrameInFlight)
			return;

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

		// GBV 的部分错误在 ExecuteCommandLists 后同步产出，立即转储，
		// 确保即便本帧随后崩溃也不会丢失关键信息。
		DrainInfoQueue();

		FrameInFlight = false;
	}

	extern "C" ENGINE_API void FramePresent(bool vsync)
	{
		Swapchain->Present(vsync ? 1 : 0, 0);
		MoveToNextFrame();
	}

	// R5 模块资源访问
	extern "C" ENGINE_API ID3D12Device* GetDevice() { return D3DDevice.Get(); }
	extern "C" ENGINE_API ID3D12CommandQueue* GetCommandQueue() { return CmdQueue.Get(); }
	extern "C" ENGINE_API ID3D12GraphicsCommandList* GetCmdList() { return CmdList.Get(); }
	extern "C" ENGINE_API D3D12_CPU_DESCRIPTOR_HANDLE GetCurrentRTV() { return CurrentRTV(); }
	extern "C" ENGINE_API D3D12_CPU_DESCRIPTOR_HANDLE GetCurrentDSV() { return CurrentDSV(); }
	// 交换链 backbuffer 格式（R10G10B10A2_UNORM）：供 R5 在裸 RTV 绑定时合成匹配的 PSO
	extern "C" ENGINE_API int GetBackbufferFormat() { return (int)DXGI_FORMAT_R10G10B10A2_UNORM; }
	// 渲染层在创建主深度（rt_Position）后回填其 DSV 包装对象指针，
	// 使 Device.GetDepthTexture() / 全局 RDepth 宏指向有效的深度视图。
	extern "C" ENGINE_API void SetRenderDSV(void* dsv) { RenderDSV = dsv; }
	extern "C" ENGINE_API ID3D12Resource* GetCurrentBackBuffer() { return BackBuffer[FrameIndex].Get(); }
	extern "C" ENGINE_API UINT GetFrameIndex() { return FrameIndex; }
	extern "C" ENGINE_API UINT GetRTVDescriptorSize() { return RTVDescriptorSize; }
}
