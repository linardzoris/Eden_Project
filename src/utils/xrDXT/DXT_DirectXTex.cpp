#include "StdAfx.h"

#ifdef IXR_WINDOWS
#include "../../Layers/xrRender/ETextureParams.h"
#include <DirectXTex.h>

extern u32* Build32MipLevel(u32& _w, u32& _h, u32& _p, u32* pdwPixelSrc, STextureParams* fmt, float blend);

// Builds the mip chain with the engine's own advanced filter (keeps fade support),
// mirroring GenerateAdvancedFilterMipMaps from the NVTT backend.
static void FillAdvancedMipChain(DirectX::ScratchImage& src, u32 w, u32 h, u32 pitch, u8* raw_data, STextureParams* fmt)
{
	auto GetPowerOf2Plus1Lambda = [](u32 v)
	{
		u32 cnt = 0;
		while (v) { v >>= 1; cnt++; }
		return cnt;
	};

	const u32 numMipmaps = GetPowerOf2Plus1Lambda(std::min(w, h));
	VERIFY(SUCCEEDED(src.Initialize2D(DirectX::DXGI_FORMAT_R8G8B8A8_UNORM, w, h, 1, numMipmaps)));

	u32 dwW = w;
	u32 dwH = h;
	u32 dwP = pitch;
	u32* pLastMip = xr_alloc<u32>(w * h * 4);
	memcpy(pLastMip, raw_data, w * h * 4);

	float inv_fade = clampr(1.f - float(fmt->fade_amount) / 100.f, 0.f, 1.f);
	float blend = fmt->flags.is_any(STextureParams::flFadeToColor | STextureParams::flFadeToAlpha) ? inv_fade : 1.f;

	for (u32 i = 0; i < numMipmaps; i++)
	{
		const DirectX::Image& img = src.GetImages()[i];
		VERIFY(img.width == dwW && img.height == dwH && img.rowPitch == dwW * 4);
		memcpy(img.pixels, pLastMip, dwW * dwH * 4);

		if (i + 1 < numMipmaps)
		{
			u32* pNewMip = Build32MipLevel(dwW, dwH, dwP, pLastMip, fmt, (i + 1) < fmt->fade_delay ? 0.f : 1.f - blend);
			xr_free(pLastMip);
			pLastMip = pNewMip;
		}
	}
	xr_free(pLastMip);
}

static void FillSingleImage(DirectX::ScratchImage& src, u32 w, u32 h, u32 pitch, u8* raw_data)
{
	VERIFY(SUCCEEDED(src.Initialize2D(DirectX::DXGI_FORMAT_R8G8B8A8_UNORM, w, h, 1, 1)));
	const DirectX::Image& img = src.GetImages()[0];
	for (u32 row = 0; row < h; row++)
		memcpy(img.pixels + row * img.rowPitch, raw_data + row * pitch, w * 4);
}

// BC7 through DirectXTex: multithreaded block compression.
// Replaces the old NVTT(RGBA) -> RedImage(BC7) round-trip which was single-threaded
// and extremely slow for BC7, especially above 2048x2048.
int DXTCompressImageDirectXTex(LPCSTR out_name, u8* raw_data, u32 w, u32 h, u32 pitch, STextureParams* fmt, u32 depth)
{
	CTimer T;
	T.Start();
	R_ASSERT(0 != w && 0 != h);

	DirectX::ScratchImage src;
	if (fmt->flags.is(STextureParams::flGenerateMipMaps))
	{
		if (STextureParams::kMIPFilterAdvanced == fmt->mip_filter)
		{
			FillAdvancedMipChain(src, w, h, pitch, raw_data, fmt);
		}
		else
		{
			DirectX::TEX_FILTER_FLAGS filter = DirectX::TEX_FILTER_TRIANGLE;
			switch (fmt->mip_filter)
			{
			case STextureParams::kMIPFilterPoint:	filter = DirectX::TEX_FILTER_POINT; break;
			case STextureParams::kMIPFilterBox:		filter = DirectX::TEX_FILTER_BOX; break;
			case STextureParams::kMIPFilterTriangle:	filter = DirectX::TEX_FILTER_TRIANGLE; break;
			case STextureParams::kMIPFilterCubic:
			case STextureParams::kMIPFilterCatrom:
			case STextureParams::kMIPFilterMitchell:	filter = DirectX::TEX_FILTER_CUBIC; break;
			}

			FillSingleImage(src, w, h, pitch, raw_data);
			DirectX::ScratchImage mips;
			if (SUCCEEDED(DirectX::GenerateMipMaps(src.GetImages()[0], filter, 0, mips)))
				src = std::move(mips);
		}
	}
	else
	{
		FillSingleImage(src, w, h, pitch, raw_data);
	}

	DirectX::ScratchImage compressed;
	if (!SUCCEEDED(DirectX::Compress(src.GetImages(), src.GetImageCount(), src.GetMetadata(),
		DXGI_FORMAT_BC7_UNORM, DirectX::TEX_COMPRESS_PARALLEL, DirectX::TEX_THRESHOLD_DEFAULT, compressed)))
	{
		return 0;
	}

	if (!SUCCEEDED(DirectX::SaveToDDSFile(compressed.GetImages(), compressed.GetImageCount(), compressed.GetMetadata(),
		DirectX::DDS_FLAGS_NONE, std::filesystem::path(out_name).c_str())))
	{
		return 0;
	}

	Msg("* DXT: BC7 compressed '%s' [%ux%u, %u mip(s)] in %.3f sec", out_name, w, h, (u32)src.GetImageCount(), T.GetElapsed_sec());
	return 1;
}
#endif // IXR_WINDOWS
