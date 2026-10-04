#include "stdafx.h"
#include "DX12CommonTypes.h"
#include "dx12Backend.h"

namespace dx12
{
	Backend g_backend;

	//--------------------------------------------------------------------------
	// 设备 / 命令列表访问（由 xrEngine 的 Device_create_render_dx12.cpp 暴露）
	//--------------------------------------------------------------------------
	ID3D12Device* GetD3D12Device()
	{
		return (ID3D12Device*)HWRenderDevice;
	}

	ID3D12GraphicsCommandList* GetD3D12CmdList()
	{
		return dx12::GetCmdList();
	}

	//--------------------------------------------------------------------------
	// DescriptorHeap
	//--------------------------------------------------------------------------
	bool DescriptorHeap::Create(ID3D12Device* dev, D3D12_DESCRIPTOR_HEAP_TYPE type, UINT capacity, bool shaderVisible)
	{
		m_size = dev->GetDescriptorHandleIncrementSize(type);
		m_capacity = capacity;
		m_offset = 0;

		D3D12_DESCRIPTOR_HEAP_DESC desc = {};
		desc.Type = type;
		desc.NumDescriptors = capacity;
		desc.Flags = shaderVisible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
		desc.NodeMask = 0;

		if (FAILED(dev->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_heap))))
		{
			Msg("! DX12: CreateDescriptorHeap failed (type=%d, count=%u)", (int)type, capacity);
			return false;
		}

		m_cpuStart = m_heap->GetCPUDescriptorHandleForHeapStart();
		if (shaderVisible)
			m_gpuStart = m_heap->GetGPUDescriptorHandleForHeapStart();
		return true;
	}

	bool DescriptorHeap::Alloc(UINT count, D3D12_CPU_DESCRIPTOR_HANDLE& cpu, D3D12_GPU_DESCRIPTOR_HANDLE& gpu)
	{
		if (m_offset + count > m_capacity)
		{
			Msg("! DX12: descriptor heap overflow (%u + %u > %u)", m_offset, count, m_capacity);
			return false;
		}
		cpu = CPU(m_offset);
		gpu = GPU(m_offset);
		m_offset += count;
		return true;
	}

	bool DescriptorHeap::AllocPersistent(UINT count, D3D12_CPU_DESCRIPTOR_HANDLE& cpu)
	{
		if (m_persistOffset + count > m_capacity)
		{
			Msg("! DX12: persistent descriptor heap overflow (%u + %u > %u)", m_persistOffset, count, m_capacity);
			return false;
		}
		cpu = CPU(m_persistOffset);
		m_persistOffset += count;
		return true;
	}

	D3D12_CPU_DESCRIPTOR_HANDLE DescriptorHeap::CPU(UINT index) const	{
		D3D12_CPU_DESCRIPTOR_HANDLE h = m_cpuStart;
		h.ptr += (SIZE_T)index * m_size;
		return h;
	}

	D3D12_GPU_DESCRIPTOR_HANDLE DescriptorHeap::GPU(UINT index) const
	{
		D3D12_GPU_DESCRIPTOR_HANDLE h = m_gpuStart;
		h.ptr += (UINT64)index * m_size;
		return h;
	}

	//--------------------------------------------------------------------------
	// UploadRing
	//--------------------------------------------------------------------------
	bool UploadRing::Create(ID3D12Device* dev, UINT64 size)
	{
		m_size = size;

		D3D12_HEAP_PROPERTIES heapProps = {};
		heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

		D3D12_RESOURCE_DESC resDesc = {};
		resDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		resDesc.Width = size;
		resDesc.Height = 1;
		resDesc.DepthOrArraySize = 1;
		resDesc.MipLevels = 1;
		resDesc.Format = DXGI_FORMAT_UNKNOWN;
		resDesc.SampleDesc.Count = 1;
		resDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

		if (FAILED(dev->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &resDesc,
			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_buffer))))
		{
			Msg("! DX12: upload ring create failed (%llu bytes)", size);
			return false;
		}

		D3D12_RANGE readRange = { 0, 0 };
		if (FAILED(m_buffer->Map(0, &readRange, &m_mapped)))
		{
			Msg("! DX12: upload ring map failed");
			return false;
		}
		return true;
	}

	void* UploadRing::Alloc(UINT64 size, UINT64 align, D3D12_GPU_VIRTUAL_ADDRESS& gpuAddr)
	{
		UINT64 offset = (m_offset + (align - 1)) & ~(align - 1);
		if (offset + size > m_size)
		{
			Msg("! DX12: upload ring overflow (%llu + %llu > %llu)", offset, size, m_size);
			gpuAddr = 0;
			return nullptr;
		}
		m_offset = offset + size;
		gpuAddr = m_buffer->GetGPUVirtualAddress() + offset;
		return (u8*)m_mapped + offset;
	}

	//--------------------------------------------------------------------------
	// 根签名 / 静态采样器
	//--------------------------------------------------------------------------
	static const UINT kNumSRVSlots = 16;
	static const UINT kNumCBVSlots = 14;	// b0..b13

	D3D12_STATIC_SAMPLER_DESC GetStaticSampler(UINT slot)
	{
		D3D12_STATIC_SAMPLER_DESC s = {};
		s.Filter = D3D12_FILTER_ANISOTROPIC;
		s.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		s.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		s.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		s.MipLODBias = 0.0f;
		s.MaxAnisotropy = 16;
		s.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
		s.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
		s.MinLOD = 0.0f;
		s.MaxLOD = D3D12_FLOAT32_MAX;
		s.ShaderRegister = slot;
		s.RegisterSpace = 0;
		s.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		return s;
	}

	bool Init(ID3D12Device* dev)
	{
		if (g_backend.valid) return true;

		if (!g_backend.cbvSrvUav.Create(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4096, true)) return false;
		if (!g_backend.sampler.Create(dev, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 256, true)) return false;
		if (!g_backend.ring.Create(dev, 256ull * 1024 * 1024)) return false;

		if (!g_backend.persistentSrv.Create(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 8192, false)) return false;
		if (!g_backend.persistentUav.Create(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 512, false)) return false;
		if (!g_backend.persistentRtv.Create(dev, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 512, false)) return false;
		if (!g_backend.persistentDsv.Create(dev, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 512, false)) return false;

		g_backend.rtvDescriptorSize = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
		g_backend.dsvDescriptorSize = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

		// ---- 根签名 ----
		D3D12_ROOT_PARAMETER params[2] = {};
		// [0] CBV 描述符表：b0..b13
		D3D12_DESCRIPTOR_RANGE cbvRange = {};
		cbvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
		cbvRange.NumDescriptors = kNumCBVSlots;
		cbvRange.BaseShaderRegister = 0;
		cbvRange.RegisterSpace = 0;
		cbvRange.OffsetInDescriptorsFromTableStart = 0;
		params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		params[0].DescriptorTable.NumDescriptorRanges = 1;
		params[0].DescriptorTable.pDescriptorRanges = &cbvRange;
		params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		// [1] SRV 描述符表：t0..t15
		D3D12_DESCRIPTOR_RANGE srvRange = {};
		srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
		srvRange.NumDescriptors = kNumSRVSlots;
		srvRange.BaseShaderRegister = 0;
		srvRange.RegisterSpace = 0;
		srvRange.OffsetInDescriptorsFromTableStart = 0;
		params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		params[1].DescriptorTable.NumDescriptorRanges = 1;
		params[1].DescriptorTable.pDescriptorRanges = &srvRange;
		params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

		xr_vector<D3D12_STATIC_SAMPLER_DESC> samplers;
		samplers.reserve(kNumSRVSlots);
		for (UINT i = 0; i < kNumSRVSlots; ++i)
			samplers.push_back(GetStaticSampler(i));

		D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
		rsDesc.NumParameters = 2;
		rsDesc.pParameters = params;
		rsDesc.NumStaticSamplers = (UINT)samplers.size();
		rsDesc.pStaticSamplers = samplers.data();
		rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

		ComPtr<ID3DBlob> sig, err;
		if (FAILED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)))
		{
			Msg("! DX12: D3D12SerializeRootSignature failed: %s", err ? (LPCSTR)err->GetBufferPointer() : "?");
			return false;
		}
		if (FAILED(dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&g_backend.rootSignature))))
		{
			Msg("! DX12: CreateRootSignature failed");
			return false;
		}

		g_backend.valid = true;
		Msg("* DX12: backend initialized (root sig + heaps + 256MB ring)");
		return true;
	}

	void Shutdown()
	{
		g_backend.psoCache.clear();
		g_backend.rootSignature.Reset();
		g_backend.valid = false;
	}

	void BackendBeginFrame()
	{
		g_backend.cbvSrvUav.Reset();
		g_backend.sampler.Reset();
		g_backend.ring.Reset();
	}

	void BackendEndFrame()
	{
	}

	ID3D12RootSignature* GetRootSignature()
	{
		return g_backend.rootSignature.Get();
	}
}
