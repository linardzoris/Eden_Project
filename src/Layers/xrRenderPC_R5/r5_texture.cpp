#include "stdafx.h"
#include "r5_texture.h"
#include "r5_resources.h"

#include <DirectXTex.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace dx12
{
	extern "C" ENGINE_API ID3D12Device* GetDevice();
	extern "C" ENGINE_API ID3D12GraphicsCommandList* GetCmdList();
}

// ---------------------------------------------------------------------------
// 状态
// ---------------------------------------------------------------------------

// 通用纹理缓存条目（M6）
struct TexEntry
{
	Microsoft::WRL::ComPtr<ID3D12Resource> res;
	Microsoft::WRL::ComPtr<ID3D12Resource> uploadBuf;
	xr_vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts;
	xr_vector<UINT> numRows;
	xr_vector<UINT64> rowSizes;
	UINT numSub = 0;
	bool uploaded = false;
	D3D12_GPU_DESCRIPTOR_HANDLE gpuSRV = {};
};

static xr_vector<TexEntry> g_entries;
static xr_vector<shared_str> g_names;

static ComPtr<ID3D12Resource> g_testTex;
static ComPtr<ID3D12Resource> g_uploadBuf;
static D3D12_CPU_DESCRIPTOR_HANDLE g_testSRV_CPU = {};
static D3D12_GPU_DESCRIPTOR_HANDLE g_testSRV = {};
static bool g_ready = false;

// 延迟上传：Init 阶段仅创建资源 + 填充 UPLOAD 缓冲，
// CopyTextureRegion 命令必须在首帧（命令列表打开）录制
static UINT g_numSub = 0;
static xr_vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> g_layouts;
static xr_vector<UINT> g_numRows;
static xr_vector<UINT64> g_rowSizes;
static bool g_uploadDone = false;

// 通用 DDS 加载核心：读取文件 -> 建纹理 + UPLOAD 缓冲 -> 拷贝像素到 UPLOAD -> 创建 SRV
// 返回是否成功；SRV 描述符分配在 g_gpuHeap（caller 需保证 Init 期调用）
static bool LoadDDSCommon(const wchar_t* wpath, ID3D12Resource** outRes, ID3D12Resource** outUpload,
	DirectX::TexMetadata& outMeta, xr_vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT>& layouts,
	xr_vector<UINT>& numRows, xr_vector<UINT64>& rowSizes)
{
	ID3D12Device* dev = dx12::GetDevice();

	DirectX::ScratchImage image;
	HRESULT hr = DirectX::LoadFromDDSFile(wpath, DirectX::DDS_FLAGS_NONE, &outMeta, image);
	if (FAILED(hr))
		return false;

	D3D12_HEAP_PROPERTIES heapProps = {};
	heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

	D3D12_RESOURCE_DESC texDesc = {};
	texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	texDesc.Width = outMeta.width;
	texDesc.Height = (UINT)outMeta.height;
	texDesc.DepthOrArraySize = (UINT16)outMeta.arraySize;
	texDesc.MipLevels = (UINT16)outMeta.mipLevels;
	texDesc.Format = outMeta.format;
	texDesc.SampleDesc.Count = 1;
	texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

	ComPtr<ID3D12Resource> res;
	hr = dev->CreateCommittedResource(
		&heapProps, D3D12_HEAP_FLAG_NONE, &texDesc,
		D3D12_RESOURCE_STATE_COPY_DEST,
		nullptr,
		IID_PPV_ARGS(&res));
	if (FAILED(hr))
		return false;

	UINT numSub = (UINT)outMeta.mipLevels * (UINT)outMeta.arraySize;
	layouts.resize(numSub);
	numRows.resize(numSub);
	rowSizes.resize(numSub);
	UINT64 totalBytes = 0;
	dev->GetCopyableFootprints(&texDesc, 0, numSub, 0, layouts.data(), numRows.data(), rowSizes.data(), &totalBytes);

	D3D12_HEAP_PROPERTIES uploadHeap = {};
	uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

	D3D12_RESOURCE_DESC uploadDesc = {};
	uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	uploadDesc.Width = totalBytes;
	uploadDesc.Height = 1;
	uploadDesc.DepthOrArraySize = 1;
	uploadDesc.MipLevels = 1;
	uploadDesc.SampleDesc.Count = 1;
	uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

	ComPtr<ID3D12Resource> uploadBuf;
	hr = dev->CreateCommittedResource(
		&uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(&uploadBuf));
	if (FAILED(hr))
		return false;

	void* mapped = nullptr;
	hr = uploadBuf->Map(0, nullptr, &mapped);
	if (FAILED(hr))
		return false;
	u8* uploadCpu = (u8*)mapped;

	for (UINT i = 0; i < numSub; ++i)
	{
		const DirectX::Image* img = image.GetImage(i % outMeta.mipLevels, i / outMeta.mipLevels, 0);
		if (!img) continue;

		D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp = layouts[i];
		u8* dst = uploadCpu + fp.Offset;
		const u8* src = (const u8*)img->pixels;

		for (UINT row = 0; row < numRows[i]; ++row)
		{
			memcpy(dst + row * fp.Footprint.RowPitch, src + row * img->rowPitch, rowSizes[i]);
		}
	}
	uploadBuf->Unmap(0, nullptr);

	*outRes = res.Detach();
	*outUpload = uploadBuf.Detach();
	return true;
}

// 通用纹理加载核心（缓存去重 + DDS 加载 + SRV 分配）
static D3D12_GPU_DESCRIPTOR_HANDLE LoadImpl(const char* cacheKey, const char* fullPath)
{
	for (size_t i = 0; i < g_names.size(); ++i)
		if (g_names[i].equal(cacheKey))
			return g_entries[i].gpuSRV;

	wchar_t wpath[MAX_PATH];
	MultiByteToWideChar(CP_ACP, 0, fullPath, -1, wpath, MAX_PATH);

	DirectX::TexMetadata meta;
	xr_vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts;
	xr_vector<UINT> numRows;
	xr_vector<UINT64> rowSizes;
	ID3D12Resource* res = nullptr;
	ID3D12Resource* uploadBuf = nullptr;
	if (!LoadDDSCommon(wpath, &res, &uploadBuf, meta, layouts, numRows, rowSizes))
	{
		Msg("! R5 texture: Load(%s) failed", fullPath);
		return {};
	}

	TexEntry e;
	e.res.Attach(res);
	e.uploadBuf.Attach(uploadBuf);
	e.layouts = std::move(layouts);
	e.numRows = std::move(numRows);
	e.rowSizes = std::move(rowSizes);
	e.numSub = (UINT)meta.mipLevels * (UINT)meta.arraySize;

	// SRV 描述符分配在 g_gpuHeap（Init 期调用，槽位在 G-buffer/测试纹理之后）
	// 必须先 AllocCPU 再 AllocGPU（AllocGPU 依赖 offset 已被 AllocCPU 推进）
	D3D12_CPU_DESCRIPTOR_HANDLE srvCpu = r5_res::g_gpuHeap.AllocCPU(1);
	e.gpuSRV = r5_res::g_gpuHeap.AllocGPU(1);

	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	srvDesc.Format = meta.format;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.Texture2D.MipLevels = (UINT)meta.mipLevels;
	dx12::GetDevice()->CreateShaderResourceView(res, &srvDesc, srvCpu);

	g_names.push_back(cacheKey);
	g_entries.push_back(e);
	Msg("* R5 texture: %s loaded (%ux%u, %u mips, fmt %u), upload deferred", cacheKey,
		(UINT)meta.width, (UINT)meta.height, (UINT)meta.mipLevels, (UINT)meta.format);
	return e.gpuSRV;
}

// 通用纹理加载（缓存去重）
D3D12_GPU_DESCRIPTOR_HANDLE r5_texture::Load(const char* relPath)
{
	if (!relPath || !relPath[0])
		return {};

	char full[MAX_PATH];
	xr_strconcat(full, "gamedata\\textures\\", relPath, ".dds");
	return LoadImpl(relPath, full);
}

// M8: 按相对 gamedata\ 的完整路径加载（levels\... 等非标准目录）
D3D12_GPU_DESCRIPTOR_HANDLE r5_texture::LoadRoot(const char* relRootPath)
{
	if (!relRootPath || !relRootPath[0])
		return {};

	char full[MAX_PATH];
	xr_strconcat(full, "gamedata\\", relRootPath, ".dds");
	return LoadImpl(relRootPath, full);
}

// ---------------------------------------------------------------------------
// Init：加载 $shadertest.dds -> DX12 纹理 + SRV
// ---------------------------------------------------------------------------

bool r5_texture::Init()
{
	ID3D12Device* dev = dx12::GetDevice();
	ID3D12GraphicsCommandList* cmd = dx12::GetCmdList();
	if (!dev || !cmd)
		return false;

	// 加载 DDS（相对游戏根目录 cwd）
	wchar_t wpath[MAX_PATH];
	MultiByteToWideChar(CP_ACP, 0, "gamedata\\textures\\$shadertest.dds", -1, wpath, MAX_PATH);

	DirectX::TexMetadata meta;
	DirectX::ScratchImage image;
	HRESULT hr = DirectX::LoadFromDDSFile(wpath, DirectX::DDS_FLAGS_NONE, &meta, image);
	if (FAILED(hr))
	{
		Msg("! R5 texture: LoadFromDDSFile failed 0x%08x", hr);
		return false;
	}

	Msg("* R5 texture: %ux%u, %u mips, format %u",
		(UINT)meta.width, (UINT)meta.height, (UINT)meta.mipLevels, (UINT)meta.format);

	// 创建 DX12 纹理（DEFAULT 堆，COPY_DEST 初始）
	D3D12_HEAP_PROPERTIES heapProps = {};
	heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

	D3D12_RESOURCE_DESC texDesc = {};
	texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	texDesc.Width = meta.width;
	texDesc.Height = (UINT)meta.height;
	texDesc.DepthOrArraySize = (UINT16)meta.arraySize;
	texDesc.MipLevels = (UINT16)meta.mipLevels;
	texDesc.Format = meta.format;
	texDesc.SampleDesc.Count = 1;
	texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

	hr = dev->CreateCommittedResource(
		&heapProps, D3D12_HEAP_FLAG_NONE, &texDesc,
		D3D12_RESOURCE_STATE_COPY_DEST,
		nullptr,
		IID_PPV_ARGS(&g_testTex)
	);
	if (FAILED(hr))
	{
		Msg("! R5 texture: CreateCommittedResource failed 0x%08x", hr);
		return false;
	}

	// 计算上传布局
	UINT numSub = (UINT)meta.mipLevels * (UINT)meta.arraySize;
	xr_vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts(numSub);
	xr_vector<UINT> numRows(numSub);
	xr_vector<UINT64> rowSizes(numSub);
	UINT64 totalBytes = 0;
	dev->GetCopyableFootprints(&texDesc, 0, numSub, 0, layouts.data(), numRows.data(), rowSizes.data(), &totalBytes);

	// 保存布局供首帧 UploadFirstFrame 使用
	g_numSub = numSub;
	g_layouts = std::move(layouts);
	g_numRows = std::move(numRows);
	g_rowSizes = std::move(rowSizes);

	// M4c-fix: 用独立 UPLOAD 缓冲（不用 upload ring，避免 Init 分配的内存被首帧 BeginFrame Reset 清空）
	D3D12_HEAP_PROPERTIES uploadHeap = {};
	uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

	D3D12_RESOURCE_DESC uploadDesc = {};
	uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	uploadDesc.Width = totalBytes;
	uploadDesc.Height = 1;
	uploadDesc.DepthOrArraySize = 1;
	uploadDesc.MipLevels = 1;
	uploadDesc.SampleDesc.Count = 1;
	uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

	ComPtr<ID3D12Resource> uploadBuf;
	hr = dev->CreateCommittedResource(
		&uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(&uploadBuf)
	);
	if (FAILED(hr))
	{
		Msg("! R5 texture: upload buffer create failed 0x%08x", hr);
		return false;
	}

	// Map + 拷贝像素数据
	void* mapped = nullptr;
	hr = uploadBuf->Map(0, nullptr, &mapped);
	if (FAILED(hr))
	{
		Msg("! R5 texture: upload buffer map failed 0x%08x", hr);
		return false;
	}
	u8* uploadCpu = (u8*)mapped;

	for (UINT i = 0; i < numSub; ++i)
	{
		const DirectX::Image* img = image.GetImage(i % meta.mipLevels, i / meta.mipLevels, 0);
		if (!img) continue;

		D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp = g_layouts[i];
		u8* dst = uploadCpu + fp.Offset;
		const u8* src = (const u8*)img->pixels;

		// BC 压缩纹理：行数实际是块行数（每 4 像素一行），且 rowPitch 是块行字节数
		// DirectXTex 的 Image 已解压为 RGBA32？不，LoadFromDDSFile 保持原始 BC 格式，需要按块拷贝
		// 但 GetCopyableFootprints 返回的 numRows 已考虑压缩（行数 = height/4），直接按行拷贝即可
		// 关键：源行距 = img->rowPitch（块行字节数），目标行距 = fp.Footprint.RowPitch（对齐后）
		for (UINT row = 0; row < g_numRows[i]; ++row)
		{
			memcpy(dst + row * fp.Footprint.RowPitch, src + row * img->rowPitch, g_rowSizes[i]);
		}
	}
	uploadBuf->Unmap(0, nullptr);

	// 上传缓冲随纹理生命周期（Shutdown 时一起释放，避免静态列表在设备销毁后悬垂）
	g_uploadBuf = uploadBuf;

	// 注意：CopyTextureRegion + 屏障不在此录制！
	// Init 阶段命令列表处于关闭状态（CreateCommandObjects 中 Close 后未 Reset），
	// 录制的 GPU 命令会被丢弃，且首帧 BeginFrame 的 Reset 也会清空。
	// 延迟到首帧 UploadFirstFrame() 在打开的命令列表上录制。

	// M4c: 纹理 SRV 放 GPU 堆槽位 0（最优先分配，后续 G-buffer 从槽位 1 开始）
	// 注意：此分配必须在 CreateGBufferAndComposePSO 之前（见 r5_pipeline.cpp Init 调用顺序）
	// 必须先 AllocCPU 再 AllocGPU（AllocGPU 依赖 offset 已被 AllocCPU 推进）
	g_testSRV_CPU = r5_res::g_gpuHeap.AllocCPU(1);
	g_testSRV = r5_res::g_gpuHeap.AllocGPU(1);

	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	srvDesc.Format = meta.format;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.Texture2D.MipLevels = (UINT)meta.mipLevels;
	dev->CreateShaderResourceView(g_testTex.Get(), &srvDesc, g_testSRV_CPU);

	g_ready = true;
	g_uploadDone = false;
	Msg("* R5 texture: $shadertest.dds loaded (%ux%u, fmt %u), upload deferred to first frame",
		(UINT)meta.width, (UINT)meta.height, (UINT)meta.format);
	return true;
}

// ---------------------------------------------------------------------------
// 首帧上传：在打开的命令列表上录制 CopyTextureRegion + 状态屏障
// ---------------------------------------------------------------------------

void r5_texture::UploadFirstFrame(ID3D12GraphicsCommandList* cmd)
{
	if (!cmd)
		return;

	// 通用纹理缓存：未上传的条目录制 Copy + 屏障
	for (size_t i = 0; i < g_entries.size(); ++i)
	{
		TexEntry& e = g_entries[i];
		if (e.uploaded || !e.res || !e.uploadBuf || e.numSub == 0)
			continue;

		for (UINT k = 0; k < e.numSub; ++k)
		{
			D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
			dstLoc.pResource = e.res.Get();
			dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			dstLoc.SubresourceIndex = k;

			D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
			srcLoc.pResource = e.uploadBuf.Get();
			srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
			srcLoc.PlacedFootprint = e.layouts[k];

			cmd->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);
		}

		D3D12_RESOURCE_BARRIER barrier = {};
		barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barrier.Transition.pResource = e.res.Get();
		barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
		barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		cmd->ResourceBarrier(1, &barrier);

		e.uploaded = true;
	}

	// 测试纹理（既有路径）
	if (g_uploadDone || !g_testTex || !g_uploadBuf || g_numSub == 0)
		return;

	for (UINT i = 0; i < g_numSub; ++i)
	{
		D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
		dstLoc.pResource = g_testTex.Get();
		dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		dstLoc.SubresourceIndex = i;

		D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
		srcLoc.pResource = g_uploadBuf.Get();
		srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		srcLoc.PlacedFootprint = g_layouts[i];

		cmd->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);
	}

	D3D12_RESOURCE_BARRIER barrier = {};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = g_testTex.Get();
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	cmd->ResourceBarrier(1, &barrier);

	g_uploadDone = true;
	Msg("* R5 texture: CopyTextureRegion recorded (first frame)");
}

// ---------------------------------------------------------------------------
// Shutdown
// ---------------------------------------------------------------------------

void r5_texture::Shutdown()
{
	g_entries.clear();
	g_names.clear();
	g_uploadBuf.Reset();
	g_testTex.Reset();
	g_testSRV = {};
	g_testSRV_CPU = {};
	g_ready = false;
	g_numSub = 0;
	g_layouts.clear();
	g_numRows.clear();
	g_rowSizes.clear();
	g_uploadDone = false;
}

// ---------------------------------------------------------------------------
// GetTestSRV
// ---------------------------------------------------------------------------

D3D12_GPU_DESCRIPTOR_HANDLE r5_texture::GetTestSRV()
{
	return g_testSRV;
}

D3D12_CPU_DESCRIPTOR_HANDLE r5_texture::GetTestSRV_CPU()
{
	return g_testSRV_CPU;
}
