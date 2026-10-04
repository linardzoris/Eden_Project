#pragma once

//==============================================================================
// dx12TextureUtils.h
//
// DX12 纹理创建辅助：把 DirectXTex 的 ScratchImage 直接建到 D3D12 纹理
// （UPLOAD 堆 + 逐子资源按 256 对齐 pitch 拷贝），替代 DirectXTex 的 D3D11 版
// CreateTextureEx。
//==============================================================================

#include <DirectXTex.h>

namespace dx12TextureUtils
{
	// 用 ScratchImage 的首个子资源起（mipOffset 指定加载的 LOD 起始层）创建纹理
	HRESULT	CreateTexture2DFromScratch(const DirectX::ScratchImage& image,
		const DirectX::TexMetadata& meta,
		size_t firstSubresource,
		ID3DTexture2D** ppTexture);
}
