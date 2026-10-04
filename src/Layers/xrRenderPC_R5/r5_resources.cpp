#include "stdafx.h"
#include "r5_resources.h"

using Microsoft::WRL::ComPtr;

namespace dx12
{
	extern "C" ENGINE_API ID3D12Device* GetDevice();
}

namespace r5_res
{
	GpuDescriptorHeap g_gpuHeap;
	UploadRing g_upload;

	// -----------------------------------------------------------------------
	// GpuDescriptorHeap
	// -----------------------------------------------------------------------

	bool GpuDescriptorHeap::Create(ID3D12Device* dev, UINT capacity)
	{
		m_capacity = capacity;
		m_size = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

		D3D12_DESCRIPTOR_HEAP_DESC desc = {};
		desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		desc.NumDescriptors = capacity;
		desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

		HRESULT hr = dev->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_heap));
		if (FAILED(hr))
		{
			Msg("! R5: CreateDescriptorHeap(CBV_SRV_UAV) failed 0x%08x", hr);
			return false;
		}

		m_cpuStart = m_heap->GetCPUDescriptorHandleForHeapStart();
		m_gpuStart = m_heap->GetGPUDescriptorHandleForHeapStart();
		return true;
	}

	void GpuDescriptorHeap::Reset()
	{
		m_offset = 0;
	}

	D3D12_CPU_DESCRIPTOR_HANDLE GpuDescriptorHeap::AllocCPU(UINT count)
	{
		VERIFY(m_offset + count <= m_capacity);
		D3D12_CPU_DESCRIPTOR_HANDLE h = m_cpuStart;
		h.ptr += SIZE_T(m_offset) * m_size;
		m_offset += count;
		return h;
	}

	D3D12_GPU_DESCRIPTOR_HANDLE GpuDescriptorHeap::AllocGPU(UINT count)
	{
		// 与 AllocCPU 配对的 GPU 句柄（同一偏移已推进，回退计算）
		D3D12_GPU_DESCRIPTOR_HANDLE h = m_gpuStart;
		h.ptr += SIZE_T(m_offset - count) * m_size;
		return h;
	}

	// -----------------------------------------------------------------------
	// UploadRing
	// -----------------------------------------------------------------------

	bool UploadRing::Create(ID3D12Device* dev, UINT64 size)
	{
		m_size = size;

		D3D12_HEAP_PROPERTIES heapProps = {};
		heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

		D3D12_RESOURCE_DESC desc = {};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		desc.Width = size;
		desc.Height = 1;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.SampleDesc.Count = 1;
		desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

		HRESULT hr = dev->CreateCommittedResource(
			&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
			D3D12_RESOURCE_STATE_GENERIC_READ,
			nullptr, IID_PPV_ARGS(&m_buffer)
		);
		if (FAILED(hr))
		{
			Msg("! R5: UploadRing CreateCommittedResource failed 0x%08x", hr);
			return false;
		}

		hr = m_buffer->Map(0, nullptr, &m_mapped);
		if (FAILED(hr))
		{
			Msg("! R5: UploadRing Map failed 0x%08x", hr);
			return false;
		}

		return true;
	}

	void* UploadRing::Alloc(UINT64 size, D3D12_GPU_VIRTUAL_ADDRESS& gpuAddr)
	{
		// 256 字节对齐（CBV 要求）
		size = (size + 255) & ~255ull;
		VERIFY(m_offset + size <= m_size);

		void* cpu = (u8*)m_mapped + m_offset;
		gpuAddr = m_buffer->GetGPUVirtualAddress() + m_offset;
		m_offset += size;
		return cpu;
	}

	// -----------------------------------------------------------------------
	// 模块级
	// -----------------------------------------------------------------------

	bool Init()
	{
		ID3D12Device* dev = dx12::GetDevice();
		if (!dev)
			return false;

		if (!g_gpuHeap.Create(dev, 4096))
			return false;
		if (!g_upload.Create(dev, 256 * 1024 * 1024))
			return false;

		Msg("* R5: resources initialized (4096 descriptors, 256MB upload ring)");
		return true;
	}

	void Shutdown()
	{
		g_upload = UploadRing{};
		g_gpuHeap = GpuDescriptorHeap{};
	}

	void BeginFrame()
	{
		g_gpuHeap.Reset();
		g_upload.Reset();
	}
}
