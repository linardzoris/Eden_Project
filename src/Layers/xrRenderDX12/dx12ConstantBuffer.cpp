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
	: m_bChanged(true)
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
	if (!m_bChanged) return;
	if (!m_pBuffer || !m_pBufferData) return;

	// 从上传环分配新槽位（每帧重置），避免被后续 CB 覆写
	UINT64 gpu = 0;
	void* p = dx12::g_backend.ring.Alloc(m_uiBufferSize, 256, gpu);
	if (!p)
	{
		// 环未就绪（例如主菜单前）→ 回退到持久映射缓冲
		if (m_pBuffer->mapped)
		{
			CopyMemory(m_pBuffer->mapped, m_pBufferData, m_uiBufferSize);
			m_bChanged = false;
		}
		return;
	}

	CopyMemory(p, m_pBufferData, m_uiBufferSize);
	m_pBuffer->gpuVA = gpu;
	m_pBuffer->size = m_uiBufferSize;
	m_bChanged = false;
}
