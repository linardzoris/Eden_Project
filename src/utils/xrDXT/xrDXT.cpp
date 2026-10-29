#include "StdAfx.h"
#include "xrDXT.h"
#include <magic_enum/magic_enum.hpp>

#ifdef IXR_WINDOWS
#include <RedImage/RedImage.hpp>
#endif

#include "../../Layers/xrRender/ETextureParams.h"

extern int DXTCompressImageRI(LPCSTR out_name, u8* raw_data, u32 w, u32 h, u32 pitch, STextureParams* fmt, u32 depth);
extern int DXTCompressImageNVTT(LPCSTR out_name, u8* raw_data, u32 w, u32 h, u32 pitch, STextureParams* fmt, u32 depth);
extern int DXTCompressImageDirectXTex(LPCSTR out_name, u8* raw_data, u32 w, u32 h, u32 pitch, STextureParams* fmt, u32 depth);
extern int DXTCompressBump(LPCSTR out_name, u8* raw_data, u8* normal_map, u32 w, u32 h, u32 pitch, STextureParams* fmt, u32 depth);

#ifdef IXR_WINDOWS
void DXTUtils::Converter::MakeTGA(xr_path From, xr_path To)
{
	RedImageTool::RedImage Surface;
	if (Surface.LoadFromFile(From.xstring().data()))
	{
		Surface.Convert(RedImageTool::RedTexturePixelFormat::R8G8B8A8);
		Surface.SaveToTga(To.xstring().data());
	}
}

void DXTUtils::Converter::MakeTGA(u32 w, u32 h, U32Vec& From, xr_path To)
{
	RedImageTool::RedImage Surface;
	Surface.Create(w, h);

	for (u32 y = 0; y < h; y++)
	{
		for (u32 x = 0; x < w; x++)
		{
			Surface.SetPixel(From[y * w + x], x, y);
		}
	}

	Surface.Convert(RedImageTool::RedTexturePixelFormat::R8G8B8A8);
	Surface.SwapRB();


	std::filesystem::path dir = To.parent_path();

	if (!dir.empty() && (!std::filesystem::exists(dir) || !std::filesystem::is_directory(dir)))
	{
		std::filesystem::create_directories(dir);
	}

	Surface.SaveToTga(To.xstring().data());
}

void DXTUtils::Converter::MakePNG(xr_path From, xr_path To)
{
	RedImageTool::RedImage Surface;
	if (Surface.LoadFromFile(From.xstring().data()))
	{
		Surface.Convert(RedImageTool::RedTexturePixelFormat::R8G8B8A8);
		Surface.SaveToPng(To.xstring().data());
	}
}
#endif

// Regular image compression dispatch (shared by ttImage/ttCubeMap/ttNormalMap/ttTerrain
// and PBR bump passthrough)
static int CompressImageDispatch(LPCSTR out_name, u8* raw_data, u32 w, u32 h, u32 pitch, STextureParams* fmt, u32 depth)
{
#ifdef IXR_WINDOWS
	// BC7 through DirectXTex: multithreaded block compression,
	// the old NVTT+RedImage round-trip was single-threaded and very slow
	if (fmt->fmt == STextureParams::tfBC7)
		return DXTCompressImageDirectXTex(out_name, raw_data, w, h, pitch, fmt, depth);
#endif

	switch (fmt->mip_filter)
	{
		case STextureParams::kMIPFilterAdvanced:
		case STextureParams::kMIPFilterBox:
		case STextureParams::kMIPFilterTriangle:
		case STextureParams::kMIPFilterKaiser:
		{
			return DXTCompressImageNVTT(out_name, raw_data, w, h, pitch, fmt, depth);
		}

		case STextureParams::kMIPFilterPoint:
		case STextureParams::kMIPFilterCubic:
		case STextureParams::kMIPFilterCatrom:
		case STextureParams::kMIPFilterMitchell:
			return DXTCompressImageRI(out_name, raw_data, w, h, pitch, fmt, depth);
			
		case STextureParams::kMIPFilterGaussian:
		case STextureParams::kMIPFilterSinc	:
		case STextureParams::kMIPFilterBessel:
		case STextureParams::kMIPFilterHanning:
		case STextureParams::kMIPFilterHamming:
		case STextureParams::kMIPFilterBlackman:
		{
			Msg("Unsupported mip filter in %s!", out_name);
			fmt->mip_filter = STextureParams::kMIPFilterKaiser;
			return DXTCompressImageNVTT(out_name, raw_data, w, h, pitch, fmt, depth);
		}
	}

	return -1;
}

int DXT_API DXTUtils::Compress(const char* out_name, u8* raw_data, u8* normal_map, u32 w, u32 h, u32 pitch, STextureParams* fmt, u32 depth)
{
	switch (fmt->type) 
	{
	case STextureParams::ttImage:
	case STextureParams::ttCubeMap:
	case STextureParams::ttNormalMap:
	case STextureParams::ttTerrain:
		return CompressImageDispatch(out_name, raw_data, w, h, pitch, fmt, depth);

	case STextureParams::ttBumpMap:
		// PBR bump maps are authored with their final channel layout:
		// compress the raw data as-is instead of the legacy height/gloss swizzle
		if (fmt->material == STextureParams::tmPBR_Material)
			return CompressImageDispatch(out_name, raw_data, w, h, pitch, fmt, depth);

		return DXTCompressBump(out_name, raw_data, normal_map, w, h, pitch, fmt, depth);
	default: NODEFAULT;
	}
	return -1;
}

#ifdef IXR_WINDOWS
U8Vec DXT_API DXTUtils::GitPixels(const char* FileName, u32 NewW, u32 NewH)
{
	U8Vec Pixels;

	RedImageTool::RedImage Img;
	Img.LoadFromFile(FileName);
	Img.ClearMipLevels();
	Img.Convert(RedImageTool::RedTexturePixelFormat::R8G8B8A8);
	Img.Scale(NewW, NewH, RedImageTool::RedResizeFilter::Cubicbspline);
	Img.SwapRB();

	Pixels.resize(NewH * NewW * 4);
	memcpy(Pixels.data(), *Img, NewH * NewW * 4);

	return Pixels;
}

DXTUtils::ImageInfo DXT_API DXTUtils::GitPixels(const char* FileName)
{
	U8Vec Pixels;

	RedImageTool::RedImage Img;
	Img.LoadFromFile(FileName);
	RedImageTool::RedTexturePixelFormat OldFormat = Img.GetFormat();
	
	Img.ClearMipLevels();
	Img.Convert(RedImageTool::RedTexturePixelFormat::R8G8B8A8);
	Img.SwapRB();

	size_t PixelCount = Img.GetWidth() * Img.GetHeight() * 4;
	Pixels.resize(PixelCount);
	memcpy(Pixels.data(), *Img, PixelCount);


	shared_str FormatText = magic_enum::enum_name(OldFormat).data();
	return { (u32)Img.GetWidth(), (u32)Img.GetHeight(), Pixels, FormatText };
}

void DXTUtils::Filter::Resize(u32* dst, u32 dstW, u32 dstH, u32* out, u32 outW, u32 outH)
{
	RedImageTool::RedImage Surface;
	Surface.Create(dstW, dstH);

	for (u32 y = 0; y < dstH; y++)
	{
		for (u32 x = 0; x < dstW; x++)
		{
			Surface.SetPixel(dst[y * dstW + x], x, y);
		}
	}

	Surface.Convert(RedImageTool::RedTexturePixelFormat::R8G8B8A8);
	//Surface.SwapRB();
	Surface.Scale(outW, outH, RedImageTool::RedResizeFilter::Default);

	size_t PixelCount = Surface.GetWidth() * Surface.GetHeight() * 4;
	memcpy(out, *Surface, PixelCount);
}
#endif