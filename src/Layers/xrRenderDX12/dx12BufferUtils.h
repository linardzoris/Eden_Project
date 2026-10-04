#ifndef	dx12BufferUtils_included
#define	dx12BufferUtils_included
#pragma once

namespace dx12BufferUtils
{
	HRESULT	CreateVertexBuffer(ID3DVertexBuffer** ppBuffer, const void* pData, UINT DataSize, bool bImmutable = true);
	HRESULT	CreateIndexBuffer(ID3DIndexBuffer** ppBuffer, const void* pData, UINT DataSize, bool bImmutable = true);
	HRESULT	CreateConstantBuffer(ID3DBuffer** ppBuffer, UINT DataSize);
	void	ConvertVertexDeclaration(const xr_vector<D3DVERTEXELEMENT9>& declIn, xr_vector<D3D_INPUT_ELEMENT_DESC>& declOut);
}

// 让共享层既有调用点（dx10BufferUtils::Xxx）无需改动即可走 DX12 实现
namespace dx10BufferUtils = dx12BufferUtils;

#endif	//	dx12BufferUtils_included
