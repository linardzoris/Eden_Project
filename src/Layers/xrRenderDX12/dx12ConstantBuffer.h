#ifndef	dx12ConstantBuffer_included
#define	dx12ConstantBuffer_included
#pragma once

struct	R_constant;
struct	R_constant_load;

//==============================================================================
// dx12ConstantBuffer  (对应 xrRenderDX10/dx10ConstantBuffer.h)
//
// DX12 把 CB 放在上传环里：Flush() 每次从环分配新槽位并拷贝数据，
// GetBuffer() 返回的 dx12Buffer 的 gpuVA 随之更新。
//==============================================================================
class dx12ConstantBuffer : public xr_resource_named
{
public:
	dx12ConstantBuffer(ID3DShaderReflectionConstantBuffer* pTable);
	~dx12ConstantBuffer();

	bool			Similar(dx12ConstantBuffer& _in);
	ID3DBuffer*		GetBuffer() { return m_pBuffer; }

	void			Flush();

	//	Set copy data into constant buffer
	void			set(R_constant* C, R_constant_load& L, const Fmatrix& A);
	void			set(R_constant* C, R_constant_load& L, const Fvector4& A);
	void			set(R_constant* C, R_constant_load& L, float A);
	void			set(R_constant* C, R_constant_load& L, int A);
	void			seta(R_constant* C, R_constant_load& L, u32 e, const Fmatrix& A);
	void			seta(R_constant* C, R_constant_load& L, u32 e, const Fvector4& A);

	void*			AccessDirect(R_constant_load& L, u32 DataSize);

private:
	Fvector4*		Access(u16	offset);

private:
	shared_str							m_strBufferName;
	D3D_CBUFFER_TYPE					m_eBufferType;

	u32									m_uiMembersCRC;
	xr_vector<D3D_SHADER_TYPE_DESC>		m_MembersList;
	xr_vector<shared_str>				m_MembersNames;

	ID3DBuffer*							m_pBuffer;
	u32									m_uiBufferSize;
	void*								m_pBufferData;
	bool								m_bChanged;
	// 上次写入的环代次：CBV 必须每帧重新指向本帧环段的新槽位，
	// 否则槽位被环回收复用后着色器会读到别的 draw 的常量（光照/循环上限全乱）。
	u32									m_flushSerial;

	static const u32					lineSize = sizeof(Fvector4);

	dx12ConstantBuffer(const dx12ConstantBuffer&);
	dx12ConstantBuffer& operator=(dx12ConstantBuffer&);
};

typedef	resptr_core<dx12ConstantBuffer, resptr_base<dx12ConstantBuffer> > ref_cbuffer;

#endif	//	dx12ConstantBuffer_included
