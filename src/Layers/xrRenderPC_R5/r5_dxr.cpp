#include "stdafx.h"
#include "r5_dxr.h"
#include "r5_pipeline.h"
#include "r5_resources.h"

#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

// ---------------------------------------------------------------------------
// 最小 DXC API 声明（系统无 dxcapi.h；GUID/签名与公开 dxcapi.h 一致）
// dxcompiler.dll + dxil.dll 随游戏分发到 bins 目录（exe 同目录 LoadLibrary）
// ---------------------------------------------------------------------------

#define DXC_CP_UTF8 65001

struct DxcBuffer
{
	LPCVOID Ptr;
	SIZE_T Size;
	UINT Encoding;
};

struct __declspec(uuid("8BA5FB08-5195-40E2-AC58-0D989C3A0102")) IDxcBlob : public IUnknown
{
	virtual LPCVOID STDMETHODCALLTYPE GetBufferPointer() = 0;
	virtual SIZE_T STDMETHODCALLTYPE GetBufferSize() = 0;
};

struct __declspec(uuid("87346FEA-2D3C-4F92-9D0C-9F0E01B0F9A1")) IDxcOperationResult : public IUnknown
{
	virtual HRESULT STDMETHODCALLTYPE GetStatus(HRESULT* pStatus) = 0;
	virtual HRESULT STDMETHODCALLTYPE GetResult(IDxcBlob** ppResult) = 0;
	virtual HRESULT STDMETHODCALLTYPE GetErrorBuffer(IDxcBlob** ppErrors) = 0;
};

struct __declspec(uuid("58346CDA-DDE7-4497-9461-6F87AF5E0659")) IDxcResult : public IDxcOperationResult
{
	virtual HRESULT STDMETHODCALLTYPE GetOutput(
		int kind, REFIID riid, LPVOID* ppvObject, IDxcBlob** ppOutputName) = 0;
	virtual UINT32 STDMETHODCALLTYPE GetNumOutputs() = 0;
	virtual int STDMETHODCALLTYPE GetOutputByIndex(UINT32 index) = 0;
	virtual BOOL STDMETHODCALLTYPE HasOutput(int kind) = 0;
};

struct __declspec(uuid("228B4687-5A6A-4730-900C-9702B2203F54")) IDxcCompiler3 : public IUnknown
{
	virtual HRESULT STDMETHODCALLTYPE Compile(
		const DxcBuffer* pSource,
		LPCWSTR* pArguments, UINT32 argCount,
		IUnknown* pIncludeHandler,	// IDxcIncludeHandler*，无 #include 时可为 nullptr
		REFIID riid, LPVOID* ppResult) = 0;
	virtual HRESULT STDMETHODCALLTYPE Disassemble(
		const DxcBuffer* pObject, REFIID riid, LPVOID* ppResult) = 0;
};

// CLSID_DxcCompiler（官方公开 GUID）
static const CLSID CLSID_DxcCompiler = {
	0x73e22d93, 0xe6ce, 0x47f3, { 0xb5, 0xbf, 0xf0, 0x66, 0x4f, 0x39, 0xc1, 0xb0 } };

typedef HRESULT(WINAPI* DxcCreateInstanceProc)(REFCLSID rclsid, REFIID riid, LPVOID* ppv);

static DxcCreateInstanceProc GetDXC()
{
	static DxcCreateInstanceProc s_proc = []() -> DxcCreateInstanceProc
	{
		// 先显式加载验证器：DXC 签名 DXIL 依赖 dxil.dll，未签名产物会被 D3D12 拒绝
		HMODULE hVal = LoadLibraryW(L"dxil.dll");
		Msg("* R5 DXR: dxil.dll preload %s (handle 0x%p, err %u)",
			hVal ? "ok" : "FAILED", (void*)hVal, hVal ? 0 : GetLastError());

		HMODULE h = LoadLibraryW(L"dxcompiler.dll");	// exe 同目录优先
		if (!h)
		{
			Msg("! R5 DXR: dxcompiler.dll not found");
			return nullptr;
		}
		return (DxcCreateInstanceProc)GetProcAddress(h, "DxcCreateInstance");
	}();
	return s_proc;
}

namespace dx12
{
	extern "C" ENGINE_API ID3D12Device* GetDevice();
	extern "C" ENGINE_API ID3D12GraphicsCommandList* GetCmdList();
}

// 把 D3D12 调试层 InfoQueue 消息转储到日志（-dxdebug 时有效，否则队列为空）
static void DumpInfoQueue()
{
	ComPtr<ID3D12InfoQueue> iq;
	if (FAILED(dx12::GetDevice()->QueryInterface(IID_PPV_ARGS(&iq))) || !iq)
	{
		Msg("! D3D12: InfoQueue unavailable");
		return;
	}
	UINT64 n = iq->GetNumStoredMessages();
	Msg("* D3D12 InfoQueue: %llu stored messages", (unsigned long long)n);
	for (UINT64 i = 0; i < n && i < 32; ++i)
	{
		SIZE_T len = 0;
		iq->GetMessage(i, nullptr, &len);
		xr_vector<u8> buf(len);
		D3D12_MESSAGE* m = (D3D12_MESSAGE*)buf.data();
		if (SUCCEEDED(iq->GetMessage(i, m, &len)) && m->pDescription)
			Msg("! D3D12 [%s]: %s", m->Severity == D3D12_MESSAGE_SEVERITY_ERROR ? "ERR" : "warn", m->pDescription);
	}
	iq->ClearStoredMessages();
}

// ---------------------------------------------------------------------------
// 用 DXC 在线编译 DXR lib shader（lib_6_3，旧 D3DCompiler 不支持）
// ---------------------------------------------------------------------------

static ComPtr<ID3DBlob> CompileDXRLibDXC(const char* relativePath)
{
	DxcCreateInstanceProc dxcCreate = GetDXC();
	if (!dxcCreate)
		return nullptr;

	string_path fullPath;
	xr_strconcat(fullPath, "gamedata\\shaders\\d3d12\\", relativePath);

	// 读源码到内存（DxcBuffer 直接引用，无需 IDxcUtils）
	FILE* f = fopen(fullPath, "rb");
	if (!f)
	{
		Msg("! R5 DXR: open %s failed", fullPath);
		return nullptr;
	}
	fseek(f, 0, SEEK_END);
	long fsize = ftell(f);
	fseek(f, 0, SEEK_SET);
	xr_vector<char> srcText(fsize);
	fread(srcText.data(), 1, fsize, f);
	fclose(f);

	ComPtr<IDxcCompiler3> compiler;
	HRESULT hr = dxcCreate(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler));
	if (FAILED(hr) || !compiler)
	{
		Msg("! R5 DXR: DxcCreateInstance failed 0x%08x", hr);
		return nullptr;
	}

	LPCWSTR args[] = {
		L"-T", L"lib_6_3",
		L"-O3",
	};

	DxcBuffer src = {};
	src.Ptr = srcText.data();
	src.Size = srcText.size();
	src.Encoding = DXC_CP_UTF8;

	ComPtr<IDxcResult> result;
	hr = compiler->Compile(&src, args, _countof(args), nullptr, IID_PPV_ARGS(&result));
	if (FAILED(hr) || !result)
	{
		Msg("! R5 DXR: DXC compile call failed 0x%08x", hr);
		return nullptr;
	}

	HRESULT compileHr = S_OK;
	result->GetStatus(&compileHr);
	if (FAILED(compileHr))
	{
		ComPtr<IDxcBlob> errBlob;
		if (SUCCEEDED(result->GetErrorBuffer(&errBlob)) && errBlob && errBlob->GetBufferSize() > 0)
			Msg("! R5 DXR DXC error: %s", (const char*)errBlob->GetBufferPointer());
		Msg("! R5 DXR: DXC compile failed 0x%08x", compileHr);
		return nullptr;
	}

	// 取出 DXIL 字节码，转成 ID3DBlob 复用状态对象创建路径
	ComPtr<IDxcBlob> outBlob;
	result->GetResult(&outBlob);
	if (!outBlob)
		return nullptr;

	const u8* head = (const u8*)outBlob->GetBufferPointer();
	Msg("* R5 DXR: lib compiled, %llu bytes, magic=%02x %02x %02x %02x",
		(unsigned long long)outBlob->GetBufferSize(),
		outBlob->GetBufferSize() >= 4 ? head[0] : 0,
		outBlob->GetBufferSize() >= 4 ? head[1] : 0,
		outBlob->GetBufferSize() >= 4 ? head[2] : 0,
		outBlob->GetBufferSize() >= 4 ? head[3] : 0);

	// 枚举 DXBC 容器 part；DXIL 需 HASH part 且 digest 非零（验证器签名），否则运行时拒绝
	{
		const u8* base = (const u8*)outBlob->GetBufferPointer();
		SIZE_T total = outBlob->GetBufferSize();
		if (total >= 32 && memcmp(base, "DXBC", 4) == 0)
		{
			u32 partCount = *(const u32*)(base + 28);
			const u32* offsets = (const u32*)(base + 32);
			Msg("* R5 DXR: container parts=%u", partCount);
			for (u32 i = 0; i < partCount && i < 16; ++i)
			{
				u32 off = offsets[i];
				if (off + 8 > total)
					break;
				char fourcc[5] = {};
				memcpy(fourcc, base + off, 4);
				u32 psize = *(const u32*)(base + off + 4);
				if (memcmp(fourcc, "HASH", 4) == 0 && off + 8 + 20 <= total)
				{
					u32 flags = *(const u32*)(base + off + 8);
					const u8* digest = base + off + 12;
					bool zero = true;
					for (int b = 0; b < 16; ++b) if (digest[b]) { zero = false; break; }
					Msg("* R5 DXR: part[%u] %.4s size=%u flags=%u digestZero=%d", i, fourcc, psize, flags, (int)zero);
				}
				else if (memcmp(fourcc, "VERS", 4) == 0 && off + 8 + psize <= total && psize >= 8)
				{
					const u32* v = (const u32*)(base + off + 8);
					Msg("* R5 DXR: part[%u] VERS size=%u dwords=%u %u %u %u %u %u %u",
						i, psize, v[0], v[1], v[2], v[3], v[4], v[5], v[6]);
				}
				else
				{
					Msg("* R5 DXR: part[%u] %.4s size=%u", i, fourcc, psize);
				}
			}
		}
	}

	ComPtr<ID3DBlob> code;
	hr = D3DCreateBlob(outBlob->GetBufferSize(), &code);
	if (FAILED(hr))
		return nullptr;
	memcpy(code->GetBufferPointer(), outBlob->GetBufferPointer(), outBlob->GetBufferSize());
	return code;
}

// ---------------------------------------------------------------------------
// 状态
// ---------------------------------------------------------------------------

static ComPtr<ID3D12Device5> g_dev5;
static ComPtr<ID3D12RootSignature> g_globalSig;	// TLAS t0 + UAV u0 + CBV b0
static ComPtr<ID3D12RootSignature> g_localSig;	// SRV 表 t1,t2（IB/VB raw）
static ComPtr<ID3D12StateObject> g_dxrPSO;

static ComPtr<ID3D12Resource> g_blas;
static ComPtr<ID3D12Resource> g_tlas;
static ComPtr<ID3D12Resource> g_scratchBLAS;
static ComPtr<ID3D12Resource> g_scratchTLAS;

static D3D12_GPU_VIRTUAL_ADDRESS g_tlasVA = 0;
static D3D12_GPU_VIRTUAL_ADDRESS g_shaderTableVA = 0;	// 3 条记录，64B 步长

static bool g_ready = false;
static bool g_built = false;

// ---------------------------------------------------------------------------
// 工具
// ---------------------------------------------------------------------------

static ComPtr<ID3DBlob> LoadDXILLib(const char* relativePath)
{
	// M4b-fix2: 优先加载离线预编译 DXIL（.dxil 与 .hlsl 同名同目录），
	// 规避运行时 DXC 签名链（dxcompiler/dxil 版本错配导致未签名产物被 D3D12 拒绝）
	{
		string_path fullPath;
		xr_strconcat(fullPath, "gamedata\\shaders\\d3d12\\", relativePath);
		if (char* dot = strrchr(fullPath, '.'))
			strcpy_s(dot, sizeof(fullPath) - (dot - fullPath), ".dxil");

		if (FILE* f = fopen(fullPath, "rb"))
		{
			fseek(f, 0, SEEK_END);
			long fsize = ftell(f);
			fseek(f, 0, SEEK_SET);
			ComPtr<ID3DBlob> code;
			if (SUCCEEDED(D3DCreateBlob(fsize, &code)))
			{
				fread(code->GetBufferPointer(), 1, fsize, f);
				fclose(f);
				Msg("* R5 DXR: using precompiled %s (%ld bytes)", fullPath, fsize);
				return code;
			}
			fclose(f);
		}
	}

	// 回退：运行时 DXC 编译
	ComPtr<ID3DBlob> code = CompileDXRLibDXC(relativePath);
	if (!code)
		Msg("! R5 DXR: failed to compile %s", relativePath);
	return code;
}

static ComPtr<ID3D12Resource> CreateASBuffer(UINT64 size, D3D12_RESOURCE_STATES state)
{
	D3D12_HEAP_PROPERTIES hp = {};
	hp.Type = D3D12_HEAP_TYPE_DEFAULT;

	D3D12_RESOURCE_DESC desc = {};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	desc.Width = size;
	desc.Height = 1;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.SampleDesc.Count = 1;
	desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

	ComPtr<ID3D12Resource> res;
	HRESULT hr = dx12::GetDevice()->CreateCommittedResource(
		&hp, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&res)
	);
	if (FAILED(hr))
	{
		Msg("! R5 DXR: AS buffer alloc failed 0x%08x", hr);
		return nullptr;
	}
	return res;
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

bool r5_dxr::Init()
{
	ID3D12Device* dev = dx12::GetDevice();
	if (!dev)
		return false;

	// 设备能力检查
	if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&g_dev5))) || !g_dev5)
	{
		Msg("! R5 DXR: ID3D12Device5 unavailable, fallback to raster");
		return false;
	}

	D3D12_FEATURE_DATA_D3D12_OPTIONS5 opt5 = {};
	if (FAILED(g_dev5->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &opt5, sizeof(opt5))) ||
		opt5.RaytracingTier < D3D12_RAYTRACING_TIER_1_0)
	{
		Msg("! R5 DXR: raytracing tier unsupported, fallback to raster");
		g_dev5.Reset();
		return false;
	}

	HRESULT hr;

	// -----------------------------------------------------------------------
	// 全局根签名：root SRV t0（TLAS）、UAV 描述符表 u0（输出）、root CBV b0（相机）
	// -----------------------------------------------------------------------
	{
		// t0 (TLAS) 保持 root SRV；b0 (CBV) 保持 root CBV；
		// u0 (typed UAV) 必须走描述符表（D3D12 限制：typed UAV 不能是 root descriptor）
		D3D12_ROOT_PARAMETER params[3] = {};
		params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
		params[0].Descriptor.ShaderRegister = 0;
		params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		D3D12_DESCRIPTOR_RANGE uavRange = {};
		uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
		uavRange.NumDescriptors = 1;
		uavRange.BaseShaderRegister = 0;
		uavRange.RegisterSpace = 0;
		params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		params[1].DescriptorTable.NumDescriptorRanges = 1;
		params[1].DescriptorTable.pDescriptorRanges = &uavRange;
		params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
		params[2].Descriptor.ShaderRegister = 0;
		params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

		D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
		rsDesc.NumParameters = 3;
		rsDesc.pParameters = params;

		ComPtr<ID3DBlob> blob, errBlob;
		hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errBlob);
		if (FAILED(hr))
		{
			if (errBlob) Msg("! R5 DXR global sig: %s", (const char*)errBlob->GetBufferPointer());
			return false;
		}
		hr = dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g_globalSig));
		if (FAILED(hr)) return false;
	}

	// -----------------------------------------------------------------------
	// 局部根签名（命中组）：SRV 描述符表 t1,t2（IB/VB raw buffer）
	// -----------------------------------------------------------------------
	{
		D3D12_DESCRIPTOR_RANGE range = {};
		range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
		range.NumDescriptors = 2;
		range.BaseShaderRegister = 1;	// t1
		range.RegisterSpace = 0;

		D3D12_ROOT_PARAMETER param = {};
		param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		param.DescriptorTable.NumDescriptorRanges = 1;
		param.DescriptorTable.pDescriptorRanges = &range;
		param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

		D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
		rsDesc.NumParameters = 1;
		rsDesc.pParameters = &param;
		rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE;

		ComPtr<ID3DBlob> blob, errBlob;
		hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errBlob);
		if (FAILED(hr))
		{
			if (errBlob) Msg("! R5 DXR local sig: %s", (const char*)errBlob->GetBufferPointer());
			return false;
		}
		hr = dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g_localSig));
		if (FAILED(hr)) return false;
	}

	// -----------------------------------------------------------------------
	// DXIL 库 + 状态对象
	// -----------------------------------------------------------------------
	ComPtr<ID3DBlob> lib = LoadDXILLib("raytrace_cube.lib.hlsl");
	if (!lib)
		return false;

	D3D12_STATE_SUBOBJECT subobjs[7] = {};

	D3D12_DXIL_LIBRARY_DESC dxil = {};
	dxil.DXILLibrary.BytecodeLength = lib->GetBufferSize();
	dxil.DXILLibrary.pShaderBytecode = lib->GetBufferPointer();
	subobjs[0].Type = D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY;
	subobjs[0].pDesc = &dxil;

	D3D12_HIT_GROUP_DESC hg = {};
	hg.HitGroupExport = L"HitGroup";
	hg.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
	hg.ClosestHitShaderImport = L"ClosestHit";
	subobjs[1].Type = D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP;
	subobjs[1].pDesc = &hg;

	D3D12_RAYTRACING_SHADER_CONFIG scfg = {};
	scfg.MaxPayloadSizeInBytes = 16;	// float4 color
	scfg.MaxAttributeSizeInBytes = 8;	// float2 barycentrics
	subobjs[2].Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG;
	subobjs[2].pDesc = &scfg;

	D3D12_RAYTRACING_PIPELINE_CONFIG pcfg = {};
	pcfg.MaxTraceRecursionDepth = 1;
	subobjs[3].Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG;
	subobjs[3].pDesc = &pcfg;

	subobjs[4].Type = D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE;
	subobjs[4].pDesc = g_globalSig.GetAddressOf();

	subobjs[5].Type = D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE;
	subobjs[5].pDesc = g_localSig.GetAddressOf();

	LPCWSTR assocExports[] = { L"HitGroup" };
	D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION assoc = {};
	assoc.pSubobjectToAssociate = &subobjs[5];
	assoc.NumExports = 1;
	assoc.pExports = assocExports;
	subobjs[6].Type = D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION;
	subobjs[6].pDesc = &assoc;

	D3D12_STATE_OBJECT_DESC soDesc = {};
	soDesc.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
	soDesc.NumSubobjects = _countof(subobjs);
	soDesc.pSubobjects = subobjs;

	hr = g_dev5->CreateStateObject(&soDesc, IID_PPV_ARGS(&g_dxrPSO));
	if (FAILED(hr))
	{
		Msg("! R5 DXR: CreateStateObject failed 0x%08x", hr);
		DumpInfoQueue();

		// 诊断：E_INVALIDARG 时累积叠加 subobject，定位无效组合
		// 注意：射线追踪管线必须含 PIPELINE_CONFIG，单独 DXIL/HITGROUP 必然失败（假阳性）
		D3D12_STATE_SUBOBJECT sLib = { D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &dxil };
		D3D12_STATE_SUBOBJECT sShCfg = { D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &scfg };
		D3D12_STATE_SUBOBJECT sPipe = { D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pcfg };
		D3D12_STATE_SUBOBJECT sHG = { D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hg };
		D3D12_STATE_SUBOBJECT sGRS = { D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, g_globalSig.GetAddressOf() };
		D3D12_STATE_SUBOBJECT sLRS = { D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE, g_localSig.GetAddressOf() };
		D3D12_STATE_SUBOBJECT sAssoc = { D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION, &assoc };

		struct Combo { const char* name; D3D12_STATE_SUBOBJECT subs[7]; UINT num; };
		Combo combos[6] = {};
		combos[0].name = "LIB+PIPE";           combos[0].subs[0] = sLib; combos[0].subs[1] = sPipe; combos[0].num = 2;
		combos[1].name = "+SHADER_CFG";        combos[1].subs[0] = sLib; combos[1].subs[1] = sPipe; combos[1].subs[2] = sShCfg; combos[1].num = 3;
		combos[2].name = "+HIT_GROUP";         combos[2].subs[0] = sLib; combos[2].subs[1] = sPipe; combos[2].subs[2] = sShCfg; combos[2].subs[3] = sHG; combos[2].num = 4;
		combos[3].name = "+GLOBAL_RS";         combos[3].subs[0] = sLib; combos[3].subs[1] = sPipe; combos[3].subs[2] = sShCfg; combos[3].subs[3] = sHG; combos[3].subs[4] = sGRS; combos[3].num = 5;
		combos[4].name = "+LOCAL_RS";          combos[4].subs[0] = sLib; combos[4].subs[1] = sPipe; combos[4].subs[2] = sShCfg; combos[4].subs[3] = sHG; combos[4].subs[4] = sGRS; combos[4].subs[5] = sLRS; combos[4].num = 6;
		combos[5].name = "+ASSOC";             combos[5].subs[0] = sLib; combos[5].subs[1] = sPipe; combos[5].subs[2] = sShCfg; combos[5].subs[3] = sHG; combos[5].subs[4] = sGRS; combos[5].subs[5] = sLRS; combos[5].subs[6] = sAssoc; combos[5].num = 7;

		for (int i = 0; i < 6; ++i)
		{
			D3D12_STATE_OBJECT_DESC d1 = {};
			d1.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
			d1.NumSubobjects = combos[i].num;
			d1.pSubobjects = combos[i].subs;
			ComPtr<ID3D12StateObject> probe;
			HRESULT hr1 = g_dev5->CreateStateObject(&d1, IID_PPV_ARGS(&probe));
			Msg("! R5 DXR combo[%d] %-14s: 0x%08x", i, combos[i].name, hr1);
			if (FAILED(hr1))
				DumpInfoQueue();
		}
		return false;
	}

	ComPtr<ID3D12StateObjectProperties> soProps;
	g_dxrPSO->QueryInterface(IID_PPV_ARGS(&soProps));
	const void* idRayGen = soProps->GetShaderIdentifier(L"RayGen");
	const void* idMiss = soProps->GetShaderIdentifier(L"Miss");
	const void* idHitGroup = soProps->GetShaderIdentifier(L"HitGroup");
	if (!idRayGen || !idMiss || !idHitGroup)
		return false;

	// -----------------------------------------------------------------------
	// IB/VB raw SRV（GPU 堆槽位 3,4；上传环内地址换算成 R32 元素偏移）
	// -----------------------------------------------------------------------
	D3D12_GPU_VIRTUAL_ADDRESS ringBase = r5_res::g_upload.Resource()->GetGPUVirtualAddress();
	UINT64 vbOff = r5_pipeline::GetCubeVB() - ringBase;
	UINT64 ib32Off = r5_pipeline::GetCubeIB32() - ringBase;

	D3D12_CPU_DESCRIPTOR_HANDLE geoSRV_CPU = r5_res::g_gpuHeap.AllocCPU(2);
	D3D12_GPU_DESCRIPTOR_HANDLE geoSRV_GPU = r5_res::g_gpuHeap.AllocGPU(2);

	D3D12_SHADER_RESOURCE_VIEW_DESC rawDesc = {};
	rawDesc.Format = DXGI_FORMAT_R32_TYPELESS;
	rawDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
	rawDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	rawDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;

	rawDesc.Buffer.FirstElement = UINT(ib32Off / 4);
	rawDesc.Buffer.NumElements = 36;	// 36 × u32
	dev->CreateShaderResourceView(r5_res::g_upload.Resource(), &rawDesc, geoSRV_CPU);

	D3D12_CPU_DESCRIPTOR_HANDLE vbCpu = geoSRV_CPU;
	vbCpu.ptr += r5_res::g_gpuHeap.DescriptorSize();
	rawDesc.Buffer.FirstElement = UINT(vbOff / 4);
	rawDesc.Buffer.NumElements = 24 * 6;	// 24 顶点 × 6 float
	dev->CreateShaderResourceView(r5_res::g_upload.Resource(), &rawDesc, vbCpu);

	// -----------------------------------------------------------------------
	// Shader table：3 条记录（32B ID 对齐到 64B 步长），命中组附局部根参数
	// -----------------------------------------------------------------------
	{
		D3D12_GPU_VIRTUAL_ADDRESS tableVA;
		u8* table = (u8*)r5_res::g_upload.Alloc(256, tableVA);

		memcpy(table, idRayGen, 32);
		memcpy(table + 64, idMiss, 32);
		memcpy(table + 128, idHitGroup, 32);
		// 局部根参数：IB/VB SRV 描述符表 GPU 句柄（8 字节）
		memcpy(table + 128 + 32, &geoSRV_GPU.ptr, 8);

		g_shaderTableVA = tableVA;
	}

	r5_res::g_upload.MarkPersist();

	g_ready = true;
	Msg("* R5 DXR: raytracing pipeline ready (tier %u)", (u32)opt5.RaytracingTier);
	return true;
}

void r5_dxr::Shutdown()
{
	g_dxrPSO.Reset();
	g_localSig.Reset();
	g_globalSig.Reset();
	g_scratchTLAS.Reset();
	g_scratchBLAS.Reset();
	g_tlas.Reset();
	g_blas.Reset();
	g_dev5.Reset();
	g_ready = false;
	g_built = false;
}

bool r5_dxr::Ready()
{
	return g_ready;
}

// ---------------------------------------------------------------------------
// 首帧：构建 BLAS/TLAS + 清 normal RT
// ---------------------------------------------------------------------------

static bool BuildAccelStructs(ID3D12GraphicsCommandList4* cmd4)
{
	// BLAS 几何描述
	D3D12_RAYTRACING_GEOMETRY_DESC geo = {};
	geo.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
	geo.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
	geo.Triangles.VertexBuffer.StartAddress = r5_pipeline::GetCubeVB();
	geo.Triangles.VertexBuffer.StrideInBytes = 24;	// CubeVertex
	geo.Triangles.VertexCount = 24;
	geo.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
	geo.Triangles.IndexBuffer = r5_pipeline::GetCubeIB32();
	geo.Triangles.IndexCount = 36;
	geo.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;

	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS blasIn = {};
	blasIn.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
	blasIn.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE;
	blasIn.NumDescs = 1;
	blasIn.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
	blasIn.pGeometryDescs = &geo;

	D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO blasInfo = {};
	g_dev5->GetRaytracingAccelerationStructurePrebuildInfo(&blasIn, &blasInfo);

	g_scratchBLAS = CreateASBuffer(blasInfo.ScratchDataSizeInBytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	g_blas = CreateASBuffer(blasInfo.ResultDataMaxSizeInBytes, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
	if (!g_scratchBLAS || !g_blas)
		return false;

	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC blasBuild = {};
	blasBuild.Inputs = blasIn;
	blasBuild.ScratchAccelerationStructureData = g_scratchBLAS->GetGPUVirtualAddress();
	blasBuild.DestAccelerationStructureData = g_blas->GetGPUVirtualAddress();
	cmd4->BuildRaytracingAccelerationStructure(&blasBuild, 0, nullptr);

	// BLAS 完成 -> TLAS 输入
	D3D12_RESOURCE_BARRIER uavBar = {};
	uavBar.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
	uavBar.UAV.pResource = g_blas.Get();
	cmd4->ResourceBarrier(1, &uavBar);

	// 实例描述（上传环，静态）
	D3D12_GPU_VIRTUAL_ADDRESS instVA;
	D3D12_RAYTRACING_INSTANCE_DESC* inst =
		(D3D12_RAYTRACING_INSTANCE_DESC*)r5_res::g_upload.Alloc(sizeof(D3D12_RAYTRACING_INSTANCE_DESC), instVA);
	ZeroMemory(inst, sizeof(*inst));
	inst->Transform[0][0] = 1.0f;
	inst->Transform[1][1] = 1.0f;
	inst->Transform[2][2] = 1.0f;
	inst->InstanceID = 0;
	inst->InstanceMask = 0xFF;
	inst->InstanceContributionToHitGroupIndex = 0;
	inst->Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
	inst->AccelerationStructure = g_blas->GetGPUVirtualAddress();
	r5_res::g_upload.MarkPersist();

	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tlasIn = {};
	tlasIn.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
	tlasIn.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE;
	tlasIn.NumDescs = 1;
	tlasIn.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
	tlasIn.InstanceDescs = instVA;

	D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO tlasInfo = {};
	g_dev5->GetRaytracingAccelerationStructurePrebuildInfo(&tlasIn, &tlasInfo);

	g_scratchTLAS = CreateASBuffer(tlasInfo.ScratchDataSizeInBytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	g_tlas = CreateASBuffer(tlasInfo.ResultDataMaxSizeInBytes, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
	if (!g_scratchTLAS || !g_tlas)
		return false;

	D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC tlasBuild = {};
	tlasBuild.Inputs = tlasIn;
	tlasBuild.ScratchAccelerationStructureData = g_scratchTLAS->GetGPUVirtualAddress();
	tlasBuild.DestAccelerationStructureData = g_tlas->GetGPUVirtualAddress();
	cmd4->BuildRaytracingAccelerationStructure(&tlasBuild, 0, nullptr);

	g_tlasVA = g_tlas->GetGPUVirtualAddress();
	return true;
}

// normal RT 只需清一次（DXR 路径不写 RT1，保持 0 = 合成直通）
static void ClearNormalRTOnce(ID3D12GraphicsCommandList* cmd)
{
	ID3D12Resource* rt1 = r5_pipeline::GetGBufferRT(1);
	D3D12_CPU_DESCRIPTOR_HANDLE rtv1 = r5_pipeline::GetGBufferRTV(1);

	D3D12_RESOURCE_BARRIER toRT = {};
	toRT.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	toRT.Transition.pResource = rt1;
	toRT.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	toRT.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
	cmd->ResourceBarrier(1, &toRT);

	const float zero[4] = { 0, 0, 0, 0 };
	cmd->ClearRenderTargetView(rtv1, zero, 0, nullptr);

	D3D12_RESOURCE_BARRIER toSRV = {};
	toSRV.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	toSRV.Transition.pResource = rt1;
	toSRV.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
	toSRV.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	cmd->ResourceBarrier(1, &toSRV);
}

// ---------------------------------------------------------------------------
// 每帧：DispatchRays -> G-buffer RT0（UAV）
// ---------------------------------------------------------------------------

void r5_dxr::Render(float timeSec)
{
	if (!g_ready)
		return;

	ID3D12GraphicsCommandList* cmd = dx12::GetCmdList();
	if (!cmd)
		return;

	ComPtr<ID3D12GraphicsCommandList4> cmd4;
	if (FAILED(cmd->QueryInterface(IID_PPV_ARGS(&cmd4))) || !cmd4)
		return;

	if (!g_built)
	{
		if (!BuildAccelStructs(cmd4.Get()))
		{
			Msg("! R5 DXR: AS build failed, fallback to raster");
			g_ready = false;
			return;
		}
		ClearNormalRTOnce(cmd);
		g_built = true;
		Msg("* R5 DXR: BLAS/TLAS built");
	}

	// -----------------------------------------------------------------------
	// 相机 CB（环绕轨道，每帧上传环分配）
	// -----------------------------------------------------------------------
	float a = timeSec * 0.6f;
	float ex = _sin(a) * 2.5f, ey = 0.3f, ez = -_cos(a) * 2.5f;

	// fwd = normalize(at - eye), at = origin
	float fx = -ex, fy = -ey, fz = -ez;
	float fl = _sqrt(fx * fx + fy * fy + fz * fz);
	fx /= fl; fy /= fl; fz /= fl;

	// right = normalize(cross(fwd, up)), up = (0,1,0)
	float rx = fz * 1.0f - 0.0f, ry = 0.0f, rz = -fx;	// cross(f,(0,1,0)) = (fz*1? -> (fy*0-fz*1, fz*0-fx*0, fx*1-fy*0)) 简化
	// 明确计算：cross((fx,fy,fz),(0,1,0)) = (fy*0 - fz*1, fz*0 - fx*0, fx*1 - fy*0)
	rx = -fz; ry = 0.0f; rz = fx;
	float rl = _sqrt(rx * rx + ry * ry + rz * rz);
	rx /= rl; ry /= rl; rz /= rl;

	// up2 = cross(right, fwd)
	float ux = ry * fz - rz * fy;
	float uy = rz * fx - rx * fz;
	float uz = rx * fy - ry * fx;

	D3D12_GPU_VIRTUAL_ADDRESS camVA;
	float* cam = (float*)r5_res::g_upload.Alloc(256, camVA);
	cam[0] = ex; cam[1] = ey; cam[2] = ez; cam[3] = 0;
	cam[4] = rx; cam[5] = ry; cam[6] = rz; cam[7] = 0;
	cam[8] = ux; cam[9] = uy; cam[10] = uz; cam[11] = 0;
	cam[12] = fx; cam[13] = fy; cam[14] = fz; cam[15] = 0;
	cam[16] = tanf(60.0f * (3.14159265f / 180.0f) * 0.5f);
	cam[17] = (float)Device.TargetWidth / (float)Device.TargetHeight;
	cam[18] = timeSec; cam[19] = 0;

	// -----------------------------------------------------------------------
	// RT0: PS SRV -> UAV
	// -----------------------------------------------------------------------
	ID3D12Resource* rt0 = r5_pipeline::GetGBufferRT(0);

	D3D12_RESOURCE_BARRIER toUAV = {};
	toUAV.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	toUAV.Transition.pResource = rt0;
	toUAV.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	toUAV.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	cmd->ResourceBarrier(1, &toUAV);

	// -----------------------------------------------------------------------
	// DispatchRays
	// -----------------------------------------------------------------------
	ID3D12DescriptorHeap* heaps[] = { r5_res::g_gpuHeap.Heap() };
	cmd->SetDescriptorHeaps(1, heaps);

	cmd4->SetPipelineState1(g_dxrPSO.Get());
	cmd->SetComputeRootSignature(g_globalSig.Get());
	cmd->SetComputeRootShaderResourceView(0, g_tlasVA);
	cmd->SetComputeRootDescriptorTable(1, r5_pipeline::GetGBufferUAV());
	cmd->SetComputeRootConstantBufferView(2, camVA);

	D3D12_DISPATCH_RAYS_DESC dr = {};
	dr.RayGenerationShaderRecord.StartAddress = g_shaderTableVA;
	dr.RayGenerationShaderRecord.SizeInBytes = 64;
	dr.MissShaderTable.StartAddress = g_shaderTableVA + 64;
	dr.MissShaderTable.SizeInBytes = 64;
	dr.MissShaderTable.StrideInBytes = 64;
	dr.HitGroupTable.StartAddress = g_shaderTableVA + 128;
	dr.HitGroupTable.SizeInBytes = 64;
	dr.HitGroupTable.StrideInBytes = 64;
	dr.Width = Device.TargetWidth;
	dr.Height = Device.TargetHeight;
	dr.Depth = 1;
	cmd4->DispatchRays(&dr);

	// -----------------------------------------------------------------------
	// RT0: UAV -> PS SRV（供合成 pass）
	// -----------------------------------------------------------------------
	D3D12_RESOURCE_BARRIER toSRV = {};
	toSRV.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	toSRV.Transition.pResource = rt0;
	toSRV.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	toSRV.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	cmd->ResourceBarrier(1, &toSRV);
}
