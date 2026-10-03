#include "stdafx.h"
#include "r5_pipeline.h"
#include "r5_resources.h"
#include "r5_dxr.h"
#include "r5_visual.h"
#include "r5_texture.h"

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

// M6: 顶点规范化布局 32B = pos + nrm + uv（与 R5Visual 子网格一致）
struct CubeVertex { float pos[3]; float nrm[3]; float uv[2]; };
static D3D12_GPU_VIRTUAL_ADDRESS g_cubeVB = 0;
static D3D12_GPU_VIRTUAL_ADDRESS g_cubeIB = 0;

// M4b: DXR 用 32 位索引副本（D3D12_RAYTRACING_GEOMETRY_DESC 仅支持 u32）+ 描述符暴露
static D3D12_GPU_VIRTUAL_ADDRESS g_cubeIB32 = 0;
static D3D12_GPU_DESCRIPTOR_HANDLE g_gbufUAV_GPU = {};	// RT0 的 UAV（GPU 堆槽位）

// M4a: G-buffer（RT0=albedo, RT1=normal）+ 合成源
static ComPtr<ID3D12Resource> g_gbufRT[2];
static ComPtr<ID3D12DescriptorHeap> g_rtvHeap;	// CPU-only RTV 堆（2 槽）
static D3D12_CPU_DESCRIPTOR_HANDLE g_gbufRTV[2] = {};
static UINT g_rtvDescriptorSize = 0;

// M4a: 合成 PSO（2×SRV 描述符表 + 静态采样器）
static ComPtr<ID3D12RootSignature> g_compRootSig;
static ComPtr<ID3D12PipelineState> g_compPSO;
static D3D12_CPU_DESCRIPTOR_HANDLE g_gbufSRV_CPU = {};	// 2 个连续 SRV 基址
static D3D12_GPU_DESCRIPTOR_HANDLE g_gbufSRV_GPU = {};

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

// M4a: 立方体着色器（MRT 输出 G-buffer：albedo + 法线）
// M4c/M6: 采样纹理（t2 + s0），使用顶点真实 UV
static const char* g_hlslCube = R"(
cbuffer CB : register(b0)
{
	row_major float4x4 mvp;
	row_major float4x4 world;
	float4 color;
};

Texture2D g_texture : register(t2);
SamplerState g_smp : register(s0);

struct VSIn
{
	float3 pos : POSITION;
	float3 nrm : NORMAL;
	float2 uv  : TEXCOORD0;
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
	o.nrm = mul(float4(i.nrm, 0.0), world).xyz;
	o.uv = i.uv;	// 顶点真实 UV（M6）
	return o;
}

struct PSOut
{
	float4 albedo : SV_TARGET0;
	float4 normal : SV_TARGET1;
};

PSOut PSMain(VSOut i)
{
	PSOut o;
	float3 n = normalize(i.nrm);
	// M4c: BC3(DXT5) 带 alpha，采样后强制忽略 alpha 只取 RGB
	float3 tex = g_texture.Sample(g_smp, i.uv).rgb;
	o.albedo = float4(tex, 1.0);	// 去掉 color 调制，直接显示纹理
	o.normal = float4(n * 0.5 + 0.5, 1.0);
	return o;
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
// M4a: 创建 G-buffer（2×RT）+ 合成 PSO（多 SRV 描述符表）
// ---------------------------------------------------------------------------

static bool CreateGBufferAndComposePSO(ID3D12Device* dev, UINT width, UINT height)
{
	// RTV 堆（CPU-only，2 槽）
	D3D12_DESCRIPTOR_HEAP_DESC rtvHd = {};
	rtvHd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	rtvHd.NumDescriptors = 2;
	HRESULT hr = dev->CreateDescriptorHeap(&rtvHd, IID_PPV_ARGS(&g_rtvHeap));
	if (FAILED(hr))
		return false;

	g_rtvDescriptorSize = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	g_gbufRTV[0] = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
	g_gbufRTV[1].ptr = g_gbufRTV[0].ptr + g_rtvDescriptorSize;

	// SRV 描述符（GPU 堆，2 个连续槽）
	g_gbufSRV_CPU = r5_res::g_gpuHeap.AllocCPU(2);
	g_gbufSRV_GPU = r5_res::g_gpuHeap.AllocGPU(2);

	D3D12_HEAP_PROPERTIES heapProps = {};
	heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

	// RT0=albedo 清为深蓝（背景），RT1=normal 清为黑（标记无几何）
	static const float kAlbedoClear[4] = { 0.0f, 0.0f, 0.3f, 1.0f };
	static const float kNormalClear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	static const float* kClears[2] = { kAlbedoClear, kNormalClear };

	for (int i = 0; i < 2; ++i)
	{
		D3D12_CLEAR_VALUE clearVal = {};
		clearVal.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		clearVal.Color[0] = kClears[i][0];
		clearVal.Color[1] = kClears[i][1];
		clearVal.Color[2] = kClears[i][2];
		clearVal.Color[3] = kClears[i][3];

		D3D12_RESOURCE_DESC rtDesc = {};
		rtDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		rtDesc.Width = width;
		rtDesc.Height = height;
		rtDesc.DepthOrArraySize = 1;
		rtDesc.MipLevels = 1;
		rtDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		rtDesc.SampleDesc.Count = 1;
		rtDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
		// M4b: RT0 同时作为 DXR DispatchRays 的 UAV 输出
		if (i == 0)
			rtDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

		hr = dev->CreateCommittedResource(
			&heapProps, D3D12_HEAP_FLAG_NONE, &rtDesc,
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
			&clearVal,
			IID_PPV_ARGS(&g_gbufRT[i])
		);
		if (FAILED(hr))
			return false;

		dev->CreateRenderTargetView(g_gbufRT[i].Get(), nullptr, g_gbufRTV[i]);

		// 每个 RT 的 SRV（顺序对应合成 shader 的 t0/t1）
		D3D12_CPU_DESCRIPTOR_HANDLE srvCpu = g_gbufSRV_CPU;
		srvCpu.ptr += SIZE_T(i) * r5_res::g_gpuHeap.DescriptorSize();

		D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srvDesc.Texture2D.MipLevels = 1;
		dev->CreateShaderResourceView(g_gbufRT[i].Get(), &srvDesc, srvCpu);
	}

	// M4b: RT0 的 UAV 描述符（紧跟 2 个 SRV 之后，GPU 堆槽位 2）
	{
		D3D12_CPU_DESCRIPTOR_HANDLE uavCpu = r5_res::g_gpuHeap.AllocCPU(1);
		g_gbufUAV_GPU = r5_res::g_gpuHeap.AllocGPU(1);

		D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
		uavDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
		dev->CreateUnorderedAccessView(g_gbufRT[0].Get(), nullptr, &uavDesc, uavCpu);
	}

	// M4c: 纹理 SRV（GPU 堆槽位 3，在 G-buffer SRV×2 + UAV 之后，避免被覆盖）
	if (!r5_texture::Init())
	{
		Msg("! R5: texture init failed");
	}

	// 合成根签名：2 SRV 描述符表 + 静态采样器
	D3D12_DESCRIPTOR_RANGE srvRange = {};
	srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	srvRange.NumDescriptors = 2;	// t0 albedo, t1 normal
	srvRange.BaseShaderRegister = 0;
	srvRange.RegisterSpace = 0;
	srvRange.OffsetInDescriptorsFromTableStart = 0;

	D3D12_ROOT_PARAMETER compParam = {};
	compParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	compParam.DescriptorTable.NumDescriptorRanges = 1;
	compParam.DescriptorTable.pDescriptorRanges = &srvRange;
	compParam.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	D3D12_STATIC_SAMPLER_DESC sampler = {};
	sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
	sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.ShaderRegister = 0;
	sampler.RegisterSpace = 0;
	sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	D3D12_ROOT_SIGNATURE_DESC compRsDesc = {};
	compRsDesc.NumParameters = 1;
	compRsDesc.pParameters = &compParam;
	compRsDesc.NumStaticSamplers = 1;
	compRsDesc.pStaticSamplers = &sampler;
	compRsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

	ComPtr<ID3DBlob> sigBlob;
	ComPtr<ID3DBlob> sigErr;
	hr = D3D12SerializeRootSignature(&compRsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &sigErr);
	if (FAILED(hr))
	{
		if (sigErr)
			Msg("! R5 compose root sig error: %s", (const char*)sigErr->GetBufferPointer());
		return false;
	}

	hr = dev->CreateRootSignature(0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(), IID_PPV_ARGS(&g_compRootSig));
	if (FAILED(hr))
		return false;

	// 加载 gamedata 合成 shader
	ComPtr<ID3DBlob> vs = LoadShaderFromFile("gbuffer_compose.ps.hlsl", "VSMain", "vs_5_1");
	ComPtr<ID3DBlob> ps = LoadShaderFromFile("gbuffer_compose.ps.hlsl", "PSMain", "ps_5_1");
	if (!vs || !ps)
		return false;

	// PSO
	D3D12_GRAPHICS_PIPELINE_STATE_DESC compPsoDesc = {};
	compPsoDesc.pRootSignature = g_compRootSig.Get();
	compPsoDesc.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
	compPsoDesc.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
	compPsoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	compPsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	compPsoDesc.RasterizerState.DepthClipEnable = FALSE;
	compPsoDesc.BlendState.RenderTarget[0].BlendEnable = FALSE;
	compPsoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
	compPsoDesc.DepthStencilState.DepthEnable = FALSE;
	compPsoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
	compPsoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
	compPsoDesc.DSVFormat = DXGI_FORMAT_UNKNOWN;
	compPsoDesc.SampleMask = UINT_MAX;
	compPsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	compPsoDesc.NumRenderTargets = 1;
	compPsoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
	compPsoDesc.SampleDesc.Count = 1;

	hr = dev->CreateGraphicsPipelineState(&compPsoDesc, IID_PPV_ARGS(&g_compPSO));
	if (FAILED(hr))
	{
		Msg("! R5: compose PSO failed 0x%08x", hr);
		return false;
	}

	Msg("* R5: G-buffer (2xRT) + compose PSO created (%ux%u)", width, height);
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
	// M3c/M4c: 立方体根签名（CBV + SRV 表 t2 + 静态采样器 s0）+ PSO + 静态几何
	// -----------------------------------------------------------------------

	// param[0] = CBV (b0)
	D3D12_ROOT_PARAMETER cubeParams[2] = {};
	cubeParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	cubeParams[0].Descriptor.ShaderRegister = 0;
	cubeParams[0].Descriptor.RegisterSpace = 0;
	cubeParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

	// param[1] = SRV 描述符表 (t2)
	D3D12_DESCRIPTOR_RANGE srvRange = {};
	srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	srvRange.NumDescriptors = 1;
	srvRange.BaseShaderRegister = 2;	// t2
	srvRange.RegisterSpace = 0;
	cubeParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	cubeParams[1].DescriptorTable.NumDescriptorRanges = 1;
	cubeParams[1].DescriptorTable.pDescriptorRanges = &srvRange;
	cubeParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	// 静态采样器 s0
	D3D12_STATIC_SAMPLER_DESC sampler = {};
	sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
	sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	sampler.ShaderRegister = 0;	// s0
	sampler.RegisterSpace = 0;
	sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	D3D12_ROOT_SIGNATURE_DESC cubeRsDesc = {};
	cubeRsDesc.NumParameters = 2;
	cubeRsDesc.pParameters = cubeParams;
	cubeRsDesc.NumStaticSamplers = 1;
	cubeRsDesc.pStaticSamplers = &sampler;
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

	// 输入布局（规范化 32B：pos@0 + nrm@12 + uv@24）
	D3D12_INPUT_ELEMENT_DESC layout[] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
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
	// M4a: MRT 输出到 G-buffer（RT0=albedo, RT1=normal）
	cubePsoDesc.NumRenderTargets = 2;
	cubePsoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
	cubePsoDesc.RTVFormats[1] = DXGI_FORMAT_R8G8B8A8_UNORM;
	cubePsoDesc.SampleDesc.Count = 1;

	hr = dev->CreateGraphicsPipelineState(&cubePsoDesc, IID_PPV_ARGS(&g_cubePSO));
	if (FAILED(hr))
	{
		Msg("! R5: cube PSO failed 0x%08x", hr);
		return false;
	}

	// 静态几何：24 顶点立方体（每面独立法线）+ 每面 box-mapping UV
	// UV 规则（对应旧 shader box mapping）：±X 面 pos.zy、±Y 面 pos.xz、±Z 面 pos.xy，各 +0.5
	static const CubeVertex cubeVerts[24] = {
		// -Z (nrm 0,0,-1): uv = pos.xy + 0.5
		{{-0.5f,-0.5f,-0.5f},{0,0,-1},{0,0}}, {{0.5f,-0.5f,-0.5f},{0,0,-1},{1,0}},
		{{0.5f,0.5f,-0.5f},{0,0,-1},{1,1}}, {{-0.5f,0.5f,-0.5f},{0,0,-1},{0,1}},
		// +Z (nrm 0,0,1): uv = pos.xy + 0.5
		{{-0.5f,-0.5f,0.5f},{0,0,1},{0,0}}, {{0.5f,-0.5f,0.5f},{0,0,1},{1,0}},
		{{0.5f,0.5f,0.5f},{0,0,1},{1,1}}, {{-0.5f,0.5f,0.5f},{0,0,1},{0,1}},
		// -X (nrm -1,0,0): uv = pos.zy + 0.5 (z→u, y→v)
		{{-0.5f,-0.5f,-0.5f},{-1,0,0},{0,0}}, {{-0.5f,0.5f,-0.5f},{-1,0,0},{0,1}},
		{{-0.5f,0.5f,0.5f},{-1,0,0},{1,1}}, {{-0.5f,-0.5f,0.5f},{-1,0,0},{1,0}},
		// +X (nrm 1,0,0): uv = pos.zy + 0.5
		{{0.5f,-0.5f,-0.5f},{1,0,0},{0,0}}, {{0.5f,0.5f,-0.5f},{1,0,0},{0,1}},
		{{0.5f,0.5f,0.5f},{1,0,0},{1,1}}, {{0.5f,-0.5f,0.5f},{1,0,0},{1,0}},
		// -Y (nrm 0,-1,0): uv = pos.xz + 0.5
		{{-0.5f,-0.5f,-0.5f},{0,-1,0},{0,0}}, {{-0.5f,-0.5f,0.5f},{0,-1,0},{1,0}},
		{{0.5f,-0.5f,0.5f},{0,-1,0},{1,1}}, {{0.5f,-0.5f,-0.5f},{0,-1,0},{0,1}},
		// +Y (nrm 0,1,0): uv = pos.xz + 0.5
		{{-0.5f,0.5f,-0.5f},{0,1,0},{0,0}}, {{-0.5f,0.5f,0.5f},{0,1,0},{1,0}},
		{{0.5f,0.5f,0.5f},{0,1,0},{1,1}}, {{0.5f,0.5f,-0.5f},{0,1,0},{0,1}},
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

	// M4b: DXR 用 32 位索引副本（光线生成/命中 shader 通过 raw SRV 读取）
	static const u32 cubeIdx32[36] = {
		0,1,2, 0,2,3,	4,6,5, 4,7,6,	8,9,10, 8,10,11,
		12,14,13, 12,15,14,	16,17,18, 16,18,19,	20,22,21, 20,23,22,
	};
	D3D12_GPU_VIRTUAL_ADDRESS ib32Addr;
	u32* ib32Cpu = (u32*)r5_res::g_upload.Alloc(sizeof(cubeIdx32), ib32Addr);
	memcpy(ib32Cpu, cubeIdx32, sizeof(cubeIdx32));
	g_cubeIB32 = ib32Addr;

	// 常量缓冲（256 对齐，M3c 单对象）
	D3D12_GPU_VIRTUAL_ADDRESS cbAddr;
	g_cubeCBPtr = r5_res::g_upload.Alloc(256, cbAddr);
	g_cubeCBV = cbAddr;

	// 保护静态数据
	r5_res::g_upload.MarkPersist();

	// M4a: G-buffer + 合成 PSO
	if (!CreateGBufferAndComposePSO(dev, Device.TargetWidth, Device.TargetHeight))
	{
		Msg("! R5: G-buffer / compose PSO init failed");
	}

	// M4b: DXR（失败时 Ready()=false，回退光栅路径）
	r5_dxr::Init();

	Msg("* R5: pipeline initialized (root sig + fullscreen PSO + cube PSO)");
	return true;
}

void r5_pipeline::Shutdown()
{
	r5_dxr::Shutdown();
	r5_texture::Shutdown();
	g_compPSO.Reset();
	g_compRootSig.Reset();
	g_gbufRT[0].Reset();
	g_gbufRT[1].Reset();
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

// 写入常量缓冲：mvp@0, world@64, color@128
// M7: 每个视觉从每帧 upload ring 分配独立 CB（单一持久 CB 会被后写的视觉覆盖）
// M7: 调试场景自转轨道相机（不依赖游戏相机：renderer 切换时游戏停在菜单，相机不更新）
static D3D12_GPU_VIRTUAL_ADDRESS WriteCubeCB(const float model[16], float timeSec)
{
	// 相机绕原点公转（轨道展示台）
	float a = timeSec * 0.4f;
	float R = 4.0f;
	float eye[3] = { R * _cos(a), 1.2f, R * _sin(a) };
	float at[3] = { 0, 0, 0 };
	float up[3] = { 0, 1, 0 };

	// 列向量 lookAt（相机从 eye 看向 at，-Z 前向）
	float zx = eye[0] - at[0], zy = eye[1] - at[1], zz = eye[2] - at[2];
	float zlen = _sqrt(zx * zx + zy * zy + zz * zz);
	zx /= zlen; zy /= zlen; zz /= zlen;
	// x = normalize(cross(up, z))
	float xx = up[1] * zz - up[2] * zy, xy = up[2] * zx - up[0] * zz, xz = up[0] * zy - up[1] * zx;
	float xlen = _sqrt(xx * xx + xy * xy + xz * xz);
	xx /= xlen; xy /= xlen; xz /= xlen;
	// y = cross(z, x)
	float yx = zy * xz - zz * xy, yy = zz * xx - zx * xz, yz = zx * xy - zy * xx;

	float view[16] = {
		xx, yx, zx, 0,
		xy, yy, zy, 0,
		xz, yz, zz, 0,
		-(xx * eye[0] + xy * eye[1] + xz * eye[2]),
		-(yx * eye[0] + yy * eye[1] + yz * eye[2]),
		-(zx * eye[0] + zy * eye[1] + zz * eye[2]),
		1
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

	// view * proj
	float vp[16] = {};
	for (int i = 0; i < 4; ++i)
		for (int j = 0; j < 4; ++j)
			for (int k = 0; k < 4; ++k)
				vp[i * 4 + j] += view[i * 4 + k] * proj[k * 4 + j];

	// model * vp
	float finalMvp[16] = {};
	for (int i = 0; i < 4; ++i)
		for (int j = 0; j < 4; ++j)
			for (int k = 0; k < 4; ++k)
				finalMvp[i * 4 + j] += model[i * 4 + k] * vp[k * 4 + j];

	float color[4] = { _cos(timeSec * 2.0f) * 0.5f + 0.5f, _sin(timeSec * 3.0f) * 0.5f + 0.5f, 0.8f, 1.0f };

	// 每帧 upload ring 分配独立 CB（视觉几何已 MarkPersist 保护，不会冲突）
	D3D12_GPU_VIRTUAL_ADDRESS cbAddr;
	void* cbPtr = r5_res::g_upload.Alloc(256, cbAddr);
	memcpy((u8*)cbPtr, finalMvp, 64);
	memcpy((u8*)cbPtr + 64, model, 64);
	memcpy((u8*)cbPtr + 128, color, 16);
	return cbAddr;
}

// 把 G-buffer 从 PS SRV 切换为 RT 并清空（每个绘制批次开始一次）
// M7: 场景开始 —— 清 G-buffer + 绑定管线（只做一次，避免多视觉互相清掉）
void r5_pipeline::BeginScene()
{
	ID3D12GraphicsCommandList* cmd = dx12::GetCmdList();
	if (!cmd || !g_cubePSO || !g_gbufRT[0])
		return;

	D3D12_CPU_DESCRIPTOR_HANDLE dsv = dx12::GetCurrentDSV();

	// G-buffer：PS SRV -> RT（两张一起屏障）
	D3D12_RESOURCE_BARRIER toRT[2] = {};
	for (int i = 0; i < 2; ++i)
	{
		toRT[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		toRT[i].Transition.pResource = g_gbufRT[i].Get();
		toRT[i].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		toRT[i].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
	}
	cmd->ResourceBarrier(2, toRT);

	D3D12_VIEWPORT vpPort = {};
	vpPort.Width = (float)Device.TargetWidth;
	vpPort.Height = (float)Device.TargetHeight;
	vpPort.MaxDepth = 1.0f;
	cmd->RSSetViewports(1, &vpPort);

	D3D12_RECT sc = {};
	sc.right = (LONG)Device.TargetWidth;
	sc.bottom = (LONG)Device.TargetHeight;
	cmd->RSSetScissorRects(1, &sc);

	cmd->OMSetRenderTargets(2, g_gbufRTV, FALSE, &dsv);
	const float clearAlbedo[4] = { 0.0f, 0.0f, 0.3f, 1.0f };
	const float clearNormal[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	cmd->ClearRenderTargetView(g_gbufRTV[0], clearAlbedo, 0, nullptr);
	cmd->ClearRenderTargetView(g_gbufRTV[1], clearNormal, 0, nullptr);
	cmd->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

	// 绑定 GPU 描述符堆 + cube 管线 + 根签名
	ID3D12DescriptorHeap* heaps[] = { r5_res::g_gpuHeap.Heap() };
	cmd->SetDescriptorHeaps(1, heaps);
	cmd->SetPipelineState(g_cubePSO.Get());
	cmd->SetGraphicsRootSignature(g_cubeRootSig.Get());
	cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

// M7: 场景结束 —— G-buffer RT -> PS SRV（供合成 pass 采样）
void r5_pipeline::EndScene()
{
	ID3D12GraphicsCommandList* cmd = dx12::GetCmdList();
	if (!cmd || !g_gbufRT[0])
		return;
	D3D12_RESOURCE_BARRIER toSRV[2] = {};
	for (int i = 0; i < 2; ++i)
	{
		toSRV[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		toSRV[i].Transition.pResource = g_gbufRT[i].Get();
		toSRV[i].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
		toSRV[i].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	}
	cmd->ResourceBarrier(2, toSRV);
}

void r5_pipeline::DrawCube(float timeSec)
{
	ID3D12GraphicsCommandList* cmd = dx12::GetCmdList();
	if (!cmd || !g_cubePSO || !g_cubeCBPtr || !g_gbufRT[0])
		return;

	// 首帧：在打开的命令列表上录制纹理上传（Init 阶段命令列表关闭，命令会被丢弃）
	r5_texture::UploadFirstFrame(cmd);

	// 模型矩阵（旋转 Y）
	float angle = timeSec * 1.0f;
	float c = _cos(angle), s = _sin(angle);
	float model[16] = {
		c, 0, -s, 0,
		0, 1, 0, 0,
		s, 0, c, 0,
		0, 0, 0, 1
	};

	D3D12_GPU_VIRTUAL_ADDRESS cbAddr = WriteCubeCB(model, timeSec);
	BeginScene();
	cmd->SetGraphicsRootConstantBufferView(0, cbAddr);
	cmd->SetGraphicsRootDescriptorTable(1, r5_texture::GetTestSRV());

	// 顶点/索引缓冲（静态，使用记录的 GPU 地址）
	D3D12_VERTEX_BUFFER_VIEW vbv = {};
	vbv.BufferLocation = g_cubeVB;
	vbv.SizeInBytes = sizeof(CubeVertex) * 24;
	vbv.StrideInBytes = sizeof(CubeVertex);	// 32B（pos+nrm+uv）
	cmd->IASetVertexBuffers(0, 1, &vbv);

	D3D12_INDEX_BUFFER_VIEW ibv = {};
	ibv.BufferLocation = g_cubeIB;
	ibv.SizeInBytes = sizeof(u16) * 36;
	ibv.Format = DXGI_FORMAT_R16_UINT;
	cmd->IASetIndexBuffer(&ibv);

	cmd->DrawIndexedInstanced(36, 1, 0, 0, 0);
	EndScene();
}

void r5_pipeline::DrawVisual(const R5Visual& v, float timeSec)
{
	ID3D12GraphicsCommandList* cmd = dx12::GetCmdList();
	if (!cmd || !g_cubePSO || !g_cubeCBPtr || !g_gbufRT[0] || v.m_meshes.empty())
		return;

	r5_texture::UploadFirstFrame(cmd);

	// M7: 模型矩阵 = T(worldOffset) · (R_y · S · T(-c))
	// 调试场景：模型摆在世界固定坐标（轨道相机绕原点公转，全部可见）
	const Fsphere& s = v.m_vis.sphere;
	float sr = s.R > 0.0001f ? s.R : 1.0f;
	float sc = 1.0f / sr;

	float ang = timeSec * 1.5f;	// 慢速自转便于观察
	float cosA = _cos(ang), sinA = _sin(ang);
	float cx = s.P.x, cy = s.P.y, cz = s.P.z;

	// 3x3 = 缩放旋转；平移 = 世界偏移 + R_y·S·T(-c) 的本地平移
	float model[16] = {
		sc * cosA, 0, -sc * sinA, 0,
		0, sc, 0, 0,
		sc * sinA, 0, sc * cosA, 0,
		v.worldOffset.x - sc * (cosA * cx - sinA * cz),
		v.worldOffset.y - sc * cy,
		v.worldOffset.z - sc * (sinA * cx + cosA * cz),
		1
	};

	D3D12_GPU_VIRTUAL_ADDRESS cbAddr = WriteCubeCB(model, timeSec);
	cmd->SetGraphicsRootConstantBufferView(0, cbAddr);

	// 逐个绘制子网格（每个自带 stride/顶点格式/贴图）
	for (const R5Visual::SubMesh& sm : v.m_meshes)
	{
		// M6: 绑定子网格自己的贴图 SRV（缺失时回退测试纹理）
		cmd->SetGraphicsRootDescriptorTable(1, sm.texSRV.ptr != 0 ? sm.texSRV : r5_texture::GetTestSRV());

		D3D12_VERTEX_BUFFER_VIEW vbv = {};
		vbv.BufferLocation = sm.VB;
		vbv.SizeInBytes = (UINT64)sm.vCount * sm.stride;
		vbv.StrideInBytes = sm.stride;
		cmd->IASetVertexBuffers(0, 1, &vbv);

		D3D12_INDEX_BUFFER_VIEW ibv = {};
		ibv.BufferLocation = sm.IB;
		ibv.SizeInBytes = (UINT64)sm.iCount * 2;
		ibv.Format = DXGI_FORMAT_R16_UINT;
		cmd->IASetIndexBuffer(&ibv);

		cmd->DrawIndexedInstanced(sm.iCount, 1, 0, 0, 0);
	}
}

// ---------------------------------------------------------------------------
// M4b: DXR 数据暴露（供 r5_dxr 使用）
// ---------------------------------------------------------------------------

namespace r5_pipeline
{
	D3D12_GPU_VIRTUAL_ADDRESS GetCubeVB() { return g_cubeVB; }
	D3D12_GPU_VIRTUAL_ADDRESS GetCubeIB32() { return g_cubeIB32; }
	ID3D12Resource* GetGBufferRT(int i) { return g_gbufRT[i].Get(); }
	D3D12_GPU_DESCRIPTOR_HANDLE GetGBufferUAV() { return g_gbufUAV_GPU; }
	D3D12_CPU_DESCRIPTOR_HANDLE GetGBufferRTV(int i) { return g_gbufRTV[i]; }
}

// M4a: G-buffer 合成 pass（采样 albedo+normal，简单 Lambert 光照输出 backbuffer）
void r5_pipeline::DrawCompose()
{
	ID3D12GraphicsCommandList* cmd = dx12::GetCmdList();
	if (!cmd || !g_compPSO || !g_gbufRT[0])
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
	cmd->SetPipelineState(g_compPSO.Get());
	cmd->SetGraphicsRootSignature(g_compRootSig.Get());

	// 绑定 2×SRV 描述符表（t0=albedo, t1=normal）
	cmd->SetGraphicsRootDescriptorTable(0, g_gbufSRV_GPU);

	cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	cmd->DrawInstanced(3, 1, 0, 0);
}
