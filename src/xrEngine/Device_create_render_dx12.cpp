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
	// 全局单调递增的提交序号 + 每个 backbuffer 上"最后一次提交"的序号。
	// 命令列表是"先录制、后由 GPU 执行"：录制下一帧时会整片重写 CPU 侧共享资源
	// （描述符堆槽、上传环段、命令分配器）。复用某个 backbuffer 的这些资源之前，
	// 必须等上一次使用它的那一帧真正执行完，否则 GPU 读到的是下一帧的数据 →
	// 关掉 -dxdebug（CPU/GPU 重叠更深）后出现高速花屏/闪烁。
	UINT64									SubmitSeq = 0;
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

		// DRED：设备挂起（DEVICE_HUNG / TDR）时记录"最后执行的命令序号"与页错误分配，
	// 是唯一能在不开 GBV 的情况下指出"哪个命令/哪块内存出事"的手段。
	// 只需 D3D12GetDebugInterface（不需要开启调试层），必须在 D3D12CreateDevice 之前设置。
	{
		ComPtr<ID3D12DeviceRemovedExtendedDataSettings> dredSettings;
		if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dredSettings))) && dredSettings)
		{
			dredSettings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
			dredSettings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
			Msg("* DX12: DRED enabled (auto breadcrumbs + page fault)");
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
		// 允许撕裂：关闭 vsync 时 Present(SyncInterval=0) 需要它（窗口化下才支持）。
		// FLIP_DISCARD 的翻转必须由显示侧在 vblank 退休；若显示侧不推进，GPU 会停在
		// driver 追加的翻转等待上、围栏永不 signal（实测的"帧间提交边界挂起"）。
		const bool windowed = !psDeviceFlags.is(rsFullscreen);
		if (windowed)
			sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

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

		// 新围栏从 0 开始计数：提交序号与每 backbuffer 的登记值必须一并清零。
		// 否则设备重建（vid_restart）后 BeginFrame 会等一个"属于旧设备的序号"——
		// 新围栏永远 signal 不到它 → 永久阻塞（表现为重建后直接卡死/崩溃）。
		SubmitSeq = 0;
		for (u32 i = 0; i < NUM_BACKBUFFERS; ++i)
			FenceValue[i] = 0;

		FenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		return FenceEvent != nullptr;
	}

	// ---------------------------------------------------------------------------
	// GPU 面包屑（设备层实现，导出给渲染层使用）
	//
	// WriteBufferImmediate 写进一个 READBACK 缓冲：设备挂起（TDR）后 CPU 仍能读回
	// "GPU 最后执行到哪一步"。放在设备层是为了覆盖 EndFrame 内部的转换/提交——
	// 实测挂起发生在帧末（渲染层的最后一个标记 'frame:tail' 之后）。
	// ---------------------------------------------------------------------------
	ComPtr<ID3D12Resource>		BcBuffer;
	void*						BcMapped = nullptr;
	static UINT					BcFrame = 0;	// 面包屑帧号（编码进写入值高 16 位）
	// 环形时间线：256 槽，WriteBreadcrumb 按全局序号递增写槽 (BcSeq&255)。
	// 单值版本只能看到"最后一条"，无法区分"卡在帧末标记之后"还是"卡在下一帧开头"，
	// 更无法回溯 GPU 死前执行了哪些 pass；环形版在设备移除时回放最近 24 条，
	// 可直接读出 GPU 最后执行的完整标记序列（配合 r4:*/frame:* 面包屑定位卡点）。
	static volatile LONG		BcSeq = 0;
	static constexpr UINT		kBcSlots = 256;
	static std::vector<const char*>	s_bcNames;	// 1-based：值 N → s_bcNames[N-1]
	static std::mutex			s_bcMutex;
	static bool					s_dredDumped = false;

	bool CreateBreadcrumbBuffer()
	{
		// 状态固定 COPY_DEST（WriteBufferImmediate 对目标状态的要求），且不是
		// shader-visible 资源，无需转换
		D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_READBACK;
		D3D12_RESOURCE_DESC bd = {};
		bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		bd.Width = kBcSlots * 4; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
		bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
		bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		if (FAILED(D3DDevice->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
			D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&BcBuffer))))
			return false;
		D3D12_RANGE rr = { 0, kBcSlots * 4 };
		return SUCCEEDED(BcBuffer->Map(0, &rr, &BcMapped));
	}

	extern "C" ENGINE_API UINT RegisterBreadcrumbName(const char* name)
	{
		if (!name) name = "?";
		std::lock_guard<std::mutex> g(s_bcMutex);
		for (size_t i = 0; i < s_bcNames.size(); ++i)
			if (s_bcNames[i] == name || 0 == strcmp(s_bcNames[i], name))
				return (UINT)(i + 1);
		s_bcNames.push_back(name);
		return (UINT)s_bcNames.size();
	}

	extern "C" ENGINE_API void WriteBreadcrumb(UINT value)
	{
		if (!BcBuffer || !FrameInFlight || !CmdList) return;

		static ID3D12GraphicsCommandList2* s_cl2 = nullptr;
		if (!s_cl2 && FAILED(CmdList->QueryInterface(IID_PPV_ARGS(&s_cl2)))) return;
		if (!s_cl2) return;

		const UINT slot = (UINT)InterlockedIncrement(&BcSeq) & (kBcSlots - 1);
		D3D12_WRITEBUFFERIMMEDIATE_PARAMETER p = {};
		p.Dest = BcBuffer->GetGPUVirtualAddress() + (SIZE_T)slot * 4;
		// 高 16 位写帧号：读回时据此判断这是"本帧"还是"上一帧"的陈旧值——
		// 帧末标记每帧都写，不打戳就无法区分"卡在帧末"与"卡在下一帧开头"。
		p.Value = ((BcFrame & 0xFFFFu) << 16) | (value & 0xFFFFu);
		s_cl2->WriteBufferImmediate(1, &p, nullptr);
	}

	extern "C" ENGINE_API void DumpBreadcrumb(const char* tag)
	{
		if (!BcMapped) return;

		const LONG cpuSeq = InterlockedOr(&BcSeq, 0);
		const volatile UINT* ring = (const volatile UINT*)BcMapped;

		// 从最新往回找 GPU 已执行的最高序号（槽为 0 = 尚未执行）
		LONG gpuSeq = 0;
		for (LONG k = cpuSeq; k > 0 && k > cpuSeq - 1024; --k)
		{
			if (ring[(size_t)(k & (kBcSlots - 1))]) { gpuSeq = k; break; }
		}

		Msg("! DX12 [%s]: GPU breadcrumb 环形时间线：CPU 已录制标记 #%u，GPU 已执行到 #%u（%u 条已录未执行）",
			tag ? tag : "?", (UINT)cpuSeq, (UINT)gpuSeq, (UINT)(cpuSeq - gpuSeq));

		std::lock_guard<std::mutex> g(s_bcMutex);
		const LONG first = gpuSeq > 24 ? gpuSeq - 24 : 1;
		for (LONG k = first; k <= gpuSeq; ++k)
		{
			const UINT packed = ring[(size_t)(k & (kBcSlots - 1))];
			const UINT marker = packed & 0xFFFFu;
			const UINT frame = packed >> 16;
			const char* name = (marker > 0 && marker <= s_bcNames.size()) ? s_bcNames[marker - 1] : "?";
			Msg("! DX12 [%s]:   bc#%u = %s @帧%u", tag ? tag : "?", (UINT)k, name, frame);
		}
	}

	void WaitFenceValue(UINT64 v)
	{
		if (!v || !Fence || !CmdQueue) return;
		if (Fence->GetCompletedValue() < v)
		{
			Fence->SetEventOnCompletion(v, FenceEvent);
			WaitForSingleObject(FenceEvent, INFINITE);
		}
	}

	void WaitForGpu()
	{
		CmdQueue->Signal(Fence.Get(), ++SubmitSeq);
		WaitFenceValue(SubmitSeq);
	}

	// Present 之后只推进 backbuffer 索引；真正的等待在 BeginFrame 里
	// （复用该 backbuffer 的资源之前等它上一次的使用者执行完），
	// 这样 CPU/GPU 仍可重叠一帧，不必全序列化。
	void MoveToNextFrame()
	{
		FrameIndex = Swapchain->GetCurrentBackBufferIndex();
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

		if (!dx12::CreateBreadcrumbBuffer())
			Msg("! DX12: breadcrumb buffer creation failed (GPU 取证不可用)");

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
		dx12::FenceValue[i] = 0;	// WaitForGpu 已等全部提交完成，无待等待帧
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

	// 设备移除（TDR / 非法访问）尽早暴露：只报一次并附上帧号与提交序号。
	// 否则后续几十万次"创建失败"的刷屏会把真正的现场（哪一帧、GPU 落后多少）淹没。
	void ReportDeviceLostOnce(const char* where)
	{
		static bool s_reported = false;
		if (s_reported || !D3DDevice) return;

		const HRESULT drr = D3DDevice->GetDeviceRemovedReason();
		if (SUCCEEDED(drr)) return;

		s_reported = true;
		Msg("! DX12: >>> DEVICE REMOVED <<< at %s: frame=%u bb=%u submit=%llu (reason 0x%08x)",
			where, Device.dwFrame, FrameIndex, SubmitSeq, (unsigned)drr);
		DumpBreadcrumb(where);
		DrainInfoQueue();
	}

	// 开始一帧：重置命令分配器/列表，转换 backbuffer 到 RT 状态
	extern "C" ENGINE_API void BeginFrame()
	{
		DrainInfoQueue();
		ReportDeviceLostOnce("BeginFrame");

		if (FrameInFlight)
			return;

		// 复用该 backbuffer 的命令分配器 / 描述符堆槽 / 上传环段之前，
		// 等上次使用它的那一帧在 GPU 上执行完毕（FenceValue 在 EndFrame 中登记）。
		const UINT64 pending = FenceValue[FrameIndex];
		if (pending && Fence && Fence->GetCompletedValue() < pending)
		{
			const ULONGLONG t0 = GetTickCount64();
			WaitFenceValue(pending);
			const ULONGLONG dt = GetTickCount64() - t0;
			// 等待本身是"GPU 落后于 CPU"的证据：正常运行时该等待几乎为 0；
			// 若频繁出现长时间等待，说明 GPU 队列积压（性能/TDR 线索）。
			if (dt >= 8)
				Msg("* DX12: [sync] bb=%u waited %llu ms for submit=%llu (queue=%llu)",
					FrameIndex, (unsigned long long)dt, (unsigned long long)pending,
					(unsigned long long)(SubmitSeq - Fence->GetCompletedValue()));
		}

		CmdAlloc[FrameIndex]->Reset();
		CmdList->Reset(CmdAlloc[FrameIndex].Get(), nullptr);

		++BcFrame;
		WriteBreadcrumb(RegisterBreadcrumbName("beginframe:pre_barrier"));

		D3D12_RESOURCE_BARRIER toRT = {};
		toRT.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		toRT.Transition.pResource = BackBuffer[FrameIndex].Get();
		toRT.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
		toRT.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
		toRT.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		CmdList->ResourceBarrier(1, &toRT);

		WriteBreadcrumb(RegisterBreadcrumbName("beginframe:post_barrier"));

		// D3D12 scissor test 始终开启，命令列表 reset 后 scissor 为空；而 D3D11 默认
		// ScissorEnable=FALSE（不裁剪）本应无需调用者干预。每帧先绑定一个最大范围
		// scissor 作为默认"不裁剪"状态，引擎需要裁剪时再经 set_Scissor(R) 覆盖。
		// 这样即便在未绑定 RT 的早期 Draw 上也不会被空矩形裁掉像素。
		static const D3D12_RECT defaultScissor = { 0, 0, 16384, 16384 };
		CmdList->RSSetScissorRects(1, &defaultScissor);

		FrameInFlight = true;
		WriteBreadcrumb(RegisterBreadcrumbName("beginframe:ready"));
	}

	// 结束一帧：转换到 PRESENT，关闭并执行命令列表
	extern "C" ENGINE_API void EndFrame()
	{
		if (!FrameInFlight)
			return;

		// 帧末三段取证：转换前 / 转换后 / 提交后，用于区分挂起发生在
		// backbuffer RT->PRESENT 转换、命令列表提交，还是提交之后的呈现/翻转
		WriteBreadcrumb(RegisterBreadcrumbName("endframe:enter"));

		D3D12_RESOURCE_BARRIER toPresent = {};
		toPresent.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		toPresent.Transition.pResource = BackBuffer[FrameIndex].Get();
		toPresent.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
		toPresent.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
		toPresent.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		CmdList->ResourceBarrier(1, &toPresent);

		WriteBreadcrumb(RegisterBreadcrumbName("endframe:barrier"));

		CmdList->Close();
		ID3D12CommandList* lists[] = { CmdList.Get() };
		CmdQueue->ExecuteCommandLists(1, lists);

		// 登记本帧提交：下一个复用该 backbuffer 的帧会等这个序号完成
		++SubmitSeq;
		CmdQueue->Signal(Fence.Get(), SubmitSeq);
		FenceValue[FrameIndex] = SubmitSeq;

		// 设备移除现场：本帧刚提交完是最早能发现"上一帧把设备干掉了"的时刻
		ReportDeviceLostOnce("EndFrame");

		// GBV 的部分错误在 ExecuteCommandLists 后同步产出，立即转储，
		// 确保即便本帧随后崩溃也不会丢失关键信息。
		DrainInfoQueue();

		FrameInFlight = false;
	}

	extern "C" ENGINE_API void FramePresent(bool vsync)
	{
		// 窗口化 + 允许撕裂时，关闭 vsync 走 tearing present：翻转立即退休，不依赖
		// 显示侧的 vblank 推进（避免"GPU 停在 driver 追加的翻转等待、围栏不 signal"）。
		UINT flags = 0;
		if (!vsync)
		{
			DXGI_SWAP_CHAIN_DESC1 sd = {};
			if (SUCCEEDED(Swapchain->GetDesc1(&sd)) && (sd.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING))
				flags = DXGI_PRESENT_ALLOW_TEARING;
		}

		const HRESULT hr = Swapchain->Present(vsync ? 1 : 0, flags);
		// 非 S_OK 状态（OCCLUDED / MODE_CHANGED 等成功状态码也在内）同样记录：
		// 窗口被遮挡时翻转会被跳过，是"帧间提交边界挂起"的可疑触发条件
		if (hr != S_OK)
		{
			static int s_reported = 0;
			if (++s_reported <= 8)
				Msg("! DX12: Present(vsync=%d flags=0x%x) -> 0x%08x", vsync ? 1 : 0, flags, (unsigned)hr);
		}
		MoveToNextFrame();
	}

	// R5 模块资源访问
	extern "C" ENGINE_API ID3D12Device* GetDevice() { return D3DDevice.Get(); }
	extern "C" ENGINE_API ID3D12CommandQueue* GetCommandQueue() { return CmdQueue.Get(); }
	extern "C" ENGINE_API ID3D12GraphicsCommandList* GetCmdList() { return CmdList.Get(); }
	// 命令列表是否处于录制中（BeginFrame..EndFrame）：GPU 面包屑写入前必须确认，
	// 否则会往已 Close 的列表里写 WriteBufferImmediate（非法调用）
	extern "C" ENGINE_API bool IsFrameRecording() { return FrameInFlight; }
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
