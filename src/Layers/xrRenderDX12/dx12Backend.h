#pragma once

//==============================================================================
// dx12Backend.h
//
// D3D12 后端全局设施：
//   * CBV/SRV/UAV 与 Sampler 的 shader-visible 描述符堆（每帧线性分配）
//   * 上传环（UPLOAD 堆，CPU 直写，替代 D3D11 的 Map(WRITE_DISCARD)）
//   * PSO 缓存：由 dx12StateManager 累积的状态 + shader + 输入布局 hash 合成
//   * 根签名：CBV(b0..bN) + SRV(t0..tN) + 静态采样器
//
// 设备 / 命令列表来自 xrEngine 的 Device_create_render_dx12.cpp。
//==============================================================================

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <unordered_map>
#include <vector>
#include <mutex>

namespace dx12
{
	using Microsoft::WRL::ComPtr;

	//--------------------------------------------------------------------------
	// 描述符堆（shader-visible，每帧线性分配）
	//--------------------------------------------------------------------------
	class DescriptorHeap
	{
	public:
		bool	Create(ID3D12Device* dev, D3D12_DESCRIPTOR_HEAP_TYPE type, UINT capacity, bool shaderVisible);
		void	Reset() { m_offset = 0; }

		bool	Alloc(UINT count, D3D12_CPU_DESCRIPTOR_HANDLE& cpu, D3D12_GPU_DESCRIPTOR_HANDLE& gpu);
		// 永久分配（不随帧重置）：用于纹理 SRV / RTV / DSV 等生命周期与资源一致的描述符
		bool	AllocPersistent(UINT count, D3D12_CPU_DESCRIPTOR_HANDLE& cpu);
		// 归还永久槽位。引擎（尤其 surface_set）会反复重建视图，D3D12 不会自动回收，
		// 不归还则堆会随帧数单调耗尽，最终所有视图创建失败。
		void	FreePersistent(UINT index);
		UINT	IndexOf(D3D12_CPU_DESCRIPTOR_HANDLE cpu) const;
		UINT	PersistUsed() const { return m_persistOffset - (UINT)m_persistFree.size(); }
		UINT	Capacity() const { return m_capacity; }
		D3D12_CPU_DESCRIPTOR_HANDLE CPU(UINT index) const;
		D3D12_GPU_DESCRIPTOR_HANDLE GPU(UINT index) const;

		ID3D12DescriptorHeap* Heap() const { return m_heap.Get(); }
		UINT	DescriptorSize() const { return m_size; }
	private:
		ComPtr<ID3D12DescriptorHeap>	m_heap;
		D3D12_CPU_DESCRIPTOR_HANDLE		m_cpuStart = {};
		D3D12_GPU_DESCRIPTOR_HANDLE		m_gpuStart = {};
		UINT							m_size = 0;
		UINT							m_capacity = 0;
		UINT							m_offset = 0;
		UINT							m_persistOffset = 0;
		std::vector<UINT>				m_persistFree;
	};

	//--------------------------------------------------------------------------
	// 上传环（CPU 直写 + GPU 虚拟地址）
	//--------------------------------------------------------------------------
	class UploadRing
	{
	public:
		bool	Create(ID3D12Device* dev, UINT64 size);
		void	Reset() { m_offset = m_persist; }
		void	MarkPersist() { m_persist = m_offset; }

		void*	Alloc(UINT64 size, UINT64 align, D3D12_GPU_VIRTUAL_ADDRESS& gpuAddr);

		ID3D12Resource* Resource() const { return m_buffer.Get(); }
	private:
		ComPtr<ID3D12Resource>	m_buffer;
		void*					m_mapped = nullptr;
		UINT64					m_size = 0;
		UINT64					m_offset = 0;
		UINT64					m_persist = 0;
	};

	//--------------------------------------------------------------------------
	// 后端全局
	//--------------------------------------------------------------------------
	struct Backend
	{
		DescriptorHeap	cbvSrvUav;		// shader-visible，4096
		DescriptorHeap	sampler;		// shader-visible，256
		UploadRing		ring;			// 256MB

		// 永久描述符堆（非 shader-visible），生命周期与资源一致
		DescriptorHeap	persistentSrv;	// 纹理 SRV
		DescriptorHeap	persistentRtv;
		DescriptorHeap	persistentDsv;
		DescriptorHeap	persistentUav;

		ComPtr<ID3D12RootSignature>	rootSignature;
		// PSO 缓存：key 由状态 hash + shader 指针 + 输入布局 hash 组成
		std::unordered_map<UINT64, ComPtr<ID3D12PipelineState>> psoCache;

		// 1x1 黑色占位纹理的 SRV（persistent 堆）：FlushPipeline 为无效/未绑定的 SRV 槽
		// 填充此描述符，避免描述符堆中残留上 Draw 的旧描述符导致采样错纹理。
		D3D12_CPU_DESCRIPTOR_HANDLE	dummySrvCpu = {};
		ComPtr<ID3D12Resource>		dummyTex;

		// 全零 64KB 常量缓冲的 CBV（persistent 堆）：FlushPipeline 为"本 Draw 未绑定
		// 常量缓冲"的槽位填充此描述符。留空会被 GBV 判为 Uninitialized descriptor，
		// 实机上着色器读到的是上一 Draw 残留的常量（光照/环境色错乱 → 画面泛白）。
		D3D12_CPU_DESCRIPTOR_HANDLE	dummyCbvCpu = {};
		ComPtr<ID3D12Resource>		dummyCb;

		UINT			rtvDescriptorSize = 0;
		UINT			dsvDescriptorSize = 0;

		// 一次性上传通道（默认堆纹理初始化 / 3D 纹理上传用）
		ComPtr<ID3D12CommandQueue>			uploadQueue;
		ComPtr<ID3D12CommandAllocator>		uploadAlloc;
		ComPtr<ID3D12GraphicsCommandList>	uploadList;
		ComPtr<ID3D12Fence>					uploadFence;
		UINT64								uploadFenceValue = 0;
		void*								uploadEvent = nullptr;

		bool			valid = false;
		bool			frameActive = false;	// 主命令列表正在录制（BeginFrame..EndFrame）
		// 串行化所有立即型 GPU 工作（上传/初始化清屏）：启动期多线程并发
		// 加载纹理会同时操作同一个上传命令列表，D3D12 要求外部同步
		std::mutex		gpuWorkMutex;
	};

	extern Backend	g_backend;

	// 惰性初始化：首次分配描述符/上传环时按需建立堆与根签名
	void	Ensure();

	// 把 CPU 数据上传到默认堆纹理的子资源 0（内部走一次性命令列表 + 围栏等待），
	// 并在完成后把资源转为 ALL_SHADER_RESOURCE 状态。
	// curState：目标资源当前的资源状态（上传期间会临时切到 COPY_DEST，完成后恢复回 curState）。
	// 必须与创建时使用的初始状态一致，否则 ResourceBarrier 的 StateBefore 不匹配会被 D3D12 判为
	// 非法调用并移除设备。
	void	UploadTextureSubresource(ID3D12Resource* dst, UINT subresourceIndex, const void* src, UINT srcRowPitch,
		UINT width, UINT height, UINT depth, DXGI_FORMAT fmt, DXGI_FORMAT footprintFormat,
		D3D12_RESOURCE_STATES curState = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

	// 把默认堆纹理的子资源读回 CPU（D3D12 的 READBACK 堆不能承载纹理）：
	// 内部 CopyTextureRegion → READBACK 缓冲 + 围栏等待，返回映射指针与行间距。
	// 缓冲按 (resource,subresource) 缓存在后端，下次读回时复用。
	// curState：纹理当前状态（读回期间 curState→COPY_SOURCE→curState）。
	void*	ReadbackTextureSubresource(ID3D12Resource* src, UINT subresourceIndex,
		UINT width, UINT height, DXGI_FORMAT fmt, UINT& outRowPitch,
		D3D12_RESOURCE_STATES curState = D3D12_RESOURCE_STATE_COPY_DEST);

	bool	Init(ID3D12Device* dev);
	void	Shutdown();

	// 诊断：打印 GetDeviceRemovedReason 与 debug layer info queue 中存储的错误消息
	void	DumpDeviceErrors(const char* tag);

	// 主帧未开启时（设备初始化期）在一次性 direct 队列上立即清 RTV
	void	ClearRTVImmediate(D3D12_CPU_DESCRIPTOR_HANDLE rtv, const FLOAT color[4]);
	void	BackendBeginFrame();		// Reset 描述符堆 + 上传环
	void	BackendEndFrame();

	// 根签名：CBV b0..b13(VS) / SRV t0..t15 + 静态采样器 s0..s15
	ID3D12RootSignature* GetRootSignature();

	// 采样器（静态）——阶段 1 先用一组固定采样器
	D3D12_STATIC_SAMPLER_DESC GetStaticSampler(UINT slot);
}
