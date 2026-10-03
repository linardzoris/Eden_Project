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
// 辅助：编译着色器 blob
// ---------------------------------------------------------------------------

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

	Msg("* R5: pipeline initialized (root sig + fullscreen PSO + cube PSO)");
	return true;
}

void r5_pipeline::Shutdown()
{
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
	if (!cmd || !g_cubePSO || !g_cubeCBPtr)
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

	D3D12_CPU_DESCRIPTOR_HANDLE rtv = dx12::GetCurrentRTV();
	D3D12_CPU_DESCRIPTOR_HANDLE dsv = dx12::GetCurrentDSV();

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
}
