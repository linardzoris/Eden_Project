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
#include <unordered_set>
#include <vector>
#include <mutex>

namespace dx12
{
	using Microsoft::WRL::ComPtr;

	//--------------------------------------------------------------------------
	// 描述符堆（shader-visible，每帧线性分配）
	//
	// 每帧线性分配的堆必须为每个 backbuffer 各备一份（slots=2）：命令列表是
	// "先录制、后由 GPU 执行"，CPU 录制下一帧时若复用同一份堆，会把 GPU 仍在读的
	// 描述符整片覆盖（表现为无 -dxdebug 时的高速花屏/闪烁/贴图错乱）。
	//--------------------------------------------------------------------------
	class DescriptorHeap
	{
	public:
		bool	Create(ID3D12Device* dev, D3D12_DESCRIPTOR_HEAP_TYPE type, UINT capacity, bool shaderVisible, UINT slots = 1);
		void	SetSlot(UINT slot) { if (slot < m_slots) m_slot = slot; }
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
		UINT	Used() const { return m_offset; }	// 本帧已分配数（容量体检用）
		D3D12_CPU_DESCRIPTOR_HANDLE CPU(UINT index) const;
		D3D12_GPU_DESCRIPTOR_HANDLE GPU(UINT index) const;

		ID3D12DescriptorHeap* Heap() const { return m_heap[m_slot].Get(); }
		UINT	DescriptorSize() const { return m_size; }
	private:
		ComPtr<ID3D12DescriptorHeap>	m_heap[2];
		D3D12_CPU_DESCRIPTOR_HANDLE		m_cpuStart[2] = {};
		D3D12_GPU_DESCRIPTOR_HANDLE		m_gpuStart[2] = {};
		UINT							m_size = 0;
		UINT							m_capacity = 0;
		UINT							m_slots = 1;
		UINT							m_slot = 0;
		UINT							m_offset = 0;
		UINT							m_persistOffset = 0;
		std::vector<UINT>				m_persistFree;
		// 持久槽位簿记互斥：加载线程（CreateTexture2DFromScratch → surface_set →
		// 建 SRV）与渲染线程（phase_combine 每帧对 t_envmap_0/1 surface_set 重建 SRV）
		// 并发走 AllocPersistent/FreePersistent。无锁时两线程会 pop_back 同一槽
		//（空 vector pop = UB）→ m_persistFree 损坏 → 垃圾索引的 CPU 句柄被
		// CreateShaderResourceView 写 32 字节 = 无界内存踩踏。表现为：无 dxdebug 时
		// 加载期必现 TDR（死点随机、PageFaultVA=0x0、GBV 全绿——CPU 侧踩踏 GPU 校验
		// 看不见）；GBV 拖慢渲染改变双方时序故 -dxdebug 不复现。
		std::mutex						m_persistMutex;
	};

	//--------------------------------------------------------------------------
	// 上传环（CPU 直写 + GPU 虚拟地址）
	//
	// 持久区 [0, m_persist) 放初始化期静态数据；其余空间按 backbuffer 分两段，
	// 本帧只在本段内线性分配（SetSlot(GetFrameIndex() & 1)）。这样 CPU 录制下一帧
	// 时不会覆盖 GPU 仍在读的上一帧数据。
	//--------------------------------------------------------------------------
	class UploadRing
	{
	public:
		bool	Create(ID3D12Device* dev, UINT64 size);
		void	SetSlot(UINT slot) { m_slot = slot & 1; }
		void	Reset();
		void	MarkPersist() { m_persist = m_offset; }	// 仅初始化期调用（钉住静态数据）

		void*	Alloc(UINT64 size, UINT64 align, D3D12_GPU_VIRTUAL_ADDRESS& gpuAddr);

		ID3D12Resource* Resource() const { return m_buffer.Get(); }
		UINT64	SlotUsed() const { return m_offset - m_segBase; }	// 本帧段已用字节（体检用）
		// 环的 GPU VA 区间：绑定路径用它判定"这个 VA 是否可能来自上传环"
		bool	Contains(UINT64 va) const { return m_vaBegin && va >= m_vaBegin && va < m_vaEnd; }
	private:
		ComPtr<ID3D12Resource>	m_buffer;
		void*					m_mapped = nullptr;
		UINT64					m_size = 0;
		UINT64					m_offset = 0;
		UINT64					m_persist = 0;
		UINT64					m_segBase = 0;	// 本帧段起点
		UINT64					m_segEnd = 0;	// 本帧段的可用上界（Alloc 不得越过）
		UINT					m_slot = 0;
		UINT64					m_vaBegin = 0;	// 环资源 GPU VA 起点
		UINT64					m_vaEnd = 0;	// 环资源 GPU VA 终点（不含）
		// 分配游标是全局状态：异步资源加载线程若与渲染线程并发分配，
		// 不加锁会得到重叠区域（互相覆写 → 垃圾数据喂给着色器）
		std::mutex				m_allocMutex;
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
	// 仅维度匹配 TEXTURE2D 的槽位使用；其余维度见 nullSrvCpu。
	D3D12_CPU_DESCRIPTOR_HANDLE	dummySrvCpu = {};
	ComPtr<ID3D12Resource>		dummyTex;

	// 各维度的空描述符（null SRV，persistent 堆，下标 = D3D12_SRV_DIMENSION 枚举值）。
	// 无效槽位按着色器反射出的维度绑定：空描述符读取返回 0，与 D3D11 未绑定行为
	// 一致。此前统一填 1x1 2D 纹理，cube 槽位维度错配（GBV: expected TEXTURECUBE
	// got TEXTURE2D；SSLR-only 的 s_env / vid_restart 后首帧的 sky_s0、env_s0 均属
	// 此类），实机读取为未定义行为。
	// [TEXTURE2D] 固定为空：2D 槽位沿用上面的真实 1x1 黑纹理，保持旧行为。
	D3D12_CPU_DESCRIPTOR_HANDLE	nullSrvCpu[D3D12_SRV_DIMENSION_TEXTURECUBEARRAY + 1] = {};

	// 兜底根表（shader-visible 堆，按堆 slot 各一份）：BackendBeginFrame 在堆 Reset
	// 后立即分配并填好（14×全零 CBV + 16×黑纹理 SRV）。本帧描述符堆耗尽导致
	// Alloc 失败时，FlushPipeline 给该 Draw 绑兜底表——保证 4 张根表永远有合法
	// 句柄。若帧首首个 Draw 以零句柄执行，GPU 读描述符会页错误（DRED 实测
	// VA=0x0）→ 队列停摆 → TDR（DEVICE_HUNG 0x887a0006）。
	D3D12_GPU_DESCRIPTOR_HANDLE	fallbackCbvGpu[2] = {};
	D3D12_GPU_DESCRIPTOR_HANDLE	fallbackSrvGpu[2] = {};

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
		// 每帧自增的"环代次"：BackendBeginFrame 里 +1。动态 VB/IB 的改名区域
		// 用它标记所属帧（比 Device.dwFrame 可靠：后者不保证每帧变化，
		// 且初始化期可能长期为 0，会让陈旧区域被误判为"本帧有效"）。
		UINT			frameSerial = 0;
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

	// 异步纹理上传（关卡/流送加载路径专用）：加载线程入队，渲染线程在帧首
	//（dxRenderDeviceRender::Begin 里 ::BeginFrame 之后）把排空录制到主命令列表。
	// 取代旧路径"每张纹理在独立 uploadQueue 上开一次性列表同步拷贝"——加载高峰期
	//（game_loaded 前后）数千条上传列表与主渲染队列并发执行，两条队列对同一批资源
	// 的屏障/拷贝交错会把 GPU 前端卡死（实测 TDR 0x887a0006，DRED PageFaultVA=0x0，
	// GPU 停在"上一帧已完、下一帧未启"的队列间隙；-dxdebug 的 GBV 序列化掩盖竞争
	// 故不复现）。请求持有 dst 的 ComPtr 引用与暂存字节；帧首字节进上传环（本帧段
	// 由 backbuffer 围栏保护），屏障走共享状态跟踪器，拷贝与帧内绘制同列表按序执行。
	// 帧循环未启动（初始化期）自动退回同步 UploadTextureSubresource。
	// 上传完成后纹理处于 ALL_SHADER_RESOURCE（加载路径的创建初态，语义正确）。
	void	EnqueueTextureUpload(ID3D12Resource* dst, UINT subresourceIndex, const void* src, UINT srcRowPitch,
		UINT width, UINT height, UINT depth, DXGI_FORMAT fmt, DXGI_FORMAT footprintFormat);
	// 帧首排空（渲染线程调用）：把 pending 上传录制进本帧主命令列表
	void	ProcessPendingTextureUploads();

	// 把默认堆纹理的子资源读回 CPU（D3D12 的 READBACK 堆不能承载纹理）：
	// 内部 CopyTextureRegion → READBACK 缓冲 + 围栏等待，返回映射指针与行间距。
	// 缓冲按 (resource,subresource) 缓存在后端，下次读回时复用。
	// curState：纹理当前状态（读回期间 curState→COPY_SOURCE→curState）。
	void*	ReadbackTextureSubresource(ID3D12Resource* src, UINT subresourceIndex,
		UINT width, UINT height, DXGI_FORMAT fmt, UINT& outRowPitch,
		D3D12_RESOURCE_STATES curState = D3D12_RESOURCE_STATE_COPY_DEST);

	bool	Init(ID3D12Device* dev);
	void	Shutdown();

	// 诊断：打印 GetDeviceRemovedReason 与 debug layer info queue 中存储的错误消息，
	// 并在首次发现设备移除时输出 DRED（自动面包屑 + 页错误分配）
	void	DumpDeviceErrors(const char* tag);
	// GPU 面包屑（RegisterBreadcrumbName / WriteBreadcrumb / DumpBreadcrumb）
	// 由设备层实现并导出（见 xrEngine/Device_create_render_dx12.cpp），
	// 声明在 xrRenderPC_R5/stdafx.h —— 渲染层各 pass 直接调用即可。

	// 主帧未开启时（设备初始化期）在一次性 direct 队列上立即清 RTV
	void	ClearRTVImmediate(D3D12_CPU_DESCRIPTOR_HANDLE rtv, const FLOAT color[4]);
	void	BackendBeginFrame();	// Reset 描述符堆 + 上传环
	void	BackendEndFrame();
	// 当前环代次（BackendBeginFrame 递增）：动态 VB/IB 改名区域的有效性标记
	UINT	FrameSerial();

	// 常量缓冲字段（gpuVA/size）读写锁。
	// Flush 可能来自资源加载线程，而建 CBV 在渲染线程：并发时会出现撕裂的
	// gpuVA（实测 0x40000004d，低字节被污染）→ 非法 CBV → 调试层异常/设备移除。
	std::mutex&	ConstantBufferMutex();

	// 【诊断·核弹实验】纹理加载生命周期 与 渲染帧（Begin..End）的完全互斥锁（递归）。
	// 渲染线程在 dxRenderDeviceRender::Begin..End 全程持有；加载线程的
	// CreateTexture2D/3DFromScratch、Enqueue/UploadTextureSubresource、
	// CreateShaderResourceView 全程持有。任一边进行时另一边整体暂停。
	// 目的：一锤定音判定"加载期 TDR"是否来自加载×渲染并发——
	//   串行后不再崩 => 竞争类根因，后续按锁分段逐步缩小范围；
	//   仍崩 => 与并发无关，排除整类，另找方向（帧机制本身/驱动）。
	std::recursive_mutex&	LifecycleMutex();

	// 设备/持久 SRV 代次：根表复用缓存（FlushPipeline）的失效因子。vid_restart 会
	// 重建设备与全部持久描述符，代次必须随之递增，否则跨设备残留的缓存键/句柄
	// 会被错误复用（旧堆句柄 → GPU 页错误）。FreePersistent 归还持久 SRV 槽位时
	// 递增 SrvGeneration，防止"同地址同槽位的新 SRV"被误判为未变。
	UINT	DeviceGeneration();
	UINT	SrvGeneration();
	void	BumpSrvGeneration();

	//--------------------------------------------------------------------------
	// 缓冲存活登记 + 上传环区间判定 —— 绑定路径的"陈旧绑定"防护/取证
	//
	// vid_restart 会销毁重建渲染层的着色器与常量缓冲，但 CBackend/dx12Context 里
	// 缓存的原始指针（cbPS/cbVS/vb/ib）不会随之清空。指向已析构 dx12Buffer 的
	// "陈旧绑定"读到的是堆回收后的垃圾字段：实测 gpuVA=0x40000004d（非 256 对齐），
	// 而字段为 0 时反倒能通过 256 对齐检查 → 生成 BufferLocation=0 的 CBV →
	// 着色器读常量触发 GPU 页错误（DRED 实测 VA=0x0）→ 队列停摆 → TDR/DEVICE_HUNG。
	//
	// 绑定前先判定对象是否存活；再要求 VA 只能落在上传环区间或等于资源自身 VA
	// （合法 CBV/VB/IB 的 VA 来源只有这两处），其余一律视为非法：CBV 槽改填占位
	// 描述符、Draw 直接跳过，并把现场（缓冲指针/存活标志/VA/资源 VA）写进日志。
	//--------------------------------------------------------------------------
	void	RegisterLiveBuffer(const void* buf);
	void	UnregisterLiveBuffer(const void* buf);
	bool	IsLiveBuffer(const void* buf);
	bool	UploadRingContains(UINT64 va);

	// 根签名：CBV b0..b13(VS) / SRV t0..t15 + 静态采样器 s0..s15
	ID3D12RootSignature* GetRootSignature();

	// 采样器（静态）——阶段 1 先用一组固定采样器
	D3D12_STATIC_SAMPLER_DESC GetStaticSampler(UINT slot);
}
