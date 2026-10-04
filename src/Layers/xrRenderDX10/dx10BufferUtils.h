#ifndef	dx10BufferUtils_included
#define	dx10BufferUtils_included
#pragma once

#ifndef USE_DX12
// DX12 构建下由 xrRenderDX12/dx12BufferUtils.h 提供同名命名空间（别名），
// 此处不再重复声明，避免命名空间重定义。
namespace dx10BufferUtils
{
HRESULT	CreateVertexBuffer( ID3DVertexBuffer** ppBuffer, const void* pData, UINT DataSize, bool bImmutable = true);
HRESULT	CreateIndexBuffer( ID3DIndexBuffer** ppBuffer, const void* pData, UINT DataSize, bool bImmutable = true);
HRESULT	CreateConstantBuffer( ID3DBuffer** ppBuffer, UINT DataSize);
void	ConvertVertexDeclaration( const xr_vector<D3DVERTEXELEMENT9> &declIn, xr_vector<D3D_INPUT_ELEMENT_DESC> &declOut);
};
#endif // USE_DX12

#endif	//	dx10BufferUtils_included