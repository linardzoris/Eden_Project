#include "stdafx.h"
#include "r5_pipeline.h"
#include "r5_resources.h"

#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

// 引擎侧 DX12 上下文访问
namespace dx12
{
	extern "C" ENGINE_API void BeginFrame();
	extern "C" ENGINE_API void EndFrame();
	extern "C" ENGINE_API ID3D12Device* GetDevice();
	extern "C" ENGINE_API ID3D12GraphicsCommandList* GetCmdList();
	extern "C" ENGINE_API D3D12_CPU_DESCRIPTOR_HANDLE GetCurrentRTV();
	extern "C" ENGINE_API D3D12_CPU_DESCRIPTOR_HANDLE GetCurrentDSV();
	extern "C" ENGINE_API UINT GetFrameIndex();
}

// ---------------------------------------------------------------------------
// 全屏三角形 PSO
// ---------------------------------------------------------------------------

static ComPtr<ID3D12RootSignature> g_rootSig;
static ComPtr<ID3D12PipelineState> g_psoFullscreen;

// M3c: 立方体
static ComPtr<ID3D12RootSignature> g_cubeRootSig;
static ComPtr<ID3D12PipelineState> g_cubePSO;
static D3D12_GPU_VIRTUAL_ADDRESS g_cubeCBV = 0;
static void* g_cubeCBPtr = nullptr;

struct CubeVertex { float pos[3]; float nrm[3]; };
static D3D12_GPU_VIRTUAL_ADDRESS g_cubeVB = 0;
static D3D12_GPU_VIRTUAL_ADDRESS g_cubeIB = 0;

// M3b: 内部 RT（立方体渲染目标 + 拷贝源）
static ComPtr<ID3D12Resource> g_sceneRT;
static ComPtr<ID3D12DescriptorHeap> g_rtvHeap;	// CPU-only RTV 堆
static D3D12_CPU_DESCRIPTOR_HANDLE g_sceneRTV = {};
static UINT g_rtvDescriptorSize = 0;

// M3b: 拷贝 PSO（SRV 描述符表 + 静态采样器）
static ComPtr<ID3D12RootSignature> g_copyRootSig;
static ComPtr<ID3D12PipelineState> g_copyPSO;
static D3D12_CPU_DESCRIPTOR_HANDLE g_copySRV_CPU = {};
static D3D12_GPU_DESCRIPTOR_HANDLE g_copySRV_GPU = {};

// 内嵌 HLSL 源码（M2 验证用，后续迁移到 gamedata）
static const char* g_hlslFullscreen = R"(
struct VSOut
{
	float4 pos : SV_POSITION;
	float2 uv  : TEXCOORD0;
};

VSOut VSMain(uint vid : SV_VertexID)
{
	VSOut o;
	// 全屏三角形：(-1,-1) (3,-1) (-1,3)
	o.pos = float4(
		(vid == 1) ? 3.0 : -1.0,
		(vid == 2) ? 3.0 : -1.0,
		0.0, 1.0
	);
	o.uv = float2(o.pos.x * 0.5 + 0.5, 0.5 - o.pos.y * 0.5);
	return o;
}

float4 PSMain(VSOut i) : SV_TARGET
{
	// 蓝紫渐变验证 UV 正确性
	return float4(i.uv.x, i.uv.y * 0.3, 0.5 + i.uv.x * 0.3, 1.0);
}
)";

// M3c: 立方体着色器
static const char* g_hlslCube = R"(
cbuffer CB : register(b0)
{
	row_major float4x4 mvp;
	float4 color;
};

struct VSIn
{
	float3 pos : POSITION;
	float3 nrm : NORMAL;
};

struct VSOut
{
	float4 pos : SV_POSITION;
	float3 nrm : NORMAL;
	float2 uv  : TEXCOORD0;
};

VSOut VSMain(VSIn i)
{
	VSOut o;
	o.pos = mul(float4(i.pos, 1.0), mvp);
	o.nrm = i.nrm;
	o.uv = i.pos.xy * 0.5 + 0.5;
	return o;
}

float4 PSMain(VSOut i) : SV_TARGET
{
	// 简单法线可视化 + 时间变色
	float3 n = normalize(i.nrm) * 0.5 + 0.5;
	return float4(n * color.rgb, 1.0);
}
)";

// ---------------------------------------------------------------------------
// M3b: 从 gamedata 加载着色器
// ---------------------------------------------------------------------------

static ComPtr<ID3DBlob> LoadShaderFromFile(const char* relativePath, const char* entry, const char* target)
{
	string_path fullPath;
	xr_strconcat(fullPath, "gamedata\\shaders\\d3d12\\", relativePath);

	// 相对 exe 工作目录（Eden Launcher 启动时 cwd = 游戏根目录）
	wchar_t wPath[MAX_PATH];
	MultiByteToWideChar(CP_ACP, 0, fullPath, -1, wPath, MAX_PATH);

	ComPtr<ID3DBlob> code;
	ComPtr<ID3DBlob> err;
	HRESULT hr = D3DCompileFromFile(
		wPath,
		nullptr,
		D3D_COMPILE_STANDARD_FILE_INCLUDE,
		entry, target,
		D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_WARNINGS_ARE_ERRORS,
		0,
		&code, &err
	);
	if (FAILED(hr))
	{
		Msg("! R5 shader load %s failed 0x%08x", fullPath, hr);
		if (err)
			Msg("! %s", (const char*)err->GetBufferPointer());
		return nullptr;
	}
	return code;
}

// ---------------------------------------------------------------------------
// M3b: 创建内部 RT + 拷贝 PSO
// ---------------------------------------------------------------------------

static bool CreateSceneRTAndCopyPSO(ID3D12Device* dev, UINT width, UINT height)
{
	// RTV 堆（CPU-only，仅内部使用）
	D3D12_DESCRIPTOR_HEAP_DESC rtvHd = {};
	rtvHd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	rtvHd.NumDescriptors = 1;
	HRESULT hr = dev->CreateDescriptorHeap(&rtvHd, IID_PPV_ARGS(&g_rtvHeap));
	if (FAILED(hr))
		return false;

	g_rtvDescriptorSize = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	g_sceneRTV = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();

	// 内部 RT：R8G8B8A8，可渲染可采样
	D3D12_CLEAR_VALUE clearRT = {};
	clearRT.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	clearRT.Color[0] = 0.0f;
	clearRT.Color[1] = 0.0f;
	clearRT.Color[2] = 0.3f;
	clearRT.Color[3] = 1.0f;

	D3D12_HEAP_PROPERTIES heapProps = {};
	heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

	D3D12_RESOURCE_DESC rtDesc = {};
	rtDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	rtDesc.Width = width;
	rtDesc.Height = height;
	rtDesc.DepthOrArraySize = 1;
	rtDesc.MipLevels = 1;
	rtDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	rtDesc.SampleDesc.Count = 1;
	rtDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

	hr = dev->CreateCommittedResource(
		&heapProps, D3D12_HEAP_FLAG_NONE, &rtDesc,
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
		&clearRT,
		IID_PPV_ARGS(&g_sceneRT)
	);
	if (FAILED(hr))
		return false;

	dev->CreateRenderTargetView(g_sceneRT.Get(), nullptr, g_sceneRTV);

	// SRV 描述符（在 GPU 堆中分配）
	g_copySRV_CPU = r5_res::g_gpuHeap.AllocCPU(1);
	g_copySRV_GPU = r5_res::g_gpuHeap.AllocGPU(1);

	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.Texture2D.MipLevels = 1;
	dev->CreateShaderResourceView(g_sceneRT.Get(), &srvDesc, g_copySRV_CPU);

	// 根签名：SRV 描述符表 + 静态采样器
	D3D12_DESCRIPTOR_RANGE srvRange = {};
	srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	srvRange.NumDescriptors = 1;
	srvRange.BaseShaderRegister = 0;
	srvRange.RegisterSpace = 0;
	srvRange.OffsetInDescriptorsFromTableStart = 0;

	D3D12_ROOT_PARAMETER copyParam = {};
	copyParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	copyParam.DescriptorTable.NumDescriptorRanges = 1;
	copyParam.DescriptorTable.pDescriptorRanges = &srvRange;
	copyParam.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	D3D12_STATIC_SAMPLER_DESC sampler = {};
	sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
	sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.ShaderRegister = 0;
	sampler.RegisterSpace = 0;
	sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	D3D12_ROOT_SIGNATURE_DESC copyRsDesc = {};
	copyRsDesc.NumParameters = 1;
	copyRsDesc.pParameters = &copyParam;
	copyRsDesc.NumStaticSamplers = 1;
	copyRsDesc.pStaticSamplers = &sampler;
	copyRsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

	ComPtr<ID3DBlob> sigBlob;
	ComPtr<ID3DBlob> sigErr;
	hr = D3D12SerializeRootSignature(&copyRsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &sigErr);
	if (FAILED(hr))
	{
		if (sigErr)
			Msg("! R5 copy root sig error: %s", (const char*)sigErr->GetBufferPointer());
		return false;
	}

	hr = dev->CreateRootSignature(0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(), IID_PPV_ARGS(&g_copyRootSig));
	if (FAILED(hr))
		return false;

	// 加载 gamedata shader
	ComPtr<ID3DBlob> vs = LoadShaderFromFile("copy_simple.ps.hlsl", "VSMain", "vs_5_1");
	ComPtr<ID3DBlob> ps = LoadShaderFromFile("copy_simple.ps.hlsl", "PSMain", "ps_5_1");
	if (!vs || !ps)
		return false;

	// PSO
	D3D12_GRAPHICS_PIPELINE_STATE_DESC copyPsoDesc = {};
	copyPsoDesc.pRootSignature = g_copyRootSig.Get();
	copyPsoDesc.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
	copyPsoDesc.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
	copyPsoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	copyPsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	copyPsoDesc.RasterizerState.DepthClipEnable = FALSE;
	copyPsoDesc.BlendState.RenderTarget[0].BlendEnable = FALSE;
	copyPsoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
	copyPsoDesc.DepthStencilState.DepthEnable = FALSE;
	copyPsoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
	copyPsoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
	copyPsoDesc.DSVFormat = DXGI_FORMAT_UNKNOWN;
	copyPsoDesc.SampleMask = UINT_MAX;
	copyPsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	copyPsoDesc.NumRenderTargets = 1;
	copyPsoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
	copyPsoDesc.SampleDesc.Count = 1;

	hr = dev->CreateGraphicsPipelineState(&copyPsoDesc, IID_PPV_ARGS(&g_copyPSO));
	if (FAILED(hr))
	{
		Msg("! R5: copy PSO failed 0x%08x", hr);
		return false;
	}

	Msg("* R5: scene RT + copy PSO created (%ux%u)", width, height);
	return true;
}

static ComPtr<ID3DBlob> CompileShaderBlob(const char* src, const char* entry, const char* target)
{
	ComPtr<ID3DBlob> code;
	ComPtr<ID3DBlob> err;

	UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
	flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

	HRESULT hr = D3DCompile2(
		src, strlen(src),
		nullptr, nullptr, nullptr,
		entry, target,
		flags, 0, 0, nullptr, 0,
		&code, &err
	);

	if (FAILED(hr))
	{
		if (err)
			Msg("! R5 shader compile error: %s", (const char*)err->GetBufferPointer());
		return nullptr;
	}

	return code;
}

// ---------------------------------------------------------------------------
// Init / Shutdown
// ---------------------------------------------------------------------------

bool r5_pipeline::Init()
{
	ID3D12Device* dev = dx12::GetDevice();
	if (!dev)
	{
		Msg("! R5: Init failed, no device");
		return false;
	}

	// 空根签名（M2 无资源绑定）
	D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
	rsDesc.NumParameters = 0;
	rsDesc.pParameters = nullptr;
	rsDesc.NumStaticSamplers = 0;
	rsDesc.pStaticSamplers = nullptr;
	rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

	ComPtr<ID3DBlob> sigBlob;
	ComPtr<ID3DBlob> sigErr;
	HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &sigErr);
	if (FAILED(hr))
	{
		if (sigErr)
			Msg("! R5 root sig serialize error: %s", (const char*)sigErr->GetBufferPointer());
		return false;
	}

	hr = dev->CreateRootSignature(0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(), IID_PPV_ARGS(&g_rootSig));
	if (FAILED(hr))
	{
		Msg("! R5: CreateRootSignature failed 0x%08x", hr);
		return false;
	}

	// 编译着色器
	ComPtr<ID3DBlob> vs = CompileShaderBlob(g_hlslFullscreen, "VSMain", "vs_5_1");
	ComPtr<ID3DBlob> ps = CompileShaderBlob(g_hlslFullscreen, "PSMain", "ps_5_1");
	if (!vs || !ps)
		return false;

	// PSO
	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
	psoDesc.pRootSignature = g_rootSig.Get();
	psoDesc.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
	psoDesc.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };

	// 固定管线状态
	psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	psoDesc.RasterizerState.FrontCounterClockwise = FALSE;
	psoDesc.RasterizerState.DepthClipEnable = FALSE;

	psoDesc.BlendState.RenderTarget[0].BlendEnable = FALSE;
	psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

	// 禁用深度（M2 简化）
	psoDesc.DepthStencilState.DepthEnable = FALSE;
	psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
	psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
	psoDesc.DSVFormat = DXGI_FORMAT_UNKNOWN;

	psoDesc.SampleMask = UINT_MAX;
	psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	psoDesc.NumRenderTargets = 1;
	psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
	psoDesc.SampleDesc.Count = 1;

	hr = dev->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&g_psoFullscreen));
	if (FAILED(hr))
	{
		Msg("! R5: CreateGraphicsPipelineState failed 0x%08x", hr);
		return false;
	}

	// -----------------------------------------------------------------------
	// M3c: 立方体根签名（1 CBV）+ PSO + 静态几何
	// -----------------------------------------------------------------------

	// 根签名：1 个 CBV 描述符
	D3D12_ROOT_PARAMETER cubeParam = {};
	cubeParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	cubeParam.Descriptor.ShaderRegister = 0;
	cubeParam.Descriptor.RegisterSpace = 0;
	cubeParam.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

	D3D12_ROOT_SIGNATURE_DESC cubeRsDesc = {};
	cubeRsDesc.NumParameters = 1;
	cubeRsDesc.pParameters = &cubeParam;
	cubeRsDesc.NumStaticSamplers = 0;
	cubeRsDesc.pStaticSamplers = nullptr;
	cubeRsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

	ComPtr<ID3DBlob> cubeSigBlob;
	ComPtr<ID3DBlob> cubeSigErr;
	hr = D3D12SerializeRootSignature(&cubeRsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &cubeSigBlob, &cubeSigErr);
	if (FAILED(hr))
	{
		if (cubeSigErr)
			Msg("! R5 cube root sig serialize error: %s", (const char*)cubeSigErr->GetBufferPointer());
		return false;
	}

	hr = dev->CreateRootSignature(0, cubeSigBlob->GetBufferPointer(), cubeSigBlob->GetBufferSize(), IID_PPV_ARGS(&g_cubeRootSig));
	if (FAILED(hr))
		return false;

	// 编译着色器
	ComPtr<ID3DBlob> cubeVs = CompileShaderBlob(g_hlslCube, "VSMain", "vs_5_1");
	ComPtr<ID3DBlob> cubePs = CompileShaderBlob(g_hlslCube, "PSMain", "ps_5_1");
	if (!cubeVs || !cubePs)
		return false;

	// 输入布局
	D3D12_INPUT_ELEMENT_DESC layout[] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	};

	// PSO
	D3D12_GRAPHICS_PIPELINE_STATE_DESC cubePsoDesc = {};
	cubePsoDesc.pRootSignature = g_cubeRootSig.Get();
	cubePsoDesc.VS = { cubeVs->GetBufferPointer(), cubeVs->GetBufferSize() };
	cubePsoDesc.PS = { cubePs->GetBufferPointer(), cubePs->GetBufferSize() };
	cubePsoDesc.InputLayout = { layout, _countof(layout) };
	cubePsoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	cubePsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	cubePsoDesc.RasterizerState.FrontCounterClockwise = FALSE;
	cubePsoDesc.RasterizerState.DepthClipEnable = TRUE;
	cubePsoDesc.BlendState.RenderTarget[0].BlendEnable = FALSE;
	cubePsoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
	cubePsoDesc.DepthStencilState.DepthEnable = TRUE;
	cubePsoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
	cubePsoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
	cubePsoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
	cubePsoDesc.SampleMask = UINT_MAX;
	cubePsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	cubePsoDesc.NumRenderTargets = 1;
	cubePsoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
	cubePsoDesc.SampleDesc.Count = 1;

	hr = dev->CreateGraphicsPipelineState(&cubePsoDesc, IID_PPV_ARGS(&g_cubePSO));
	if (FAILED(hr))
	{
		Msg("! R5: cube PSO failed 0x%08x", hr);
		return false;
	}

	// 静态几何：24 顶点立方体（每面独立法线）
	static const CubeVertex cubeVerts[24] = {
		// -Z
		{{-0.5f,-0.5f,-0.5f},{0,0,-1}}, {{0.5f,-0.5f,-0.5f},{0,0,-1}}, {{0.5f,0.5f,-0.5f},{0,0,-1}}, {{-0.5f,0.5f,-0.5f},{0,0,-1}},
		// +Z
		{{-0.5f,-0.5f,0.5f},{0,0,1}}, {{0.5f,-0.5f,0.5f},{0,0,1}}, {{0.5f,0.5f,0.5f},{0,0,1}}, {{-0.5f,0.5f,0.5f},{0,0,1}},
		// -X
		{{-0.5f,-0.5f,-0.5f},{-1,0,0}}, {{-0.5f,0.5f,-0.5f},{-1,0,0}}, {{-0.5f,0.5f,0.5f},{-1,0,0}}, {{-0.5f,-0.5f,0.5f},{-1,0,0}},
		// +X
		{{0.5f,-0.5f,-0.5f},{1,0,0}}, {{0.5f,0.5f,-0.5f},{1,0,0}}, {{0.5f,0.5f,0.5f},{1,0,0}}, {{0.5f,-0.5f,0.5f},{1,0,0}},
		// -Y
		{{-0.5f,-0.5f,-0.5f},{0,-1,0}}, {{-0.5f,-0.5f,0.5f},{0,-1,0}}, {{0.5f,-0.5f,0.5f},{0,-1,0}}, {{0.5f,-0.5f,-0.5f},{0,-1,0}},
		// +Y
		{{-0.5f,0.5f,-0.5f},{0,1,0}}, {{-0.5f,0.5f,0.5f},{0,1,0}}, {{0.5f,0.5f,0.5f},{0,1,0}}, {{0.5f,0.5f,-0.5f},{0,1,0}},
	};

	static const u16 cubeIdx[36] = {
		0,1,2, 0,2,3,	// -Z
		4,6,5, 4,7,6,	// +Z
		8,9,10, 8,10,11,	// -X
		12,14,13, 12,15,14,	// +X
		16,17,18, 16,18,19,	// -Y
		20,22,21, 20,23,22,	// +Y
	};

	// 顶点缓冲
	D3D12_GPU_VIRTUAL_ADDRESS vbAddr;
	CubeVertex* vbCpu = (CubeVertex*)r5_res::g_upload.Alloc(sizeof(cubeVerts), vbAddr);
	memcpy(vbCpu, cubeVerts, sizeof(cubeVerts));
	g_cubeVB = vbAddr;

	// 索引缓冲
	D3D12_GPU_VIRTUAL_ADDRESS ibAddr;
	u16* ibCpu = (u16*)r5_res::g_upload.Alloc(sizeof(cubeIdx), ibAddr);
	memcpy(ibCpu, cubeIdx, sizeof(cubeIdx));
	g_cubeIB = ibAddr;

	// 常量缓冲（256 对齐，M3c 单对象）
	D3D12_GPU_VIRTUAL_ADDRESS cbAddr;
	g_cubeCBPtr = r5_res::g_upload.Alloc(256, cbAddr);
	g_cubeCBV = cbAddr;

	// 保护静态数据
	r5_res::g_upload.MarkPersist();

	// M3b: 内部 RT + 拷贝 PSO
	if (!CreateSceneRTAndCopyPSO(dev, Device.TargetWidth, Device.TargetHeight))
	{
		Msg("! R5: scene RT / copy PSO init failed");
	}

	Msg("* R5: pipeline initialized (root sig + fullscreen PSO + cube PSO)");
	return true;
}

void r5_pipeline::Shutdown()
{
	g_copyPSO.Reset();
	g_copyRootSig.Reset();
	g_sceneRT.Reset();
	g_rtvHeap.Reset();
	g_cubePSO.Reset();
	g_cubeRootSig.Reset();
	g_psoFullscreen.Reset();
	g_rootSig.Reset();
}

// ---------------------------------------------------------------------------
// 帧管理
// ---------------------------------------------------------------------------

void r5_pipeline::BeginFrame()
{
	dx12::BeginFrame();
}

void r5_pipeline::EndFrame()
{
	dx12::EndFrame();
}

// ---------------------------------------------------------------------------
// 全屏三角形 pass
// ---------------------------------------------------------------------------

void r5_pipeline::DrawFullscreenTriangle()
{
	ID3D12GraphicsCommandList* cmd = dx12::GetCmdList();
	if (!cmd || !g_psoFullscreen)
		return;

	D3D12_CPU_DESCRIPTOR_HANDLE rtv = dx12::GetCurrentRTV();
	D3D12_CPU_DESCRIPTOR_HANDLE dsv = dx12::GetCurrentDSV();

	// 视口/裁剪
	D3D12_VIEWPORT vp = {};
	vp.Width = (float)Device.TargetWidth;
	vp.Height = (float)Device.TargetHeight;
	vp.MaxDepth = 1.0f;
	cmd->RSSetViewports(1, &vp);

	D3D12_RECT sc = {};
	sc.right = (LONG)Device.TargetWidth;
	sc.bottom = (LONG)Device.TargetHeight;
	cmd->RSSetScissorRects(1, &sc);

	cmd->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
	cmd->SetPipelineState(g_psoFullscreen.Get());
	cmd->SetGraphicsRootSignature(g_rootSig.Get());
	cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	cmd->DrawInstanced(3, 1, 0, 0);
}

void r5_pipeline::DrawCube(float timeSec)
{
	ID3D12GraphicsCommandList* cmd = dx12::GetCmdList();
	if (!cmd || !g_cubePSO || !g_cubeCBPtr || !g_sceneRT)
		return;

	// MVP 矩阵（单位矩阵，立方体在屏幕中心）
	float angle = timeSec * 1.0f;
	float c = _cos(angle), s = _sin(angle);

	// 旋转矩阵 Y
	float mvp[16] = {
		c, 0, -s, 0,
		0, 1, 0, 0,
		s, 0, c, 0,
		0, 0, 0, 1
	};

	// 透视投影
	float fov = 60.0f * (3.14159265f / 180.0f);
	float aspect = (float)Device.TargetWidth / (float)Device.TargetHeight;
	float f = 1.0f / tanf(fov * 0.5f);
	float znear = 0.1f, zfar = 100.0f;

	float proj[16] = {
		f / aspect, 0, 0, 0,
		0, f, 0, 0,
		0, 0, (zfar + znear) / (znear - zfar), -1,
		0, 0, 2.0f * znear * zfar / (znear - zfar), 0
	};

	// 视图：相机在 (0, 0, -2.5)
	float view[16] = {
		1, 0, 0, 0,
		0, 1, 0, 0,
		0, 0, 1, 0,
		0, 0, -2.5f, 1
	};

	// view * proj
	float vp[16] = {};
	for (int i = 0; i < 4; ++i)
		for (int j = 0; j < 4; ++j)
			for (int k = 0; k < 4; ++k)
				vp[i * 4 + j] += view[i * 4 + k] * proj[k * 4 + j];

	// mvp * vp
	float finalMvp[16] = {};
	for (int i = 0; i < 4; ++i)
		for (int j = 0; j < 4; ++j)
			for (int k = 0; k < 4; ++k)
				finalMvp[i * 4 + j] += mvp[i * 4 + k] * vp[k * 4 + j];

	// 写入 CB
	float color[4] = { _cos(timeSec * 2.0f) * 0.5f + 0.5f, _sin(timeSec * 3.0f) * 0.5f + 0.5f, 0.8f, 1.0f };
	memcpy((u8*)g_cubeCBPtr, finalMvp, 64);
	memcpy((u8*)g_cubeCBPtr + 64, color, 16);

	D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_sceneRTV;
	D3D12_CPU_DESCRIPTOR_HANDLE dsv = dx12::GetCurrentDSV();

	// 场景 RT：PS SRV -> RT
	D3D12_RESOURCE_BARRIER toRT = {};
	toRT.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	toRT.Transition.pResource = g_sceneRT.Get();
	toRT.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	toRT.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
	cmd->ResourceBarrier(1, &toRT);

	D3D12_VIEWPORT vpPort = {};
	vpPort.Width = (float)Device.TargetWidth;
	vpPort.Height = (float)Device.TargetHeight;
	vpPort.MaxDepth = 1.0f;
	cmd->RSSetViewports(1, &vpPort);

	D3D12_RECT sc = {};
	sc.right = (LONG)Device.TargetWidth;
	sc.bottom = (LONG)Device.TargetHeight;
	cmd->RSSetScissorRects(1, &sc);

	cmd->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
	const float sceneClear[4] = { 0.0f, 0.0f, 0.3f, 1.0f };
	cmd->ClearRenderTargetView(rtv, sceneClear, 0, nullptr);
	cmd->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

	cmd->SetPipelineState(g_cubePSO.Get());
	cmd->SetGraphicsRootSignature(g_cubeRootSig.Get());

	// 绑定 CBV（root CBV，无需描述符表）
	cmd->SetGraphicsRootConstantBufferView(0, g_cubeCBV);

	// 顶点/索引缓冲（静态，使用记录的 GPU 地址）
	D3D12_VERTEX_BUFFER_VIEW vbv = {};
	vbv.BufferLocation = g_cubeVB;
	vbv.SizeInBytes = sizeof(CubeVertex) * 24;
	vbv.StrideInBytes = sizeof(CubeVertex);
	cmd->IASetVertexBuffers(0, 1, &vbv);

	D3D12_INDEX_BUFFER_VIEW ibv = {};
	ibv.BufferLocation = g_cubeIB;
	ibv.SizeInBytes = sizeof(u16) * 36;
	ibv.Format = DXGI_FORMAT_R16_UINT;
	cmd->IASetIndexBuffer(&ibv);

	cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	cmd->DrawIndexedInstanced(36, 1, 0, 0, 0);

	// 场景 RT：RT -> PS SRV（供拷贝 pass 采样）
	D3D12_RESOURCE_BARRIER toSRV = {};
	toSRV.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	toSRV.Transition.pResource = g_sceneRT.Get();
	toSRV.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
	toSRV.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	cmd->ResourceBarrier(1, &toSRV);
}

void r5_pipeline::DrawCopy()
{
	ID3D12GraphicsCommandList* cmd = dx12::GetCmdList();
	if (!cmd || !g_copyPSO || !g_sceneRT)
		return;

	D3D12_CPU_DESCRIPTOR_HANDLE rtv = dx12::GetCurrentRTV();
	D3D12_CPU_DESCRIPTOR_HANDLE dsv = dx12::GetCurrentDSV();

	// 绑定 GPU 描述符堆（SRV）
	ID3D12DescriptorHeap* heaps[] = { r5_res::g_gpuHeap.Heap() };
	cmd->SetDescriptorHeaps(1, heaps);

	D3D12_VIEWPORT vpPort = {};
	vpPort.Width = (float)Device.TargetWidth;
	vpPort.Height = (float)Device.TargetHeight;
	vpPort.MaxDepth = 1.0f;
	cmd->RSSetViewports(1, &vpPort);

	D3D12_RECT sc = {};
	sc.right = (LONG)Device.TargetWidth;
	sc.bottom = (LONG)Device.TargetHeight;
	cmd->RSSetScissorRects(1, &sc);

	cmd->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
	cmd->SetPipelineState(g_copyPSO.Get());
	cmd->SetGraphicsRootSignature(g_copyRootSig.Get());

	// 绑定 SRV 描述符表
	cmd->SetGraphicsRootDescriptorTable(0, g_copySRV_GPU);

	cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	cmd->DrawInstanced(3, 1, 0, 0);
}
