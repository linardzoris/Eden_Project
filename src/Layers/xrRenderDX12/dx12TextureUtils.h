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

	// 3D 体纹理：本包 DirectXTex 把 TEXTURE3D 展开为"每深度切片一个 Image"，
	// 按 mip 顺序连续排列（每 mip 的切片数 = max(1, depth>>mip)），且同 mip 内
	// 各切片像素连续。创建 TEXTURE3D 资源并逐 mip 整体上传（subresource=mip）。
	HRESULT	CreateTexture3DFromScratch(const DirectX::ScratchImage& image,
		const DirectX::TexMetadata& meta,
		size_t firstSubresource,
		ID3DTexture3D** ppTexture);
}
