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

		UINT			rtvDescriptorSize = 0;
		UINT			dsvDescriptorSize = 0;

		bool			valid = false;
	};

	extern Backend	g_backend;

	bool	Init(ID3D12Device* dev);
	void	Shutdown();
	void	BackendBeginFrame();		// Reset 描述符堆 + 上传环
	void	BackendEndFrame();

	// 根签名：CBV b0..b13(VS) / SRV t0..t15 + 静态采样器 s0..s15
	ID3D12RootSignature* GetRootSignature();

	// 采样器（静态）——阶段 1 先用一组固定采样器
	D3D12_STATIC_SAMPLER_DESC GetStaticSampler(UINT slot);
}
