#include "stdafx.h"
#include "UIImageConverterForm.h"
#include "imgui.h"
#include "../xrEUI/EditorLocalization.h"
#include <d3d11.h> // must precede DirectXTex.h: GPU Compress overloads are
                   // only declared when __d3d11_h__ is defined
#include <DirectXTex.h>
#include <shlobj.h>

// ----------------------------------------------------------------------------
// Image Converter (see header + _doc/_code_temp/image_converter/DESIGN3.md)
// ----------------------------------------------------------------------------

UIImageConverterForm* UIImageConverterForm::Form = nullptr;

UIImageConverterForm::UIImageConverterForm()
{
}

UIImageConverterForm::~UIImageConverterForm()
{
}

void UIImageConverterForm::Update()
{
	if (!Form)
		return;

	if (Form->bOpen)
	{
		ImGui::SetNextWindowSize(ImVec2(720, 640), ImGuiCond_FirstUseEver);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(520, 380));
		if (ImGui::Begin(Loc("Image Converter"), &Form->bOpen))
		{
			Form->Draw();
		}
		ImGui::End();
		ImGui::PopStyleVar();
	}
	else
	{
		xr_delete(Form);
	}
}

void UIImageConverterForm::Show()
{
	if (Form)
	{
		Form->bOpen = true;
		return;
	}
	Form = new UIImageConverterForm();
}

// ----------------------------------------------------------------------------
// small helpers
// ----------------------------------------------------------------------------

static const char* RoleName(u32 role)
{
	switch (role)
	{
	case UIImageConverterForm::RoleD:   return "D";
	case UIImageConverterForm::RoleN:   return "N";
	case UIImageConverterForm::RoleRHA: return "RHA";
	case UIImageConverterForm::RoleRMA: return "RMA";
	default:                            return "?";
	}
}

static const char* FmtName(u32 fmt)
{
	switch (fmt)
	{
	case UIImageConverterForm::FmtBC1:   return "DXT1 (BC1)";
	case UIImageConverterForm::FmtBC1a:  return "DXT1a (BC1 alpha)";
	case UIImageConverterForm::FmtBC2:   return "DXT3 (BC2)";
	case UIImageConverterForm::FmtBC3:   return "DXT5 (BC3)";
	case UIImageConverterForm::FmtBC5:   return "BC5";
	case UIImageConverterForm::FmtRGBA8: return "R8G8B8A8";
	case UIImageConverterForm::FmtBC7:   return "BC7";
	default:                             return "?";
	}
}

// true when the target size is not one of the square presets (custom mode)
static bool IsCustomSize(int w, int h)
{
	static const int presets[] = { 256, 512, 1024, 2048, 4096 };
	if (w != h)
		return true;
	for (int p : presets)
		if (w == p)
			return false;
	return true;
}

// ANSI -> wide (engine paths are ANSI, CP_ACP matches the rest of the codebase)
static std::wstring ToWide(const char* s)
{
	int n = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
	std::wstring w(n > 0 ? n - 1 : 0, L'\0');
	if (n > 0)
		MultiByteToWideChar(CP_ACP, 0, s, -1, w.data(), n);
	return w;
}

static std::string ToNarrow(const wchar_t* s)
{
	int n = WideCharToMultiByte(CP_ACP, 0, s, -1, nullptr, 0, nullptr, nullptr);
	std::string a(n > 0 ? n - 1 : 0, '\0');
	if (n > 0)
		WideCharToMultiByte(CP_ACP, 0, s, -1, a.data(), n, nullptr, nullptr);
	return a;
}

// row accessor for RGBA32F scratch images
static const float* RowF(const DirectX::Image& im, size_t y)
{
	return reinterpret_cast<const float*>(im.pixels + im.rowPitch * y);
}

static float* RowW(const DirectX::Image& im, size_t y)
{
	return reinterpret_cast<float*>(im.pixels + im.rowPitch * y);
}

// lazily created D3D11 device for the DirectCompute BC compressor; nullptr
// when unavailable (CPU path with TEX_COMPRESS_PARALLEL is the fallback)
static ID3D11Device* AcquireGPUDevice()
{
	static ID3D11Device* dev = nullptr;
	static bool tried = false;
	if (!tried)
	{
		tried = true;
		const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0 };
		HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
			levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &dev, nullptr, nullptr);
		if (FAILED(hr))
			dev = nullptr;
	}
	return dev;
}

// binary alpha cutout applied to every mip of an R8G8B8A8 chain; used on the
// GPU path where Compress only carries the BC1 threshold internally
static void ThresholdAlpha8(DirectX::ScratchImage& img, float thr)
{
	for (size_t i = 0; i < img.GetImageCount(); ++i)
	{
		const DirectX::Image& im = img.GetImages()[i];
		for (size_t y = 0; y < im.height; ++y)
		{
			u8* row = im.pixels + im.rowPitch * y;
			for (size_t x = 0; x < im.width; ++x)
				row[x * 4 + 3] = (row[x * 4 + 3] / 255.f) < thr ? 0 : 255;
		}
	}
}

// header-only read used at import for the multiple-of-4 source size gate
static bool ReadSourceSize(const char* path, int& w, int& h)
{
	std::wstring wpath = ToWide(path);
	size_t len = strlen(path);
	DirectX::TexMetadata m;
	HRESULT hr;
	if (len >= 4 && _stricmp(path + len - 4, ".tga") == 0)
		hr = DirectX::GetMetadataFromTGAFile(wpath.c_str(), m);
	else
		hr = DirectX::GetMetadataFromWICFile(wpath.c_str(), DirectX::WIC_FLAGS_NONE, m);
	if (FAILED(hr))
		return false;
	w = (int)m.width;
	h = (int)m.height;
	return true;
}

// loads any tga/png as an RGBA32F float buffer (linear color space, no
// sRGB decode/encode anywhere in this pipeline)
static bool LoadFloat4(const char* path, DirectX::ScratchImage& out, xr_string& err)
{
	std::wstring wpath = ToWide(path);
	size_t len = strlen(path);
	bool tga = len >= 4 && _stricmp(path + len - 4, ".tga") == 0;
	DirectX::ScratchImage img;
	DirectX::TexMetadata meta;
	HRESULT hr;
	if (tga)
		hr = DirectX::LoadFromTGAFile(wpath.c_str(), &meta, img);
	else
		hr = DirectX::LoadFromWICFile(wpath.c_str(), DirectX::WIC_FLAGS_NONE, &meta, img);
	if (FAILED(hr))
	{
		char b[96];
		sprintf_s(b, sizeof(b), "load failed (0x%08X)", (u32)hr);
		err = b;
		return false;
	}
	hr = DirectX::Convert(*img.GetImage(0, 0, 0), DXGI_FORMAT_R32G32B32A32_FLOAT,
		DirectX::TEX_FILTER_DEFAULT, 0.f, out);
	if (FAILED(hr))
	{
		err = "convert to float4 failed";
		return false;
	}
	return true;
}

// Win32 multi-select open dialog (DESIGN3 v2-Q5: thin GetOpenFileNameA wrapper
// with a 64KB buffer; remembers the last directory for the session)
static bool OpenMultiFileDialog(xr_vector<xr_string>& out, xr_string& last_dir)
{
	static const DWORD BUF = 64 * 1024;
	xr_vector<char> buf(BUF, 0);

	OPENFILENAMEA ofn;
	ZeroMemory(&ofn, sizeof(ofn));
	char filter[] = "TGA images\0*.tga\0PNG images\0*.png\0All files\0*.*\0";
	ofn.lStructSize = sizeof(ofn);
	ofn.hwndOwner = GetForegroundWindow();
	ofn.lpstrFilter = filter;
	ofn.lpstrFile = buf.data();
	ofn.nMaxFile = BUF;
	ofn.lpstrTitle = "Import Stalker2 Textures";
	if (!last_dir.empty())
		ofn.lpstrInitialDir = last_dir.c_str();
	ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY |
		OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_NOCHANGEDIR;

	if (!GetOpenFileNameA(&ofn))
		return false;

	// explorer mode: single selection -> one full path; multiple selection ->
	// first token is the directory, the rest are file names
	const char* p = buf.data();
	if (!*p)
		return false;
	xr_vector<xr_string> toks;
	while (*p)
	{
		toks.push_back(p);
		p += strlen(p) + 1;
	}
	if (toks.size() == 1)
	{
		out.push_back(toks[0]);
		size_t s = toks[0].find_last_of("\\/");
		if (s != xr_string::npos)
			last_dir = toks[0].substr(0, s);
	}
	else
	{
		for (size_t i = 1; i < toks.size(); ++i)
			out.push_back(toks[0] + "\\" + toks[i]);
		last_dir = toks[0];
	}
	return !out.empty();
}

// native folder picker for the global output directory override.
// The engine main thread runs COM in MTA mode (xrCore.cpp), where the modern
// IFileDialog deadlocks; use SHBrowseForFolderA and only request the new
// dialog style when this thread successfully owns a COM STA apartment.
static bool PickFolder(xr_string& out)
{
	HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	bool com_owned = (hr == S_OK || hr == S_FALSE);
	bool new_style = com_owned;

	BROWSEINFOA bi = {};
	char display[MAX_PATH] = "";
	bi.hwndOwner = GetForegroundWindow();
	bi.pszDisplayName = display;
	bi.lpszTitle = "Select output folder";
	bi.ulFlags = BIF_RETURNONLYFSDIRS | (new_style ? BIF_NEWDIALOGSTYLE : 0);
	LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);
	bool ok = false;
	if (pidl)
	{
		char path[MAX_PATH] = "";
		if (SHGetPathFromIDListA(pidl, path))
		{
			out = path;
			ok = true;
		}
		CoTaskMemFree(pidl);
	}
	if (com_owned)
		CoUninitialize();
	return ok;
}

// ----------------------------------------------------------------------------
// grouping / provenance / gating
// ----------------------------------------------------------------------------

void UIImageConverterForm::RebuildGroups()
{
	// (lowercased base name) -> first-occurrence entry index per role
	xr_map<xr_string, int> idx[5]; // indexed by ERole
	for (int i = 0; i < (int)m_Entries.size(); ++i)
	{
		ConvEntry& e = m_Entries[i];
		e.comp_d = e.comp_n = e.comp_rha = e.comp_rma = -1;
		if (e.role == RoleUnknown)
			continue;
		xr_string key = e.base.c_str();
		xr_strlwr(key);
		auto& slot = idx[e.role];
		if (slot.find(key) == slot.end())
			slot[key] = i;
		else
			ELog.Msg(mtWarning, "Image Converter: duplicate %s group '%s', keeping first occurrence.",
				RoleName(e.role), e.base.c_str());
	}

	for (int self = 0; self < (int)m_Entries.size(); ++self)
	{
		ConvEntry& e = m_Entries[self];
		xr_string key = e.base.c_str();
		xr_strlwr(key);
		auto pick = [&](u32 role) -> int
		{
			auto it = idx[role].find(key);
			int i = it != idx[role].end() ? it->second : -1;
			return i == self ? -1 : i; // never pair with self
		};
		e.comp_d = pick(RoleD);
		e.comp_n = pick(RoleN);
		e.comp_rha = pick(RoleRHA);
		e.comp_rma = pick(RoleRMA);
		UpdateProvenance(e);
	}
}

void UIImageConverterForm::UpdateProvenance(ConvEntry& e)
{
	const ConvEntry* n = e.comp_n >= 0 ? &m_Entries[e.comp_n] : nullptr;
	const ConvEntry* rha = e.comp_rha >= 0 ? &m_Entries[e.comp_rha] : nullptr;
	auto set = [&](int ch, bool found, const char* text)
	{
		e.prov[ch].found = found;
		e.prov[ch].text = text;
	};

	switch (e.role)
	{
	case RoleD:
		set(0, true, "Albedo.r <self.r");
		set(1, true, "Albedo.g <self.g");
		set(2, true, "Albedo.b <self.b");
		set(3, true, "Transparency <self.a");
		break;
	case RoleN:
		if (rha)
			set(0, true, "Height <RHA.g");
		else if (e.comp_rma >= 0)
			set(0, true, "Height (50% gray) <const");
		else
			set(0, false, "Height NOT FOUND");
		set(1, true, "Normal.y <self.g");
		set(2, true, "0 (const)");
		set(3, true, "Normal.x <N.r");
		break;
	case RoleRHA:
		set(0, true, "0 (const)");
		set(1, n != nullptr, n ? "Roughness <N.a" : "Roughness NOT FOUND");
		set(2, true, "0 (const)");
		set(3, n != nullptr, n ? "AO <N.b" : "AO NOT FOUND");
		break;
	case RoleRMA:
		set(0, true, "Metallic <RMA.g");
		set(1, n != nullptr, n ? "Roughness <N.a" : "Roughness NOT FOUND");
		set(2, true, "0 (const)");
		set(3, n != nullptr, n ? "AO <N.b" : "AO NOT FOUND");
		break;
	default:
		set(0, false, "NOT FOUND");
		set(1, false, "NOT FOUND");
		set(2, false, "NOT FOUND");
		set(3, false, "NOT FOUND");
		break;
	}
}

bool UIImageConverterForm::CanApprove(const ConvEntry& e) const
{
	switch (e.role)
	{
	case RoleD:   return true;
	case RoleN:   return e.comp_rha >= 0 || e.comp_rma >= 0;
	case RoleRHA: return e.comp_n >= 0;
	case RoleRMA: return e.comp_n >= 0;
	default:      return false;
	}
}

xr_string UIImageConverterForm::OutputName(const ConvEntry& e) const
{
	switch (e.role)
	{
	case RoleD:   return e.base + ".dds";
	case RoleN:   return e.base + "_bump.dds";
	case RoleRHA: return e.base + "_bump#.dds";
	case RoleRMA: return e.base + "_bump#.dds";
	default:      return e.base + ".dds";
	}
}

// ----------------------------------------------------------------------------
// import
// ----------------------------------------------------------------------------

void UIImageConverterForm::DoImport()
{
	xr_vector<xr_string> files;
	if (!OpenMultiFileDialog(files, m_LastDir))
		return;

	u32 added = 0, rejected = 0, dup = 0;
	for (const xr_string& p : files)
	{
		size_t slash = p.find_last_of("\\/");
		xr_string name = p.substr(slash + 1);
		size_t dot = name.find_last_of('.');
		xr_string stem = dot != xr_string::npos ? name.substr(0, dot) : name;
		xr_string ext = dot != xr_string::npos ? name.substr(dot) : xr_string();

		if (_stricmp(ext.c_str(), ".tga") != 0 && _stricmp(ext.c_str(), ".png") != 0)
		{
			ELog.Msg(mtError, "Image Converter: unsupported extension '%s', skipped.", name.c_str());
			++rejected;
			continue;
		}

		// dedupe by full path
		bool exists = false;
		for (const ConvEntry& e : m_Entries)
			if (_stricmp(e.path.c_str(), p.c_str()) == 0) { exists = true; break; }
		if (exists) { ++dup; continue; }

		// source size gate: BC compression and mip chains require a
		// multiple-of-4 size (v3-Q2: warn AND reject)
		int w = 0, h = 0;
		if (!ReadSourceSize(p.c_str(), w, h))
		{
			ELog.Msg(mtError, "Image Converter: can't read '%s', skipped.", p.c_str());
			++rejected;
			continue;
		}
		if (w % 4 || h % 4)
		{
			ELog.Msg(mtError, "Image Converter: '%s' is %dx%d, size must be a multiple of 4, rejected.",
				name.c_str(), w, h);
			++rejected;
			continue;
		}

		ConvEntry e;
		e.path = p;
		e.name = name;
		e.role = RoleUnknown;
		// role suffixes indexed by ERole (slot 0 unused)
		static const char* sfx[5] = { "", "_D", "_N", "_RHA", "_RMA" };
		for (int r = RoleD; r <= RoleRMA; ++r)
		{
			size_t l = strlen(sfx[r]);
			if (stem.size() >= l && _stricmp(stem.c_str() + stem.size() - l, sfx[r]) == 0)
			{
				e.role = (u32)r;
				e.base = stem.substr(0, stem.size() - l);
				break;
			}
		}
		if (e.role == RoleUnknown)
		{
			e.base = stem;
			ELog.Msg(mtWarning, "Image Converter: unrecognized role suffix '%s', added as Unknown.", name.c_str());
		}
		e.src_w = w;
		e.src_h = h;
		// target defaults to the source size (no resize unless changed)
		e.target_w = w;
		e.target_h = h;
		e.fmt = e.role == RoleD ? FmtBC1 : FmtBC7;
		e.cutout = 0;
		m_Entries.push_back(e);
		++added;
	}

	RebuildGroups();

	if (!m_Entries.empty())
		SelectOnly((int)m_Entries.size() - 1);
	ELog.Msg(mtInformation, "Image Converter: imported %u, rejected %u, duplicates %u (list: %u).",
		added, rejected, dup, (u32)m_Entries.size());
}

// ----------------------------------------------------------------------------
// conversion pipeline
// ----------------------------------------------------------------------------

bool UIImageConverterForm::ConvertEntry(ConvEntry& e)
{
	e.error.clear();
	if (!CanApprove(e))
	{
		e.error = "configuration incomplete";
		return false;
	}

	// output directory: global override or the source file's folder
	xr_string dir;
	if (!m_ExportDir.empty())
		dir = m_ExportDir;
	else
	{
		size_t s = e.path.find_last_of("\\/");
		dir = s != xr_string::npos ? e.path.substr(0, s) : xr_string(".");
	}
	xr_string out_path = dir + "\\" + OutputName(e);

	// self source -> float4
	DirectX::ScratchImage self_img;
	if (!LoadFloat4(e.path.c_str(), self_img, e.error))
		return false;
	const DirectX::Image* self_im = self_img.GetImage(0, 0, 0);

	// companions (only the ones the recipe needs), aligned to self size
	DirectX::ScratchImage rha_img, n_img;
	const DirectX::Image* rha_im = nullptr;
	const DirectX::Image* n_im = nullptr;
	auto load_aligned = [&](int idx, DirectX::ScratchImage& store, const DirectX::Image*& im) -> bool
	{
		if (!LoadFloat4(m_Entries[idx].path.c_str(), store, e.error))
			return false;
		im = store.GetImage(0, 0, 0);
		if (im->width != self_im->width || im->height != self_im->height)
		{
			DirectX::ScratchImage resized;
			if (FAILED(DirectX::Resize(*im, self_im->width, self_im->height,
				DirectX::TEX_FILTER_BOX, resized)))
			{
				e.error = "companion resize failed";
				return false;
			}
			store = std::move(resized);
			im = store.GetImage(0, 0, 0);
		}
		return true;
	};
	if (e.role == RoleN && e.comp_rha >= 0 && !load_aligned(e.comp_rha, rha_img, rha_im))
		return false;
	if ((e.role == RoleRHA || e.role == RoleRMA) && e.comp_n >= 0 && !load_aligned(e.comp_n, n_img, n_im))
		return false;

	// channel remap in float linear space (DESIGN3 3.2)
	DirectX::ScratchImage remap_img;
	if (FAILED(remap_img.Initialize2D(DXGI_FORMAT_R32G32B32A32_FLOAT, self_im->width, self_im->height, 1, 1)))
	{
		e.error = "scratch alloc failed";
		return false;
	}
	const DirectX::Image* remap_im = remap_img.GetImage(0, 0, 0);
	for (size_t y = 0; y < self_im->height; ++y)
	{
		const float* sr = RowF(*self_im, y);
		const float* cr = rha_im ? RowF(*rha_im, y) : (n_im ? RowF(*n_im, y) : nullptr);
		float* dr = RowW(*remap_im, y);
		for (size_t x = 0; x < self_im->width; ++x)
		{
			const float* s = sr + x * 4;
			const float* c = cr ? cr + x * 4 : nullptr;
			float* o = dr + x * 4;
			switch (e.role)
			{
			case RoleD:
				o[0] = s[0]; o[1] = s[1]; o[2] = s[2]; o[3] = s[3];
				break;
			case RoleN:
				// R = height (RHA.g channel, 50% gray for _RMA groups),
				// G = normal.y, B = 0, A = normal.x (from the source's R)
				o[0] = c ? c[1] : 0.5f; o[1] = s[1]; o[2] = 0.f; o[3] = s[0];
				break;
			case RoleRHA:
				// R = 0, G = roughness (N.a), B = 0, A = ao (N.b)
				o[0] = 0.f; o[1] = c[3]; o[2] = 0.f; o[3] = c[2];
				break;
			case RoleRMA:
				// R = metallic (self.g), G = roughness (N.a), B = 0, A = ao (N.b)
				o[0] = s[1]; o[1] = c[3]; o[2] = 0.f; o[3] = c[2];
				break;
			default:
				o[0] = o[1] = o[2] = o[3] = 0.f;
				break;
			}
		}
	}

	// resize to target size (config layer guarantees no upscale, multiple of 4)
	DirectX::ScratchImage work_img;
	if (e.target_w > 0 && ((int)self_im->width != e.target_w || (int)self_im->height != e.target_h))
	{
		DirectX::ScratchImage resized;
		if (FAILED(DirectX::Resize(*remap_im, (size_t)e.target_w, (size_t)e.target_h,
			DirectX::TEX_FILTER_BOX, resized)))
		{
			e.error = "resize failed";
			return false;
		}
		work_img = std::move(resized);
	}
	else
		work_img = std::move(remap_img);

	DirectX::ScratchImage result;
	if (e.fmt == FmtRGBA8)
	{
		// uncompressed target: alpha cutout on the base level before mips,
		// then legacy A8R8G8B8 via B8G8R8A8_UNORM (DirectXTex legacy encode)
		if (e.cutout > 0)
		{
			float thr = e.cutout / 255.f;
			const DirectX::Image& base = *work_img.GetImage(0, 0, 0);
			for (size_t y = 0; y < base.height; ++y)
			{
				float* row = RowW(base, y);
				for (size_t x = 0; x < base.width; ++x)
					row[x * 4 + 3] = row[x * 4 + 3] < thr ? 0.f : 1.f;
			}
		}
		DirectX::ScratchImage mips;
		if (FAILED(DirectX::GenerateMipMaps(*work_img.GetImage(0, 0, 0), DirectX::TEX_FILTER_BOX, 0, mips)))
		{
			e.error = "mipmap generation failed";
			return false;
		}
		DirectX::ScratchImage out8;
		if (FAILED(DirectX::Convert(mips.GetImages(), mips.GetImageCount(), mips.GetMetadata(),
			DXGI_FORMAT_B8G8R8A8_UNORM, DirectX::TEX_FILTER_DEFAULT, 0.f, out8)))
		{
			e.error = "convert to BGRA8 failed";
			return false;
		}
		result = std::move(out8);
	}
	else
	{
		// BC targets: mip filter in float space, quantize to 8-bit, compress
		DirectX::ScratchImage mips;
		if (FAILED(DirectX::GenerateMipMaps(*work_img.GetImage(0, 0, 0), DirectX::TEX_FILTER_BOX, 0, mips)))
		{
			e.error = "mipmap generation failed";
			return false;
		}
		DirectX::ScratchImage out8;
		if (FAILED(DirectX::Convert(mips.GetImages(), mips.GetImageCount(), mips.GetMetadata(),
			DXGI_FORMAT_R8G8B8A8_UNORM, DirectX::TEX_FILTER_DEFAULT, 0.f, out8)))
		{
			e.error = "convert to RGBA8 failed";
			return false;
		}

		DXGI_FORMAT dxgi = DXGI_FORMAT_BC7_UNORM;
		switch (e.fmt)
		{
		case FmtBC1:
		case FmtBC1a: dxgi = DXGI_FORMAT_BC1_UNORM; break;
		case FmtBC2:  dxgi = DXGI_FORMAT_BC2_UNORM; break;
		case FmtBC3:  dxgi = DXGI_FORMAT_BC3_UNORM; break;
		case FmtBC5:  dxgi = DXGI_FORMAT_BC5_UNORM; break;
		case FmtBC7:  dxgi = DXGI_FORMAT_BC7_UNORM; break;
		}

		// GPU first (DirectCompute, much faster), multithreaded CPU fallback;
		// GPU BC7 is a fixed-mode search with slightly lower quality, the
		// "GPU Compression" checkbox switches back to the CPU path
		ID3D11Device* gpu = m_UseGPU ? AcquireGPUDevice() : nullptr;
		DirectX::ScratchImage comp;
		HRESULT hr = E_FAIL;
		if (gpu)
		{
			// the GPU compressor applies no external BC1 threshold: quantize
			// alpha ourselves per mip so 1-bit alpha matches the cutout setting
			if ((e.fmt == FmtBC1 || e.fmt == FmtBC1a) && e.cutout > 0)
				ThresholdAlpha8(out8, e.cutout / 255.f);
			hr = DirectX::Compress(gpu, out8.GetImages(), out8.GetImageCount(), out8.GetMetadata(),
				dxgi, DirectX::TEX_COMPRESS_DEFAULT, 1.0f, comp);
			if (FAILED(hr))
			{
				comp = DirectX::ScratchImage();
				ELog.Msg(mtWarning, "Image Converter: GPU compression failed (0x%08X), falling back to CPU.", (u32)hr);
			}
		}
		if (!gpu || FAILED(hr))
		{
			// threshold is only used by BC1; PARALLEL enables multithreading
			float thr = (e.fmt == FmtBC1 || e.fmt == FmtBC1a) && e.cutout > 0 ? e.cutout / 255.f : 0.5f;
			hr = DirectX::Compress(out8.GetImages(), out8.GetImageCount(), out8.GetMetadata(),
				dxgi, DirectX::TEX_COMPRESS_PARALLEL, thr, comp);
		}
		if (FAILED(hr))
		{
			e.error = "compression failed";
			return false;
		}
		result = std::move(comp);
	}

	// save: legacy fourcc headers come out of DirectXTex by default for
	// BC1/BC2/BC3 and B8G8R8A8; BC5 must be forced through the DX10 header;
	// BC7 always writes the DX10 header (matches the reference outputs)
	DirectX::DDS_FLAGS ddsflags = e.fmt == FmtBC5 ? DirectX::DDS_FLAGS_FORCE_DX10_EXT : DirectX::DDS_FLAGS_NONE;
	if (FAILED(DirectX::SaveToDDSFile(result.GetImages(), result.GetImageCount(), result.GetMetadata(),
		ddsflags, ToWide(out_path.c_str()).c_str())))
	{
		e.error = "save failed: " + out_path;
		return false;
	}
	return true;
}

void UIImageConverterForm::DoConvert()
{
	u32 converted = 0, failed = 0;
	for (ConvEntry& e : m_Entries)
	{
		if (!e.approved)
			continue;
		if (!CanApprove(e))
		{
			ELog.Msg(mtWarning, "Image Converter: '%s' approved but incomplete, skipped.", e.name.c_str());
			continue;
		}
		if (ConvertEntry(e))
		{
			e.state = StConverted;
			++converted;
			ELog.Msg(mtInformation, "Image Converter: '%s' -> '%s'", e.name.c_str(), OutputName(e).c_str());
		}
		else
		{
			e.state = StFailed;
			++failed;
			ELog.Msg(mtError, "Image Converter: FAILED '%s': %s", e.name.c_str(), e.error.c_str());
		}
	}
	m_LastConverted = converted;
	m_LastFailed = failed;
	ELog.Msg(mtInformation, "Image Converter: converted %u / failed %u.", converted, failed);
}

// ----------------------------------------------------------------------------
// UI
// ----------------------------------------------------------------------------

bool UIImageConverterForm::Selected(int i) const
{
	return std::find(m_Selection.begin(), m_Selection.end(), i) != m_Selection.end();
}

void UIImageConverterForm::SelectOnly(int i)
{
	m_Selection.clear();
	m_Selection.push_back(i);
	m_Anchor = i;
}

void UIImageConverterForm::ToggleSelect(int i)
{
	auto it = std::find(m_Selection.begin(), m_Selection.end(), i);
	if (it != m_Selection.end())
		m_Selection.erase(it);
	else
		m_Selection.insert(std::upper_bound(m_Selection.begin(), m_Selection.end(), i), i);
	m_Anchor = i;
}

void UIImageConverterForm::RangeSelect(int i)
{
	if (m_Anchor < 0 || m_Anchor >= (int)m_Entries.size())
	{
		SelectOnly(i);
		return;
	}
	int lo = m_Anchor < i ? m_Anchor : i;
	int hi = m_Anchor < i ? i : m_Anchor;
	m_Selection.clear();
	for (int k = lo; k <= hi; ++k)
		m_Selection.push_back(k);
	// anchor stays on the row that started the range
}

// mixed-selection style tint (UIThmEditorForm pattern)
namespace
{
	struct MixedStyle
	{
		MixedStyle(bool mixed)
		{
			if (mixed)
			{
				ImVec4 c = ImGui::GetStyleColorVec4(ImGuiCol_FrameBg);
				c.x = c.x * 0.3f + 0.70f;
				c.y = c.y * 0.3f + 0.45f;
				c.z = c.z * 0.3f + 0.10f;
				c.w = 0.85f;
				ImGui::PushStyleColor(ImGuiCol_FrameBg, c);
				ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, c);
				ImGui::PushStyleColor(ImGuiCol_FrameBgActive, c);
				m_pops = 3;
			}
		}
		~MixedStyle()
		{
			if (m_pops)
				ImGui::PopStyleColor(m_pops);
		}
		int m_pops = 0;
	};
}

void UIImageConverterForm::DrawFileList()
{
	if (m_Entries.empty())
		return;

	ImGui::BeginChild("ic_file_list", ImVec2(0, ImGui::GetFrameHeightWithSpacing() * 7.0f), ImGuiChildFlags_FrameStyle);
	for (int i = 0; i < (int)m_Entries.size(); ++i)
	{
		const ConvEntry& e = m_Entries[i];
		const char* mark = e.state == StConverted ? "[A]" : "[U]";
		char row[1024];
		sprintf_s(row, sizeof(row), "%s %s  %s  %s  %dx%d -> %s",
			mark, e.name.c_str(), RoleName(e.role), FmtName(e.fmt),
			e.target_w, e.target_h, OutputName(e).c_str());

		bool sel = Selected(i);
		if (ImGui::Selectable(row, sel))
		{
			ImGuiIO& io = ImGui::GetIO();
			if (io.KeyShift)
				RangeSelect(i);
			else if (io.KeyCtrl)
				ToggleSelect(i);
			else
				SelectOnly(i);
		}
		if (ImGui::IsItemHovered())
		{
			if (e.state == StFailed && !e.error.empty())
				ImGui::SetTooltip("%s\n%s", e.path.c_str(), e.error.c_str());
			else
				ImGui::SetTooltip("%s", e.path.c_str());
		}
	}
	ImGui::EndChild();
}

void UIImageConverterForm::ApplyTargetSize(int w, int h)
{
	if (w < 4 || h < 4 || w % 4 || h % 4)
	{
		m_SizeError = true; // raised as a modal at the end of Draw()
		return;
	}
	for (int i : m_Selection)
	{
		ConvEntry& e = m_Entries[i];
		if (w > e.src_w || h > e.src_h)
		{
			ELog.Msg(mtError, "Image Converter: '%s': target %dx%d exceeds source %dx%d, skipped.",
				e.name.c_str(), w, h, e.src_w, e.src_h);
			continue;
		}
		e.target_w = w;
		e.target_h = h;
	}
}

void UIImageConverterForm::DrawSettings()
{
	int prim = PrimaryIndex();
	ConvEntry& P = m_Entries[prim];

	// refresh edit mirrors from the primary selection
	m_EdFmt = (int)P.fmt;
	m_EdW = P.target_w;
	m_EdH = P.target_h;
	m_EdCutout = P.cutout;
	m_EdApproved = P.approved;

	ImGui::Separator();
	ImGui::TextDisabled("%s", Loc("Conversion Settings"));

	// format
	{
		bool mixed = FieldMixed([](const ConvEntry& e) { return (int)e.fmt; });
		MixedStyle style(mixed);
		if (ImGui::BeginCombo(Loc("Format"), mixed ? Loc("(mixed)") : FmtName((u32)m_EdFmt)))
		{
			for (int f = 0; f < FmtCount; ++f)
			{
				bool sel = (f == m_EdFmt);
				if (ImGui::Selectable(FmtName((u32)f), sel))
					ForSelected([f](ConvEntry& e) { e.fmt = (u32)f; });
				if (sel)
					ImGui::SetItemDefaultFocus();
			}
			ImGui::EndCombo();
		}
	}

	// target size: square presets or custom W/H, no upscale (v3-Q3)
	{
		static const int presets[5] = { 256, 512, 1024, 2048, 4096 };
		int cur = 5; // custom
		for (int i = 0; i < 5; ++i)
			if (m_EdW == m_EdH && m_EdW == presets[i])
				cur = i;
		bool mixed = FieldMixed([](const ConvEntry& e) { return e.target_w * 100000 + e.target_h; });
		char preview[32];
		if (mixed)
			xr_strcpy(preview, sizeof(preview), Loc("(mixed)"));
		else if (cur == 5)
			xr_strcpy(preview, sizeof(preview), Loc("Custom"));
		else
			sprintf_s(preview, sizeof(preview), "%d", presets[cur]);
		MixedStyle style(mixed);
		if (ImGui::BeginCombo(Loc("Target Size"), preview))
		{
			for (int i = 0; i < 5; ++i)
			{
				bool sel = (i == cur);
				char lbl[16];
				sprintf_s(lbl, sizeof(lbl), "%d", presets[i]);
				if (ImGui::Selectable(lbl, sel))
					ApplyTargetSize(presets[i], presets[i]);
				if (sel)
					ImGui::SetItemDefaultFocus();
			}
			if (ImGui::Selectable(Loc("Custom"), cur == 5))
			{
				// switching to custom keeps the current W/H
			}
			if (cur == 5)
				ImGui::SetItemDefaultFocus();
			ImGui::EndCombo();
		}
	}
	if (IsCustomSize(m_EdW, m_EdH))
	{
		bool mixed_w = FieldMixed([](const ConvEntry& e) { return e.target_w; });
		bool mixed_h = FieldMixed([](const ConvEntry& e) { return e.target_h; });
		{
			MixedStyle style(mixed_w);
			int w = m_EdW;
			if (ImGui::InputInt(Loc("Width"), &w, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue))
				ApplyTargetSize(w, m_EdH);
		}
		ImGui::SameLine();
		{
			MixedStyle style(mixed_h);
			int h = m_EdH;
			if (ImGui::InputInt(Loc("Height"), &h, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue))
				ApplyTargetSize(m_EdW, h);
		}
		ImGui::SameLine();
		ImGui::TextDisabled("(Enter)");
	}

	// GPU compression toggle (DirectCompute BC1-BC7; CPU fallback is
	// multithreaded, just slower and higher BC7 quality)
	ImGui::Checkbox(Loc("GPU Compression"), &m_UseGPU);

	// alpha cutout: only albedo entries carry transparency semantics
	{
		bool has_d = false;
		for (int i : m_Selection)
			if (m_Entries[i].role == RoleD) { has_d = true; break; }
		ImGui::BeginDisabled(!has_d);
		bool mixed = FieldMixed([](const ConvEntry& e) { return e.cutout; });
		MixedStyle style(mixed);
		if (ImGui::SliderInt(Loc("Alpha Cutout"), &m_EdCutout, 0, 255, "%d"))
			ForSelected([this](ConvEntry& e) { if (e.role == RoleD) e.cutout = m_EdCutout; });
		ImGui::EndDisabled();
	}

	// approval: gated by role knowledge and companion presence
	{
		bool all_can = true;
		for (int i : m_Selection)
			if (!CanApprove(m_Entries[i])) { all_can = false; break; }
		ImGui::BeginDisabled(!all_can);
		bool mixed = FieldMixed([](const ConvEntry& e) { return e.approved; });
		MixedStyle style(mixed);
		bool v = m_EdApproved;
		if (ImGui::Checkbox(Loc("Approve Configuration"), &v))
			ForSelected([v](ConvEntry& e) { e.approved = v; });
		ImGui::EndDisabled();
	}
}

void UIImageConverterForm::DrawProvenance()
{
	int prim = PrimaryIndex();
	const ConvEntry& P = m_Entries[prim];

	if (ImGui::CollapsingHeader(Loc("Channel Provenance"), ImGuiTreeNodeFlags_DefaultOpen))
	{
		static const char chans[5] = "RGBA";
		for (int ch = 0; ch < 4; ++ch)
		{
			char line[512];
			sprintf_s(line, sizeof(line), "%c: %s", chans[ch], P.prov[ch].text.c_str());
			if (P.prov[ch].found)
				ImGui::TextUnformatted(line);
			else
				ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f), "%s", line);
		}
	}
}

void UIImageConverterForm::DrawConfirmModal()
{
	// "Width/Height must be a multiple of 4!" popup (v3-Q1)
	if (ImGui::BeginPopupModal("Size Error", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		ImGui::Text("%s", Loc("Width/Height must be a multiple of 4!"));
		if (ImGui::Button(Loc("OK")))
			ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}

	// Make Texture confirmation (v2-Q6: synchronous conversion on Proceed)
	if (ImGui::BeginPopupModal("Make Texture", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		int approved = 0, unconf = 0;
		for (const ConvEntry& e : m_Entries)
		{
			if (e.approved && CanApprove(e))
				++approved;
			else
				++unconf;
		}
		ImGui::Text("Total: %d Textures approved, left %d Textures unconfigured, proceed?", approved, unconf);
		if (ImGui::Button(Loc("Proceed")))
		{
			DoConvert();
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button(Loc("Cancel")))
			ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}
}

void UIImageConverterForm::Draw()
{
	if (ImGui::Button(Loc("Import Stalker2")))
		DoImport();
	ImGui::SameLine();
	if (ImGui::Button(Loc("Make Texture")))
		m_ConfirmOpen = true;
	ImGui::SameLine();
	if (ImGui::Button(Loc("Export to...")))
	{
		xr_string dir;
		if (PickFolder(dir))
			m_ExportDir = dir;
	}
	ImGui::SameLine();
	if (ImGui::Button(Loc("Remove Selected")))
	{
		for (int i = (int)m_Entries.size() - 1; i >= 0; --i)
			if (Selected(i))
				m_Entries.erase(m_Entries.begin() + i);
		m_Selection.clear();
		m_Anchor = -1;
		RebuildGroups();
	}
	ImGui::SameLine();
	if (ImGui::Button(Loc("Clear")))
	{
		m_Entries.clear();
		m_Selection.clear();
		m_Anchor = -1;
	}

	ImGui::TextDisabled("%s: %s", Loc("Output"),
		m_ExportDir.empty() ? Loc("<source folders>") : m_ExportDir.c_str());

	u32 approved_cnt = 0;
	for (const ConvEntry& e : m_Entries)
		if (e.approved && CanApprove(e))
			++approved_cnt;
	ImGui::Text("Approved: %u / %u    %s: %u, %s: %u",
		approved_cnt, (u32)m_Entries.size(),
		Loc("converted"), m_LastConverted, Loc("failed"), m_LastFailed);

	DrawFileList();

	if (HasSelection())
	{
		ImGui::Separator();
		DrawSettings();
		DrawProvenance();
	}

	if (m_ConfirmOpen)
	{
		ImGui::OpenPopup("Make Texture");
		m_ConfirmOpen = false;
	}
	if (m_SizeError)
	{
		ImGui::OpenPopup("Size Error");
		m_SizeError = false;
	}
	DrawConfirmModal();
}
