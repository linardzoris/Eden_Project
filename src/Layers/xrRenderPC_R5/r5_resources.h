#pragma once

// M3a: DX12 资源基础设施
// - GpuDescriptorHeap: shader-visible CBV/SRV/UAV 堆，每帧线性分配
// - UploadRing: UPLOAD 堆环形缓冲，CB/顶点数据 CPU 直写，256 字节对齐

#include <d3d12.h>
#include <wrl/client.h>

namespace r5_res
{
	// -----------------------------------------------------------------------
	// GPU 可见描述符堆（CBV/SRV/UAV），每帧线性分配
	// -----------------------------------------------------------------------
	class GpuDescriptorHeap
	{
	public:
		bool Create(ID3D12Device* dev, UINT capacity);
		void Reset();	// 每帧开始调用

		// 分配 count 个连续描述符，返回 CPU/GPU 句柄
		D3D12_CPU_DESCRIPTOR_HANDLE AllocCPU(UINT count);
		D3D12_GPU_DESCRIPTOR_HANDLE AllocGPU(UINT count);

		ID3D12DescriptorHeap* Heap() const { return m_heap.Get(); }
		UINT DescriptorSize() const { return m_size; }

	private:
		Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_heap;
		D3D12_CPU_DESCRIPTOR_HANDLE m_cpuStart = {};
		D3D12_GPU_DESCRIPTOR_HANDLE m_gpuStart = {};
		UINT m_size = 0;
		UINT m_capacity = 0;
		UINT m_offset = 0;
	};

	// -----------------------------------------------------------------------
	// 上传环形缓冲：Map 一次，线性分配，每帧 Reset
	// -----------------------------------------------------------------------
	class UploadRing
	{
	public:
		bool Create(ID3D12Device* dev, UINT64 size);
		void Reset() { m_offset = m_persist; }	// 每帧开始调用（M0 单帧围栏安全）
		void MarkPersist() { m_persist = m_offset; }	// Init 阶段静态数据保护点

		// 分配 size 字节，返回 CPU 指针 + GPU 虚拟地址，256 对齐
		void* Alloc(UINT64 size, D3D12_GPU_VIRTUAL_ADDRESS& gpuAddr);

		ID3D12Resource* Resource() const { return m_buffer.Get(); }

	private:
		Microsoft::WRL::ComPtr<ID3D12Resource> m_buffer;
		void* m_mapped = nullptr;
		UINT64 m_size = 0;
		UINT64 m_offset = 0;
		UINT64 m_persist = 0;
	};

	extern GpuDescriptorHeap g_gpuHeap;	// 4096 CBV/SRV/UAV
	extern UploadRing g_upload;			// 16 MB

	bool Init();
	void Shutdown();
	void BeginFrame();	// Reset 两个分配器
}
