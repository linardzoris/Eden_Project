#include "stdafx.h"
#include "dx12TextureUtils.h"
#include "dx12Types.h"
#include "dx12Backend.h"

namespace dx12TextureUtils
{
	static UINT AlignPitch(UINT pitch)
	{
		return (pitch + (D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1)) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
	}

	HRESULT	CreateTexture2DFromScratch(const DirectX::ScratchImage& image,
		const DirectX::TexMetadata& meta,
		size_t firstSubresource,
		ID3DTexture2D** ppTexture)
	{
		if (!ppTexture) return E_INVALIDARG;
		ID3D12Device* dev = dx12::GetD3D12Device();
		if (!dev) return E_FAIL;

		const bool bCube = (meta.IsCubemap());
		const UINT arraySize = (UINT)(bCube ? meta.arraySize * 6 : meta.arraySize);
		const UINT mipLevels = (UINT)meta.mipLevels;

		dx12Texture* tex = new dx12Texture();

		D3D12_HEAP_PROPERTIES heap = {};
		heap.Type = D3D12_HEAP_TYPE_DEFAULT;

		D3D12_RESOURCE_DESC rd = {};
		rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		rd.Width = meta.width;
		rd.Height = (UINT)meta.height;
		rd.DepthOrArraySize = (UINT16)arraySize;
		rd.MipLevels = (UINT16)mipLevels;
		rd.Format = meta.format;
		rd.SampleDesc.Count = 1;
		rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

		HRESULT hr = dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rd,
			D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&tex->resource));
		if (FAILED(hr)) { Msg("! DX12: texture create failed 0x%08x", hr); dx12::DumpDeviceErrors("tex_utils_create"); tex->Release(); return hr; }

		tex->desc.Width = (UINT)meta.width;
		tex->desc.Height = (UINT)meta.height;
		tex->desc.MipLevels = mipLevels;
		tex->desc.ArraySize = arraySize;
		tex->desc.Format = meta.format;
		tex->desc.SampleDesc.Count = 1;
		tex->desc.Usage = D3D_USAGE_DEFAULT;
		tex->desc.BindFlags = D3D_BIND_SHADER_RESOURCE;
		if (bCube)
			tex->desc.MiscFlags |= D3D_RESOURCE_MISC_TEXTURECUBE;
		tex->state = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;

		const size_t totalImages = image.GetImageCount();
	const DirectX::Image* allImages = image.GetImages();

	// firstSubresource：LOD 缩减（_DDS_2D 的 Reduce）时为跳过的源 mip 数。
	// 目标纹理按缩小后的 meta 创建，其 mip0 对应源 images[firstSubresource]，
	// 故上传基址必须偏移（与 DX11 的 GetImages()+mip_lod 对齐）；否则会把原始
	// 大尺寸（如 256）的 mip0 上传到缩小后（128）的目标，CopyTextureRegion 越界
	// 并触发 device removed。
	const size_t baseIdx = (firstSubresource < totalImages) ? firstSubresource : 0;
	const DirectX::Image* images = allImages + baseIdx;
	const size_t imageCount = totalImages - baseIdx;


	for (UINT a = 0; a < arraySize; ++a)
	{
		for (UINT m = 0; m < mipLevels; ++m)
		{
			size_t idx = (size_t)m + (size_t)a * mipLevels;
			if (idx >= imageCount) break;

			const DirectX::Image& src = images[idx];
			UINT subresource = m + a * mipLevels;

			dx12::UploadTextureSubresource(tex->resource.Get(), subresource,
				src.pixels, (UINT)src.rowPitch,
				(UINT)src.width, (UINT)src.height, 1,
				meta.format, meta.format);
		}
	}

R5RegisterResourceState(tex->resource.Get(), tex->state);
	*ppTexture = tex;
	return S_OK;
	}

	HRESULT	CreateTexture3DFromScratch(const DirectX::ScratchImage& image,
		const DirectX::TexMetadata& meta,
		size_t firstSubresource,
		ID3DTexture3D** ppTexture)
	{
		if (!ppTexture) return E_INVALIDARG;
		*ppTexture = nullptr;
		ID3D12Device* dev = dx12::GetD3D12Device();
		if (!dev) return E_FAIL;

		const UINT mipLevels = (UINT)(meta.mipLevels ? meta.mipLevels : 1);
		const UINT depth0 = (UINT)(meta.depth ? meta.depth : 1);

		dx12Texture* tex = new dx12Texture();
		tex->volDepth = depth0;

		D3D12_HEAP_PROPERTIES heap = {};
		heap.Type = D3D12_HEAP_TYPE_DEFAULT;

		D3D12_RESOURCE_DESC rd = {};
		rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
		rd.Width = meta.width;
		rd.Height = (UINT)meta.height;
		rd.DepthOrArraySize = (UINT16)depth0;
		rd.MipLevels = (UINT16)mipLevels;
		rd.Format = meta.format;
		rd.SampleDesc.Count = 1;
		rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

		HRESULT hr = dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rd,
			D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&tex->resource));
		if (FAILED(hr)) { Msg("! DX12: texture3d create failed 0x%08x", hr); dx12::DumpDeviceErrors("tex_utils_create3d"); tex->Release(); return hr; }

		tex->desc.Width = (UINT)meta.width;
		tex->desc.Height = (UINT)meta.height;
		tex->desc.MipLevels = mipLevels;
		tex->desc.Format = meta.format;
		tex->desc.SampleDesc.Count = 1;
		tex->desc.Usage = D3D_USAGE_DEFAULT;
		tex->desc.BindFlags = D3D_BIND_SHADER_RESOURCE;
		tex->state = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;

		const size_t totalImages = image.GetImageCount();
		const DirectX::Image* allImages = image.GetImages();

		// 体纹理不走 LOD Reduce（_DDS_CUBE 无 Reduce），firstSubresource 保留形参一致性
		const size_t baseIdx = (firstSubresource < totalImages) ? firstSubresource : 0;
		const DirectX::Image* images = allImages + baseIdx;
		const size_t imageCount = totalImages - baseIdx;

		size_t imgCursor = 0;
		for (UINT m = 0; m < mipLevels; ++m)
		{
			const UINT w = (UINT)(meta.width >> m) ? (UINT)(meta.width >> m) : 1u;
			const UINT h = (UINT)(meta.height >> m) ? (UINT)(meta.height >> m) : 1u;
			const UINT d = depth0 >> m;
			const UINT slices = d ? d : 1u;

			if (imgCursor + slices > imageCount)
			{
				Msg("! DX12: texture3d images exhausted mip=%u need=%u have=%u total=%u -> stop",
					m, slices, (UINT)(imageCount - imgCursor), (UINT)imageCount);
				break;
			}
			const DirectX::Image& first = images[imgCursor];
			if ((UINT)first.width != w || (UINT)first.height != h)
			{
				Msg("! DX12: texture3d slice dim mismatch mip=%u expect=%ux%u got=%ux%u -> stop",
					m, w, h, (UINT)first.width, (UINT)first.height);
				break;
			}

			// 同 mip 各切片像素连续（offsets 以 slicePitch 步进），一次体上传整 mip。
			dx12::UploadTextureSubresource(tex->resource.Get(), m,
				first.pixels, (UINT)first.rowPitch,
				w, h, slices, meta.format, meta.format);

			imgCursor += slices;
		}

		R5RegisterResourceState(tex->resource.Get(), tex->state);
		*ppTexture = tex;
		return S_OK;
	}
}
