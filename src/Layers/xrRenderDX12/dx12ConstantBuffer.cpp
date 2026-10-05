#include "stdafx.h"
#include "dx12ConstantBuffer.h"
#include "dx12BufferUtils.h"
#include "dx12Backend.h"
#include "../xrRender/dxRenderDeviceRender.h"

dx12ConstantBuffer::~dx12ConstantBuffer()
{
	// 必须从资源管理器注销：与 DX10 一致（DX10 析构首行即调用此接口）。
	// 若省略，已释放的常量缓冲仍留在 v_constant_buffer 中，后续
	// _CreateConstantBuffer 去重遍历时会解引用悬垂指针并把它当作可复用对象
	// 返回，最终导致 ref_cbuffer 二次释放（xr_delete<dx12ConstantBuffer> 崩溃）。
	DEV->_DeleteConstantBuffer(this);
	_RELEASE(m_pBuffer);
	xr_free(m_pBufferData);
}

dx12ConstantBuffer::dx12ConstantBuffer(ID3DShaderReflectionConstantBuffer* pTable)
		: m_bChanged(true), m_flushSerial(0)
{
	D3D_SHADER_BUFFER_DESC Desc;
	CHK_DX(pTable->GetDesc(&Desc));

	m_strBufferName._set(Desc.Name);
	m_eBufferType = Desc.Type;
	m_uiBufferSize = Desc.Size;

	m_MembersList.resize(Desc.Variables);
	m_MembersNames.resize(Desc.Variables);
	for (u32 i = 0; i < Desc.Variables; ++i)
	{
		ID3DShaderReflectionVariable* pVar;
		ID3DShaderReflectionType* pType;
		D3D_SHADER_VARIABLE_DESC var_desc;

		pVar = pTable->GetVariableByIndex(i);
		VERIFY(pVar);
		pType = pVar->GetType();
		VERIFY(pType);
		pType->GetDesc(&m_MembersList[i]);
		CHK_DX(pVar->GetDesc(&var_desc));
		m_MembersNames[i] = var_desc.Name;
	}

	m_uiMembersCRC = crc32(&m_MembersList[0], Desc.Variables * sizeof(m_MembersList[0]));

	R_CHK(dx12BufferUtils::CreateConstantBuffer(&m_pBuffer, Desc.Size));
	VERIFY(m_pBuffer);
	m_pBufferData = xr_malloc(Desc.Size);
	VERIFY(m_pBufferData);
}

bool dx12ConstantBuffer::Similar(dx12ConstantBuffer& _in)
{
	if (m_strBufferName._get() != _in.m_strBufferName._get()) return false;
	if (m_eBufferType != _in.m_eBufferType) return false;
	if (m_uiMembersCRC != _in.m_uiMembersCRC) return false;
	if (m_MembersList.size() != _in.m_MembersList.size()) return false;
	if (memcmp(&m_MembersList[0], &_in.m_MembersList[0], m_MembersList.size() * sizeof(m_MembersList[0]))) return false;
	VERIFY(m_MembersNames.size() == _in.m_MembersNames.size());
	int iMemberNum = (int)m_MembersNames.size();
	for (int i = 0; i < iMemberNum; ++i)
		if (m_MembersNames[i].c_str() != _in.m_MembersNames[i].c_str()) return false;
	return true;
}

void dx12ConstantBuffer::Flush()
	{
		if (!m_pBuffer || !m_pBufferData) return;

		// 每帧都要从"本帧环段"重新分配槽位并写入：即使数值没变也不能跳过。
		// 旧实现只看 m_bChanged，数值不变的 CB 会一直沿用上一次的 gpuVA，
		// 而那块环区域会在同 slot 的下一帧被回收复用 → 着色器读到别的 draw 的
		// 常量（光照、循环上限全乱，甚至把 GPU 拖进 TDR → DEVICE_HUNG）。
		const u32 serial = dx12::FrameSerial();
		if (!m_bChanged && m_flushSerial == serial) return;

		// 与建 CBV 的渲染线程互斥：gpuVA/size 是普通字段，并发写会出现撕裂值
		//（实测 gpuVA 低字节被污染 → 非法 CBV → 调试层异常/设备移除）
		std::lock_guard<std::mutex> cbLock(dx12::ConstantBufferMutex());

		UINT64 gpu = 0;
		void* p = dx12::g_backend.ring.Alloc(m_uiBufferSize, 256, gpu);
		if (!p)
		{
			// 环不可用/已满：回退到该 CB 自身上传缓冲（内容即刻写入并同步 gpuVA，
			// 不能只写数据不改 gpuVA——那会让 CBV 继续指向已被复用的环区域）
			if (m_pBuffer->mapped && m_pBuffer->resource)
			{
				CopyMemory(m_pBuffer->mapped, m_pBufferData, m_uiBufferSize);
				m_pBuffer->gpuVA = m_pBuffer->resource->GetGPUVirtualAddress();
				m_bChanged = false;
				m_flushSerial = serial;
			}
			return;
		}

		CopyMemory(p, m_pBufferData, m_uiBufferSize);
		m_pBuffer->gpuVA = gpu;
		m_pBuffer->size = m_uiBufferSize;
		m_bChanged = false;
		m_flushSerial = serial;
	}
