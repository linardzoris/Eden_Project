#include "stdafx.h"
#include <d3d12sdklayers.h>
#include <dbghelp.h>
#include "DX12CommonTypes.h"
#include "dx12Backend.h"

#pragma comment(lib, "dbghelp.lib")

// dx12Types.h 的导出（资源状态跟踪器）：本文件不 include dx12Types.h（避免循环），
// 异步上传排空用它走共享状态账本（注意：定义在全局命名空间，声明须一致）
void R5TransitionResourceTracked(ID3D12GraphicsCommandList* cl, ID3D12Resource* r, D3D12_RESOURCE_STATES to);

namespace dx12
{
	Backend g_backend;

	//--------------------------------------------------------------------------
	// 诊断 VEH：识别上传路径上抛出的真实异常码与 C++ 对象类型名
	//--------------------------------------------------------------------------
	static thread_local bool t_inUploadPath = false;

	struct UploadPathGate
	{
		UploadPathGate()  { t_inUploadPath = true; }
		~UploadPathGate() { t_inUploadPath = false; }
	};

	struct ColX64
	{
		DWORD signature;
		DWORD offset;
		DWORD cdOffset;
		DWORD pTypeDescriptor;	// RVA
		DWORD pClassDescriptor;	// RVA
		DWORD pSelf;			// RVA
	};

	struct TypeDescX64
	{
		void* pVFTable;
		void* spare;
		char  name[1];
	};

	static void AppendProbe(const char* s)
	{
		HANDLE f = CreateFileA("D:\\Eden_Project\\veh_probe.txt", FILE_APPEND_DATA,
			FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (f != INVALID_HANDLE_VALUE)
		{
			DWORD wr = 0;
			WriteFile(f, s, (DWORD)strlen(s), &wr, nullptr);
			CloseHandle(f);
		}
	}

	static void ModuleOf(DWORD64 pc, char* out, size_t cap)
	{
		out[0] = 0;
		HMODULE mod = nullptr;
		if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			(LPCSTR)(uintptr_t)pc, &mod) && mod)
		{
			char path[MAX_PATH] = {};
			if (GetModuleFileNameA(mod, path, MAX_PATH))
			{
				const char* slash = strrchr(path, '\\');
				strncpy(out, slash ? slash + 1 : path, cap - 1);
				out[cap - 1] = 0;
			}
		}
		if (!out[0]) strncpy(out, "?", cap - 1);
	}

	// 记录调用栈：模块名 + 相对偏移（无符号也能据此判断来自哪个驱动/DLL）
	static void AppendStack(CONTEXT* ctx, int maxFrames)
	{
		STACKFRAME64 sf = {};
		sf.AddrPC.Mode = AddrModeFlat;
		sf.AddrStack.Mode = AddrModeFlat;
		sf.AddrFrame.Mode = AddrModeFlat;
		sf.AddrPC.Offset = ctx->Rip;
		sf.AddrStack.Offset = ctx->Rsp;
		sf.AddrFrame.Offset = ctx->Rsp;

		char symBuf[sizeof(IMAGEHLP_SYMBOL64) + 512];
		for (int i = 0; i < maxFrames; ++i)
		{
			if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, GetCurrentProcess(), GetCurrentThread(),
				&sf, ctx, nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) break;
			if (sf.AddrPC.Offset == 0) break;

			char modName[MAX_PATH] = {};
			ModuleOf(sf.AddrPC.Offset, modName, sizeof(modName));
			const DWORD64 modBase = SymGetModuleBase64(GetCurrentProcess(), sf.AddrPC.Offset);
			const DWORD64 off = sf.AddrPC.Offset - modBase;

			char line[760];
			IMAGEHLP_SYMBOL64* sym = (IMAGEHLP_SYMBOL64*)symBuf;
			sym->SizeOfStruct = sizeof(IMAGEHLP_SYMBOL64);
			sym->MaxNameLength = 512;
			DWORD64 disp = 0;
			if (SymGetSymFromAddr64(GetCurrentProcess(), sf.AddrPC.Offset, &disp, sym))
				xr_sprintf(line, "    #%02d %s+%I64u  %s\n", i, modName, off, sym->Name);
			else
				xr_sprintf(line, "    #%02d %s+%I64u\n", i, modName, off);
			AppendProbe(line);
		}
	}

	static BOOL CALLBACK EnumGfxModulesCb(PCSTR name, DWORD64 base, ULONG size, PVOID user)
	{
		(void)user;
		if (name && (strstr(name, "igd") || strstr(name, "\\igc") || strstr(name, "nvwgf") || strstr(name, "dxgi")))
		{
			char line[800];
			xr_sprintf(line, "  mod %s base=%p size=%u\n", name, (void*)base, (unsigned)size);
			AppendProbe(line);
		}
		return TRUE;
	}

	// x64 MSVC 异常结构
	struct ThrowInfoX64
	{
		DWORD attributes;
		DWORD pmfnUnwind;
		DWORD pForwardCompat;
		DWORD pCatchableTypeArray;
	};
	struct CatchableTypeArrayX64
	{
		DWORD nTypes;
		DWORD rvaTypes[1];
	};
	struct CatchableTypeX64
	{
		DWORD properties;
		DWORD pType;
		DWORD pmdDisp[3];
		DWORD sizeOrOffset;
		DWORD copyFunction;
	};

	// 经 ThrowInfo → CatchableType → TypeDescriptor 解析抛出类型名
	static bool ExtractThrownTypeName(EXCEPTION_RECORD* er, char* out, size_t outSize)
	{
		out[0] = 0;
		if (er->NumberParameters < 3) return false;
		ThrowInfoX64* ti = (ThrowInfoX64*)er->ExceptionInformation[2];

		HMODULE mod = nullptr;
		if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			(LPCSTR)ti, &mod) || !mod) return false;

		__try
		{
			CatchableTypeArrayX64* cta =
				(CatchableTypeArrayX64*)((u8*)mod + ti->pCatchableTypeArray);
			if (cta->nTypes == 0) return false;
			CatchableTypeX64* ct =
				(CatchableTypeX64*)((u8*)mod + cta->rvaTypes[0]);
			TypeDescX64* td = (TypeDescX64*)((u8*)mod + ct->pType);
			strncpy(out, td->name, outSize - 1);
			out[outSize - 1] = 0;
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {}
		return false;
	}

	static LONG CALLBACK DiagVEH(PEXCEPTION_POINTERS ep)
	{
		EXCEPTION_RECORD* er = ep->ExceptionRecord;
		const DWORD tid = GetCurrentThreadId();

		char typeName[256] = "";
		bool hasType = false;
		if (er->ExceptionCode == 0xE06D7363 && er->NumberParameters >= 3)
			hasType = ExtractThrownTypeName(er, typeName, sizeof(typeName));

		// 原始探针：不经引擎日志
		{
			HANDLE f = CreateFileA("D:\\Eden_Project\\veh_probe.txt", FILE_APPEND_DATA,
				FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (f != INVALID_HANDLE_VALUE)
			{
				char b[600];
				int n = hasType
					? xr_sprintf(b, "tid=%u code=0x%08x gate=%d addr=%p type=%s\n",
						(unsigned)tid, (unsigned)er->ExceptionCode, t_inUploadPath ? 1 : 0,
						er->ExceptionAddress, typeName)
					: xr_sprintf(b, "tid=%u code=0x%08x gate=%d addr=%p\n",
						(unsigned)tid, (unsigned)er->ExceptionCode, t_inUploadPath ? 1 : 0,
						er->ExceptionAddress);
				DWORD wr = 0;
				WriteFile(f, b, n, &wr, nullptr);
				CloseHandle(f);
			}
		}

		const bool intelThrow = hasType && (strstr(typeName, "MONZA") != nullptr);
		if (intelThrow)
		{
			static bool s_dumpedModules = false;
			if (!s_dumpedModules)
			{
				AppendProbe("  -- graphics modules loaded --\n");
				EnumerateLoadedModules64(GetCurrentProcess(), &EnumGfxModulesCb, nullptr);
				s_dumpedModules = true;
			}
			AppendProbe("  -- msg_end stack --\n");
			AppendStack(ep->ContextRecord, 14);
		}

		// AV 一律记录（首个崩溃线程可能不在上传门控内）
		if (er->ExceptionCode == 0xC0000005 && er->NumberParameters >= 2)
		{
			Msg("! DX12 VEH: tid=%u ACCESS VIOLATION %s addr=%p at %p (gate=%d)",
				(unsigned)tid, er->ExceptionInformation[0] ? "WRITE" : "read",
				er->ExceptionInformation[1], er->ExceptionAddress, t_inUploadPath ? 1 : 0);
			AppendProbe("  -- AV stack --\n");
			AppendStack(ep->ContextRecord, 16);
		}
		else
		{
			Msg("! DX12 VEH: tid=%u code=0x%08x addr=%p params=%u gate=%d type=%s",
				(unsigned)tid, (unsigned)er->ExceptionCode, er->ExceptionAddress,
				(unsigned)er->NumberParameters, t_inUploadPath ? 1 : 0,
				hasType ? typeName : "-");
		}
		return EXCEPTION_CONTINUE_SEARCH;
	}

	static void InstallDiagVEH()
	{
		// 默认不安装：仅当环境变量 R5_VEH=1 时启用，避免干扰引擎自身异常/符号处理。
		char ev[8] = {};
		DWORD evLen = GetEnvironmentVariableA("R5_VEH", ev, sizeof(ev));
		if (evLen == 0 || strcmp(ev, "1") != 0)
			return;

		static bool installed = false;
		if (!installed)
		{
			SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
			SymInitialize(GetCurrentProcess(), nullptr, TRUE);
			PVOID h = AddVectoredExceptionHandler(1, &DiagVEH);
			Msg("* DX12: VEH install handle=%p", h);
			installed = true;
		}
	}

	//--------------------------------------------------------------------------
	// 设备 / 命令列表访问（由 xrEngine 的 Device_create_render_dx12.cpp 暴露）
	//--------------------------------------------------------------------------
	ID3D12Device* GetD3D12Device()
	{
		// 走引擎导出接口（等价于 HWRenderDevice，但可跨 DLL 解析）
		return (ID3D12Device*)Device.GetRenderDevice();
	}

	ID3D12GraphicsCommandList* GetD3D12CmdList()
	{
		return dx12::GetCmdList();
	}

	// 上传 / 回读 / 立即清屏统一走主渲染队列（单队列串行化）。
	// 原先用独立 upload 队列：进关卡时纹理惰性上传会与渲染队列并发访问同一批纹理
	//（上传队列写 COPY_DEST、渲染队列同时按 ALL_SHADER_RESOURCE 采样），跨队列资源
	// 状态冲突会让命令处理器停在队列边界——此后所有命令列表都不再执行、围栏永不 signal，
	// 表现为"提交边界挂起"。GBV 只做队列内校验、-dxdebug 又因太慢而不重叠，两者都看不到。
	ID3D12CommandQueue* WorkQueue()
	{
		ID3D12CommandQueue* q = dx12::GetCommandQueue();
		return q ? q : g_backend.uploadQueue.Get();
	}

	void Ensure()
	{
		if (g_backend.valid) return;
		Init(GetD3D12Device());
	}

	//--------------------------------------------------------------------------
	// 设备挂起取证（DRED）
	// GPU 面包屑（RegisterBreadcrumbName / WriteBreadcrumb / DumpBreadcrumb）
	// 已迁移到设备层实现并导出，声明见 xrRenderPC_R5/stdafx.h
	//--------------------------------------------------------------------------
	static bool						s_dredDumped = false;

	static const char* BreadcrumbOpName(D3D12_AUTO_BREADCRUMB_OP op)
	{
		switch (op)
		{
		case D3D12_AUTO_BREADCRUMB_OP_SETMARKER: return " SetMarker";
		case D3D12_AUTO_BREADCRUMB_OP_BEGINEVENT: return " BeginEvent";
		case D3D12_AUTO_BREADCRUMB_OP_ENDEVENT: return " EndEvent";
		case D3D12_AUTO_BREADCRUMB_OP_DRAWINSTANCED: return " Draw";
		case D3D12_AUTO_BREADCRUMB_OP_DRAWINDEXEDINSTANCED: return " DrawIndexed";
		case D3D12_AUTO_BREADCRUMB_OP_EXECUTEINDIRECT: return " ExecuteIndirect";
		case D3D12_AUTO_BREADCRUMB_OP_DISPATCH: return " Dispatch";
		case D3D12_AUTO_BREADCRUMB_OP_COPYBUFFERREGION: return " CopyBuffer";
		case D3D12_AUTO_BREADCRUMB_OP_COPYTEXTUREREGION: return " CopyTexture";
		case D3D12_AUTO_BREADCRUMB_OP_COPYRESOURCE: return " CopyResource";
		case D3D12_AUTO_BREADCRUMB_OP_RESOLVESUBRESOURCE: return " ResolveSubres";
		case D3D12_AUTO_BREADCRUMB_OP_CLEARRENDERTARGETVIEW: return " ClearRTV";
		case D3D12_AUTO_BREADCRUMB_OP_CLEARUNORDEREDACCESSVIEW: return " ClearUAV";
		case D3D12_AUTO_BREADCRUMB_OP_CLEARDEPTHSTENCILVIEW: return " ClearDSV";
		case D3D12_AUTO_BREADCRUMB_OP_RESOURCEBARRIER: return " Barrier";
		case D3D12_AUTO_BREADCRUMB_OP_EXECUTEBUNDLE: return " ExecuteBundle";
		case D3D12_AUTO_BREADCRUMB_OP_PRESENT: return " PRESENT";
		case D3D12_AUTO_BREADCRUMB_OP_RESOLVEQUERYDATA: return " ResolveQuery";
		case D3D12_AUTO_BREADCRUMB_OP_BEGINSUBMISSION: return " BeginSubmission";
		case D3D12_AUTO_BREADCRUMB_OP_ENDSUBMISSION: return " EndSubmission";
		default: return " Op(?)";
		}
	}

	// DRED：设备移除后的自动面包屑（最后完成的命令序号）与页错误分配信息。
	// 无调试层时这是唯一能指出"哪个命令/哪块内存出了问题"的手段。
	static void DumpDRED(ID3D12Device* dev, const char* tag)
	{
		if (s_dredDumped || !dev) return;
		s_dredDumped = true;

		ComPtr<ID3D12DeviceRemovedExtendedData> dred;
		if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&dred))) || !dred)
		{
			Msg("! DX12 [%s]: DRED 不可用（设备/SDK 不支持）", tag ? tag : "?");
			return;
		}

		D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT bc = {};
		if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&bc)))
		{
			for (const D3D12_AUTO_BREADCRUMB_NODE* n = bc.pHeadAutoBreadcrumbNode; n; n = n->pNext)
			{
				const char* listName = (n->pCommandListDebugNameA && n->pCommandListDebugNameA[0])
					? n->pCommandListDebugNameA : "(unnamed cmdlist)";
				const UINT last = (n->pLastBreadcrumbValue && n->BreadcrumbCount) ? *n->pLastBreadcrumbValue : 0;
				Msg("! DX12 [%s]: DRED %s: breadcrumb %u / %u ops, cmdlist=%p",
					tag ? tag : "?", listName, last, (unsigned)n->BreadcrumbCount, (void*)n->pCommandList);

				// 逐操作历史：同时给出两种下标解读（DRED 文档用指针差；实测值可能不等于下标），
// 哪一种与帧号面包屑吻合就采用哪一种 —— 唯一能"说出卡在哪条操作上"的手段
				if (n->pCommandHistory && n->BreadcrumbCount)
				{
					const UINT* ph = (const UINT*)n->pCommandHistory;
					const UINT* pv = (const UINT*)n->pLastBreadcrumbValue;
					const UINT byVal = pv ? *pv : 0;
					const UINT byPtr = (pv && ph && pv >= ph && (UINT)(pv - ph) < n->BreadcrumbCount)
						? (UINT)(pv - ph) : UINT_MAX;

					auto dumpWindow = [&](const char* how, UINT idx)
					{
						if (idx == UINT_MAX || idx >= n->BreadcrumbCount) return;
						const UINT from = (idx > 4) ? (idx - 4) : 0;
						const UINT to = (idx + 4 < n->BreadcrumbCount) ? (idx + 4) : (n->BreadcrumbCount - 1);
						xr_string ops;
						for (UINT i = from; i <= to; ++i)
						{
							ops += (i == idx) ? " [卡住->]" : "";
							ops += BreadcrumbOpName(n->pCommandHistory[i]);
						}
						Msg("! DX12 [%s]: DRED ops(%s)[%u..%u]:%s", tag ? tag : "?", how, from, to, ops.c_str());
					};
					dumpWindow("byPtr", byPtr);
					dumpWindow("byVal", byVal);
				}
			}
		}

		D3D12_DRED_PAGE_FAULT_OUTPUT pf = {};
		if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&pf)))
		{
			Msg("! DX12 [%s]: DRED page fault VA=0x%llx", tag ? tag : "?", (unsigned long long)pf.PageFaultVA);
			for (const D3D12_DRED_ALLOCATION_NODE* a = pf.pHeadExistingAllocationNode; a; a = a->pNext)
				Msg("! DX12 [%s]: DRED alloc (in use) type=%d name='%s'",
					tag ? tag : "?", (int)a->AllocationType, a->ObjectNameA ? a->ObjectNameA : "?");
			for (const D3D12_DRED_ALLOCATION_NODE* a = pf.pHeadRecentFreedAllocationNode; a; a = a->pNext)
				Msg("! DX12 [%s]: DRED alloc (RECENTLY FREED) type=%d name='%s'",
					tag ? tag : "?", (int)a->AllocationType, a->ObjectNameA ? a->ObjectNameA : "?");
		}
	}

	//--------------------------------------------------------------------------
	// 诊断：设备移除原因 + info queue 存储消息
	//--------------------------------------------------------------------------
	void DumpDeviceErrors(const char* tag)
	{
		ID3D12Device* dev = GetD3D12Device();
		if (!dev) return;

		const HRESULT drr = dev->GetDeviceRemovedReason();
		if (FAILED(drr))
		{
			Msg("! DX12 [%s]: device removed reason 0x%08x", tag ? tag : "", (unsigned)drr);
			// 首次发现移除：输出 GPU 侧取证（面包屑 + DRED）
			DumpBreadcrumb(tag);
			DumpDRED(dev, tag);
		}

		ComPtr<ID3D12InfoQueue> iq;
		if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&iq))) || !iq) return;

		const UINT64 n = iq->GetNumStoredMessages();
		for (UINT64 i = 0; i < n; ++i)
		{
			SIZE_T len = 0;
			if (FAILED(iq->GetMessageW(i, nullptr, &len)) || !len) continue;
			D3D12_MESSAGE* m = (D3D12_MESSAGE*)xr_malloc(len);
			if (FAILED(iq->GetMessageW(i, m, &len))) { xr_free(m); continue; }
			if (m->Severity == D3D12_MESSAGE_SEVERITY_ERROR ||
				m->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION)
			{
				Msg("! DX12 IQ [%s]: %s", tag ? tag : "", m->pDescription);
			}
			xr_free(m);
		}
		iq->ClearStoredMessages();
	}

	//--------------------------------------------------------------------------
	// 初始化阶段立即清 RTV：复用一次性 direct 队列（资源创建时即处于 RENDER_TARGET 态）
	//--------------------------------------------------------------------------
	void ClearRTVImmediate(D3D12_CPU_DESCRIPTOR_HANDLE rtv, const FLOAT color[4])
	{
		Ensure();
		if (!g_backend.uploadQueue) return;

		std::lock_guard<std::mutex> glock(g_backend.gpuWorkMutex);
		ID3D12CommandAllocator* alloc = g_backend.uploadAlloc.Get();
		ID3D12GraphicsCommandList* list = g_backend.uploadList.Get();
		if (FAILED(alloc->Reset()) || FAILED(list->Reset(alloc, nullptr))) return;

		list->ClearRenderTargetView(rtv, color, 0, nullptr);
		list->Close();

		ID3D12CommandList* lists[] = { list };
		WorkQueue()->ExecuteCommandLists(1, lists);
		DumpDeviceErrors("clear_rtv_imm");
		g_backend.uploadFenceValue++;
		WorkQueue()->Signal(g_backend.uploadFence.Get(), g_backend.uploadFenceValue);
		if (g_backend.uploadFence->GetCompletedValue() < g_backend.uploadFenceValue && g_backend.uploadEvent)
		{
			g_backend.uploadFence->SetEventOnCompletion(g_backend.uploadFenceValue, (HANDLE)g_backend.uploadEvent);
			WaitForSingleObject((HANDLE)g_backend.uploadEvent, INFINITE);
		}
	}

	// 块压缩（BC）格式：返回每个 4x4 块的字节数；非压缩返回 0。
	static UINT BlockCompressedSize(DXGI_FORMAT fmt)
	{
		switch (fmt)
		{
		case DXGI_FORMAT_BC1_TYPELESS: case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC1_UNORM_SRGB:
		case DXGI_FORMAT_BC4_TYPELESS: case DXGI_FORMAT_BC4_UNORM: case DXGI_FORMAT_BC4_SNORM:
			return 8;
		case DXGI_FORMAT_BC2_TYPELESS: case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC2_UNORM_SRGB:
		case DXGI_FORMAT_BC3_TYPELESS: case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC3_UNORM_SRGB:
		case DXGI_FORMAT_BC5_TYPELESS: case DXGI_FORMAT_BC5_UNORM: case DXGI_FORMAT_BC5_SNORM:
		case DXGI_FORMAT_BC6H_TYPELESS: case DXGI_FORMAT_BC6H_UF16: case DXGI_FORMAT_BC6H_SF16:
		case DXGI_FORMAT_BC7_TYPELESS: case DXGI_FORMAT_BC7_UNORM: case DXGI_FORMAT_BC7_UNORM_SRGB:
			return 16;
		default:
			return 0;
		}
	}

	//--------------------------------------------------------------------------
	// 默认堆纹理的一次性上传（子资源 0）：暂存缓冲 + CopyTextureRegion + 围栏等待
	//--------------------------------------------------------------------------
	void UploadTextureSubresource(ID3D12Resource* dst, UINT subresourceIndex, const void* src, UINT srcRowPitch,
		UINT width, UINT height, UINT depth, DXGI_FORMAT fmt, DXGI_FORMAT footprintFormat,
		D3D12_RESOURCE_STATES curState)
	{
	try {
		UploadPathGate gate;
		if (!dst || !src) return;
		Ensure();
		if (!g_backend.uploadQueue) return;
		if (depth == 0) depth = 1;

		const UINT blockSize = BlockCompressedSize(fmt);
		const bool isBC = (blockSize != 0);

		// srcRowBytes：源数据每行（BC 为每个块行）的有效字节
		// copyRows  ：需要拷贝的行数（BC 为块行数）
		// pitch     ：目标缓冲按 256B 对齐后的行距
		UINT bpp = 4;
		UINT srcRowBytes = 0;
		UINT copyRows = 0;
		UINT pitch = 0;

		if (isBC)
		{
			// 块压缩：数据以 4x4 块组织；Footprint 的 Width/Height 仍传像素尺寸。
			const UINT blockW = (width + 3) / 4;
			const UINT blockH = (height + 3) / 4;
			srcRowBytes = blockW * blockSize;
			copyRows = blockH * depth;
			pitch = (srcRowBytes + (D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1)) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
		}
		else
		{
			switch (fmt)
			{
			case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_R8_SNORM: case DXGI_FORMAT_A8_UNORM: bpp = 1; break;
			case DXGI_FORMAT_R8G8_UNORM: case DXGI_FORMAT_R8G8_SNORM: bpp = 2; break;
			case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_UNORM:
			case DXGI_FORMAT_R32G32_FLOAT: bpp = 8; break;
			case DXGI_FORMAT_R32G32B32A32_FLOAT: case DXGI_FORMAT_R32G32B32A32_UINT:
			case DXGI_FORMAT_R32G32B32A32_SINT: bpp = 16; break;
			default: bpp = 4; break;
			}
			srcRowBytes = width * bpp;
			copyRows = height * depth;
			pitch = (srcRowBytes + (D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1)) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
		}

		// 校验：源图尺寸不得超过目标子资源的 mip 尺寸。D3D12 下越界拷贝会被判为非法调用并
		// 移除设备（且错误信息只在 device removed 后才可见）。这里提前拦截并打印上下文，
		// 避免设备移除，同时暴露源 mip 与目标 subresource 不匹配的真实纹理。
		{
			const D3D12_RESOURCE_DESC dd = dst->GetDesc();
			const UINT mip = dd.MipLevels ? (subresourceIndex % dd.MipLevels) : 0;
			const UINT dstW = (UINT)(dd.Width >> mip) ? (UINT)(dd.Width >> mip) : 1u;
			const UINT dstH = (UINT)(dd.Height >> mip) ? (UINT)(dd.Height >> mip) : 1u;
			if (width > dstW || height > dstH)
			{
				Msg("! DX12: upload size mismatch sub=%u fmt=%d rd=%llux%u mips=%u arr=%u mip=%u dst=%ux%u src=%ux%u d=%u -> skip",
					subresourceIndex, (int)fmt, (unsigned long long)dd.Width, (UINT)dd.Height,
					(UINT)dd.MipLevels, (UINT)dd.DepthOrArraySize, mip, dstW, dstH, width, height, depth);
				return;
			}
		}

		const UINT64 total = (UINT64)pitch * copyRows;

		D3D12_HEAP_PROPERTIES hp = {};
		hp.Type = D3D12_HEAP_TYPE_UPLOAD;
		D3D12_RESOURCE_DESC bd = {};
		bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		bd.Width = total; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
		bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

		ComPtr<ID3D12Resource> buf;
		if (FAILED(GetD3D12Device()->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&buf)))) return;

		void* p = nullptr;
		D3D12_RANGE rr = { 0, 0 };
		if (FAILED(buf->Map(0, &rr, &p)) || !p) return;
		const UINT sp = srcRowPitch ? srcRowPitch : srcRowBytes;
		const UINT copyBytes = srcRowBytes;
		for (UINT y = 0; y < copyRows; ++y)
			memcpy((u8*)p + (size_t)y * pitch, (const u8*)src + (size_t)y * sp, copyBytes);
		buf->Unmap(0, nullptr);

		std::lock_guard<std::mutex> glock(g_backend.gpuWorkMutex);
		ID3D12CommandAllocator* alloc = g_backend.uploadAlloc.Get();
		ID3D12GraphicsCommandList* list = g_backend.uploadList.Get();
		HRESULT hrA = alloc->Reset();
		HRESULT hrL = list->Reset(alloc, nullptr);
		if (FAILED(hrA) || FAILED(hrL))
			Msg("! DX12: upload reset failed alloc=0x%08x list=0x%08x (%ux%u d=%u fmt=%d)",
				(unsigned)hrA, (unsigned)hrL, width, height, depth, (int)fmt);

		// 目标资源当前状态 curState → COPY_DEST
		D3D12_RESOURCE_BARRIER b = {};
		b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		b.Transition.pResource = dst;
		b.Transition.StateBefore = curState;
		b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
		b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		list->ResourceBarrier(1, &b);

		D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
		dstLoc.pResource = dst;
		dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		dstLoc.SubresourceIndex = subresourceIndex;

		D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
		srcLoc.pResource = buf.Get();
		srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		srcLoc.PlacedFootprint.Offset = 0;
		srcLoc.PlacedFootprint.Footprint.Format = footprintFormat;
		srcLoc.PlacedFootprint.Footprint.Width = width;
		srcLoc.PlacedFootprint.Footprint.Height = height;
		srcLoc.PlacedFootprint.Footprint.Depth = depth;
		srcLoc.PlacedFootprint.Footprint.RowPitch = pitch;

		list->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

		b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
		b.Transition.StateAfter = curState;
		list->ResourceBarrier(1, &b);
		list->Close();

		ID3D12CommandList* lists[] = { list };
		WorkQueue()->ExecuteCommandLists(1, lists);
		DumpDeviceErrors("upload");
		g_backend.uploadFenceValue++;
		WorkQueue()->Signal(g_backend.uploadFence.Get(), g_backend.uploadFenceValue);
		if (g_backend.uploadFence->GetCompletedValue() < g_backend.uploadFenceValue && g_backend.uploadEvent)
		{
			g_backend.uploadFence->SetEventOnCompletion(g_backend.uploadFenceValue, (HANDLE)g_backend.uploadEvent);
			WaitForSingleObject((HANDLE)g_backend.uploadEvent, INFINITE);
		}
	}
	catch (const std::exception& e)
	{
		Msg("! DX12: upload path threw C++ exception: '%s' (%ux%u d=%u fmt=%d)",
			e.what(), width, height, depth, (int)fmt);
		throw;
	}
	catch (...)
	{
		Msg("! DX12: upload path threw unknown exception (%ux%u d=%u fmt=%d)",
			width, height, depth, (int)fmt);
		throw;
	}
	}

	//--------------------------------------------------------------------------
	// 异步纹理上传：加载线程入队 → 渲染线程帧首排空到主命令列表
	//
	// 即使上传统一到了主队列（WorkQueue），旧路径仍是"每张纹理一条独立的一次性
	// 命令列表"，在加载高峰期产生数千条列表，与帧列表交错提交；其屏障用的是调用方
	// 传入的 curState 参数而非共享状态跟踪器，与渲染列表的屏障簿记是两套账本。
	// 这里把加载路径的上传改为：入队（持 dst 引用 + 暂存字节）→ 帧首（主列表刚
	// Reset 后）字节进上传环、屏障走共享跟踪器、CopyTextureRegion 录进本帧主列表。
	// 上传与帧内绘制同列表按序执行，GPU 侧不再存在"帧间插入的独立列表"，
	// 状态账本唯一，dst 生命周期由 ComPtr 引用保证。
	//--------------------------------------------------------------------------
	struct PendingTextureUpload
	{
		ComPtr<ID3D12Resource>	dst;
		UINT					subresource = 0;
		u8*						data = nullptr;		// 暂存字节（按对齐 pitch 排布）
		UINT64					bytes = 0;
		UINT					rowPitch = 0;		// data 内的行距（256 对齐）
		UINT					width = 0, height = 0, depth = 1;
		DXGI_FORMAT				footprintFmt = DXGI_FORMAT_UNKNOWN;
	};
	static std::mutex						s_upMutex;
	static xr_vector<PendingTextureUpload>	s_uploads;
	static size_t							s_upHead = 0;	// FIFO 游标（避免 erase O(n²)）
	static UINT64							s_uploadPendingBytes = 0;

	// 与 UploadTextureSubresource 相同的布局计算（行距 256 对齐）
	static void UploadLayoutCalc(DXGI_FORMAT fmt, UINT width, UINT height, UINT depth,
		UINT& srcRowBytes, UINT& copyRows, UINT& pitch)
	{
		const UINT blockSize = BlockCompressedSize(fmt);
		if (blockSize)
		{
			const UINT blockW = (width + 3) / 4;
			const UINT blockH = (height + 3) / 4;
			srcRowBytes = blockW * blockSize;
			copyRows = blockH * depth;
		}
		else
		{
			UINT bpp = 4;
			switch (fmt)
			{
			case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_R8_SNORM: case DXGI_FORMAT_A8_UNORM: bpp = 1; break;
			case DXGI_FORMAT_R8G8_UNORM: case DXGI_FORMAT_R8G8_SNORM: bpp = 2; break;
			case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_UNORM:
			case DXGI_FORMAT_R32G32_FLOAT: bpp = 8; break;
			case DXGI_FORMAT_R32G32B32A32_FLOAT: case DXGI_FORMAT_R32G32B32A32_UINT:
			case DXGI_FORMAT_R32G32B32A32_SINT: bpp = 16; break;
			default: bpp = 4; break;
			}
			srcRowBytes = width * bpp;
			copyRows = height * depth;
		}
		pitch = (srcRowBytes + (D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1)) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
	}

	void EnqueueTextureUpload(ID3D12Resource* dst, UINT subresourceIndex, const void* src, UINT srcRowPitch,
		UINT width, UINT height, UINT depth, DXGI_FORMAT fmt, DXGI_FORMAT footprintFormat)
	{
		if (!dst || !src) return;
		// 帧循环未启动（初始化期）或后端异常时退回同步路径。
		// 帧录制中途（IsFrameRecording）也必须走同步：排空只在帧首执行，中途入队的
		// 上传最早也要到下一帧才落账，而本帧后续录制中的 Draw 可能已经把这张新纹理
		// 的 SRV 绑上去采样——"先采样后上传"读的是未初始化目标（实测页错误在目标
		// 纹理 VA 一带）。同步路径在本帧提交前就执行完拷贝并围栏等待，无此窗口。
		if (!g_backend.valid || g_backend.frameSerial == 0 || IsFrameRecording())
		{
			UploadTextureSubresource(dst, subresourceIndex, src, srcRowPitch, width, height, depth, fmt, footprintFormat);
			return;
		}
		if (depth == 0) depth = 1;

		// 与同步路径相同的护栏：引擎存在"源 mip 与目标 subresource 尺寸不匹配"的纹理
		//（LOD 缩减的 _DDS_2D Reduce 等），越界拷贝会让 GPU 沿目标分配之外写 —— 实测
		// 页错误落在目标纹理 VA 之外数百 MB（0x30b3b0000，环形时间线 + DRED 卡在排空区
		// 第 9 个 CopyTexture 三元组）。这里提前拦截并打印上下文，跳过该子资源。
		{
			const D3D12_RESOURCE_DESC dd = dst->GetDesc();
			const UINT mips = dd.MipLevels ? dd.MipLevels : 1;
			const UINT slices = dd.DepthOrArraySize ? dd.DepthOrArraySize : 1;
			const UINT mip = subresourceIndex % mips;
			const UINT dstW = (UINT)(dd.Width >> mip) ? (UINT)(dd.Width >> mip) : 1u;
			const UINT dstH = dd.Height ? ((UINT)(dd.Height >> mip) ? (UINT)(dd.Height >> mip) : 1u) : 1u;
			if (subresourceIndex >= mips * slices || width > dstW || height > dstH)
			{
				Msg("! DX12: async upload size mismatch sub=%u fmt=%d rd=%llux%u mips=%u arr=%u mip=%u dst=%ux%u src=%ux%u d=%u -> skip",
					subresourceIndex, (int)fmt, (unsigned long long)dd.Width, (UINT)dd.Height,
					mips, slices, mip, dstW, dstH, width, height, depth);
				return;
			}
		}

		UINT srcRowBytes = 0, copyRows = 0, pitch = 0;
		UploadLayoutCalc(fmt, width, height, depth, srcRowBytes, copyRows, pitch);
		const UINT64 total = (UINT64)pitch * copyRows;
		if (!total || total > (256ull << 20))	// 单个子资源 >256MB 属异常，走同步路径兜底
		{
			UploadTextureSubresource(dst, subresourceIndex, src, srcRowPitch, width, height, depth, fmt, footprintFormat);
			return;
		}

		PendingTextureUpload u;
		u.dst = dst;							// 持引用：排空前纹理被销毁也安全
		u.subresource = subresourceIndex;
		u.bytes = total;
		u.rowPitch = pitch;
		u.width = width; u.height = height; u.depth = depth;
		u.footprintFmt = footprintFormat;
		u.data = new u8[(size_t)total];
		const UINT sp = srcRowPitch ? srcRowPitch : srcRowBytes;
		for (UINT y = 0; y < copyRows; ++y)
			memcpy(u.data + (size_t)y * pitch, (const u8*)src + (size_t)y * sp, srcRowBytes);

		// 内存护栏：积压过大时退回同步上传，避免流送快于渲染时暂存字节无限增长
		bool sync = false;
		{
			std::lock_guard<std::mutex> g(s_upMutex);
			if (s_uploadPendingBytes + total > (768ull << 20)) sync = true;
			else
			{
				s_uploads.push_back(std::move(u));
				s_uploadPendingBytes += total;
			}
		}
		if (sync)
			UploadTextureSubresource(dst, subresourceIndex, src, srcRowPitch, width, height, depth, fmt, footprintFormat);
	}

	void ProcessPendingTextureUploads()
	{
		if (!g_backend.valid) return;
		ID3D12GraphicsCommandList* cl = GetD3D12CmdList();	// ::BeginFrame 刚 Reset 过的主列表
		if (!cl) return;

		UINT n = 0;
		UINT64 stagedBytes = 0;
		UINT64 ringBase = 0;
		{
			ID3D12Resource* ringRes = g_backend.ring.Resource();
			if (ringRes) ringBase = ringRes->GetGPUVirtualAddress();
		}

		std::lock_guard<std::mutex> g(s_upMutex);
		while (s_upHead < s_uploads.size() && stagedBytes < (96ull << 20))
		{
			PendingTextureUpload& u = s_uploads[s_upHead];
			UINT64 gpu = 0;
			void* p = g_backend.ring.Alloc(u.bytes, 256, gpu);
			if (!p) break;	// 本帧环段不足：留待下一帧

			memcpy(p, u.data, (size_t)u.bytes);

			// 屏障走共享状态跟踪器：与渲染列表同一份状态账本，StateBefore 恒真实
			R5TransitionResourceTracked(cl, u.dst.Get(), D3D12_RESOURCE_STATE_COPY_DEST);

			D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
			dstLoc.pResource = u.dst.Get();
			dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			dstLoc.SubresourceIndex = u.subresource;

			D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
			srcLoc.pResource = g_backend.ring.Resource();
			srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
			srcLoc.PlacedFootprint.Offset = gpu - ringBase;
			srcLoc.PlacedFootprint.Footprint.Format = u.footprintFmt;
			srcLoc.PlacedFootprint.Footprint.Width = u.width;
			srcLoc.PlacedFootprint.Footprint.Height = u.height;
			srcLoc.PlacedFootprint.Footprint.Depth = u.depth;
			srcLoc.PlacedFootprint.Footprint.RowPitch = u.rowPitch;

			cl->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

			// 加载路径创建初态即 ALL_SHADER_RESOURCE，上传完成回到该状态
			R5TransitionResourceTracked(cl, u.dst.Get(), D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);

			delete[] u.data; u.data = nullptr;
			s_uploadPendingBytes -= u.bytes;
			stagedBytes += u.bytes;
			++s_upHead;
			++n;
		}

		// 已消费条目回收（FIFO 游标前移后整体压实）
		if (s_upHead == s_uploads.size())
		{
			s_uploads.clear();
			s_upHead = 0;
		}
		else if (s_upHead > 256)
		{
			s_uploads.erase(s_uploads.begin(), s_uploads.begin() + s_upHead);
			s_upHead = 0;
		}

		if (n)
			WriteBreadcrumb(RegisterBreadcrumbName("frame:uploads"));
	}

	//--------------------------------------------------------------------------
	// 默认堆纹理子资源读回：READBACK 缓冲 + CopxTextureRegion + 围栏等待
	//--------------------------------------------------------------------------
	static UINT BppOf(DXGI_FORMAT fmt)
	{
		switch (fmt)
		{
		case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_R8_SNORM: case DXGI_FORMAT_A8_UNORM: return 1;
		case DXGI_FORMAT_R8G8_UNORM: case DXGI_FORMAT_R8G8_SNORM:
		case DXGI_FORMAT_R16_FLOAT: case DXGI_FORMAT_R16_UNORM: case DXGI_FORMAT_R16_SNORM: return 2;
		case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_UNORM:
		case DXGI_FORMAT_R32G32_FLOAT: case DXGI_FORMAT_D32_FLOAT: return 8;
		default: return 4;
		}
	}

	struct ReadbackEntry { ComPtr<ID3D12Resource> buf; UINT64 size = 0; UINT pitch = 0; };
	static std::unordered_map<ID3D12Resource*, ReadbackEntry> s_readbackCache;

	void* ReadbackTextureSubresource(ID3D12Resource* src, UINT subresourceIndex,
		UINT width, UINT height, DXGI_FORMAT fmt, UINT& outRowPitch,
		D3D12_RESOURCE_STATES curState)
	{
		outRowPitch = 0;
		if (!src) return nullptr;
		Ensure();
		if (!g_backend.uploadQueue) return nullptr;

		const UINT bpp = BppOf(fmt);
		const UINT pitch = (width * bpp + (D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1)) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
		const UINT64 need = (UINT64)pitch * height;

		ReadbackEntry& e = s_readbackCache[src];
		if (!e.buf || e.size < need)
		{
			D3D12_HEAP_PROPERTIES hp = {};
			hp.Type = D3D12_HEAP_TYPE_READBACK;
			D3D12_RESOURCE_DESC bd = {};
			bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			bd.Width = need; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
			bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			ComPtr<ID3D12Resource> nb;
			if (FAILED(GetD3D12Device()->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
				D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&nb)))) return nullptr;
			e.buf = nb; e.size = need;
		}
		e.pitch = pitch;
		outRowPitch = pitch;

		// 与 UploadTextureSubresource / ClearRTVImmediate 共用同一个上传命令列表与分配器，
		// 必须持同一把锁串行化：启动期预取线程并发上传纹理时，若不锁会与读回同时
		// Reset 同一 allocator/list，D3D12 判为非法调用并移除设备（0x887A0001）。
		std::lock_guard<std::mutex> glock(g_backend.gpuWorkMutex);
		ID3D12CommandAllocator* alloc = g_backend.uploadAlloc.Get();
		ID3D12GraphicsCommandList* list = g_backend.uploadList.Get();
		alloc->Reset();
		list->Reset(alloc, nullptr);

		D3D12_RESOURCE_BARRIER b = {};
		b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		b.Transition.pResource = src;
		b.Transition.StateBefore = curState;
		b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
		b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		list->ResourceBarrier(1, &b);

		D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
		dstLoc.pResource = e.buf.Get();
		dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		dstLoc.PlacedFootprint.Offset = 0;
		dstLoc.PlacedFootprint.Footprint.Format = fmt;
		dstLoc.PlacedFootprint.Footprint.Width = width;
		dstLoc.PlacedFootprint.Footprint.Height = height;
		dstLoc.PlacedFootprint.Footprint.Depth = 1;
		dstLoc.PlacedFootprint.Footprint.RowPitch = pitch;

		D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
		srcLoc.pResource = src;
		srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		srcLoc.SubresourceIndex = subresourceIndex;

		list->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

		b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
		b.Transition.StateAfter = curState;
		list->ResourceBarrier(1, &b);
		list->Close();

		ID3D12CommandList* lists[] = { list };
		WorkQueue()->ExecuteCommandLists(1, lists);
		g_backend.uploadFenceValue++;
		WorkQueue()->Signal(g_backend.uploadFence.Get(), g_backend.uploadFenceValue);
		if (g_backend.uploadFence->GetCompletedValue() < g_backend.uploadFenceValue && g_backend.uploadEvent)
		{
			g_backend.uploadFence->SetEventOnCompletion(g_backend.uploadFenceValue, (HANDLE)g_backend.uploadEvent);
			WaitForSingleObject((HANDLE)g_backend.uploadEvent, INFINITE);
		}

		void* p = nullptr;
		D3D12_RANGE rr = { 0, need };
		if (FAILED(e.buf->Map(0, &rr, &p))) return nullptr;
		return p;
	}

	//--------------------------------------------------------------------------
	// DescriptorHeap
	//--------------------------------------------------------------------------
	bool DescriptorHeap::Create(ID3D12Device* dev, D3D12_DESCRIPTOR_HEAP_TYPE type, UINT capacity, bool shaderVisible, UINT slots)
	{
		m_size = dev->GetDescriptorHandleIncrementSize(type);
		m_capacity = capacity;
		m_offset = 0;
		m_slots = (slots >= 2) ? 2 : 1;
		m_slot = 0;

		D3D12_DESCRIPTOR_HEAP_DESC desc = {};
		desc.Type = type;
		desc.NumDescriptors = capacity;
		desc.Flags = shaderVisible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
		desc.NodeMask = 0;

		for (UINT i = 0; i < m_slots; ++i)
		{
			if (FAILED(dev->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_heap[i]))))
			{
				Msg("! DX12: CreateDescriptorHeap failed (type=%d, count=%u, slot=%u)", (int)type, capacity, i);
				return false;
			}

			m_cpuStart[i] = m_heap[i]->GetCPUDescriptorHandleForHeapStart();
			if (shaderVisible)
				m_gpuStart[i] = m_heap[i]->GetGPUDescriptorHandleForHeapStart();
		}
		return true;
	}

	bool DescriptorHeap::Alloc(UINT count, D3D12_CPU_DESCRIPTOR_HANDLE& cpu, D3D12_GPU_DESCRIPTOR_HANDLE& gpu)
	{
		Ensure();
		if (m_offset + count > m_capacity)
		{
			Msg("! DX12: descriptor heap overflow (%u + %u > %u)", m_offset, count, m_capacity);
			return false;
		}
		cpu = CPU(m_offset);
		gpu = GPU(m_offset);
		m_offset += count;
		return true;
	}

	bool DescriptorHeap::AllocPersistent(UINT count, D3D12_CPU_DESCRIPTOR_HANDLE& cpu)
	{
		Ensure();
		// 锁放在 Ensure 之后：Ensure→Init 内部也走 AllocPersistent，依赖 valid=true
		// 已置位（Init 在堆创建完成后、首个持久分配前设置），不会重入。
		std::lock_guard<std::mutex> g(m_persistMutex);
		if (count == 1 && !m_persistFree.empty())
		{
			cpu = CPU(m_persistFree.back());
			m_persistFree.pop_back();
			return true;
		}
		if (m_persistOffset + count > m_capacity)
		{
			Msg("! DX12: persistent descriptor heap overflow (%u + %u > %u)", m_persistOffset, count, m_capacity);
			return false;
		}
		cpu = CPU(m_persistOffset);
		m_persistOffset += count;
		return true;
	}

	void DescriptorHeap::FreePersistent(UINT index)
	{
		std::lock_guard<std::mutex> g(m_persistMutex);
		if (index >= m_persistOffset) return;
		m_persistFree.push_back(index);
	}

	UINT DescriptorHeap::IndexOf(D3D12_CPU_DESCRIPTOR_HANDLE cpu) const
	{
		if (!m_size || cpu.ptr < m_cpuStart[m_slot].ptr) return UINT_MAX;
		return (UINT)((cpu.ptr - m_cpuStart[m_slot].ptr) / m_size);
	}

	D3D12_CPU_DESCRIPTOR_HANDLE DescriptorHeap::CPU(UINT index) const	{
		D3D12_CPU_DESCRIPTOR_HANDLE h = m_cpuStart[m_slot];
		h.ptr += (SIZE_T)index * m_size;
		return h;
	}

	D3D12_GPU_DESCRIPTOR_HANDLE DescriptorHeap::GPU(UINT index) const
	{
		D3D12_GPU_DESCRIPTOR_HANDLE h = m_gpuStart[m_slot];
		h.ptr += (UINT64)index * m_size;
		return h;
	}

	//--------------------------------------------------------------------------
	// UploadRing
	//--------------------------------------------------------------------------
	bool UploadRing::Create(ID3D12Device* dev, UINT64 size)
	{
		m_size = size;
		m_offset = 0;
		m_persist = 0;
		m_slot = 0;
		m_segEnd = size;	// 首次 Reset 前（初始化期）可整环线性分配

		D3D12_HEAP_PROPERTIES heapProps = {};
		heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

		D3D12_RESOURCE_DESC resDesc = {};
		resDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		resDesc.Width = size;
		resDesc.Height = 1;
		resDesc.DepthOrArraySize = 1;
		resDesc.MipLevels = 1;
		resDesc.Format = DXGI_FORMAT_UNKNOWN;
		resDesc.SampleDesc.Count = 1;
		resDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

		if (FAILED(dev->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &resDesc,
			D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_buffer))))
		{
			Msg("! DX12: upload ring create failed (%llu bytes)", size);
			return false;
		}

		D3D12_RANGE readRange = { 0, 0 };
		if (FAILED(m_buffer->Map(0, &readRange, &m_mapped)))
		{
			Msg("! DX12: upload ring map failed");
			return false;
		}
		m_vaBegin = m_buffer->GetGPUVirtualAddress();
		m_vaEnd = m_vaBegin + size;
		return true;
	}

	void UploadRing::Reset()
	{
		std::lock_guard<std::mutex> g(m_allocMutex);
		// 持久区 [0, m_persist) 是初始化期的静态数据（关卡 VB/IB 等），不随帧重置；
		// 其余空间按 backbuffer 分成两段，本帧只在本段内分配。
		const UINT64 usable = (m_size > m_persist) ? (m_size - m_persist) : 0;
		const UINT64 seg = usable / 2;
		m_segBase = m_persist + (UINT64)m_slot * seg;
		m_segEnd = (m_slot == 0) ? (m_persist + seg) : m_size;
		m_offset = m_segBase;
	}

	void* UploadRing::Alloc(UINT64 size, UINT64 align, D3D12_GPU_VIRTUAL_ADDRESS& gpuAddr)
	{
		Ensure();
		std::lock_guard<std::mutex> g(m_allocMutex);
		UINT64 offset = (m_offset + (align - 1)) & ~(align - 1);
		if (offset + size > m_segEnd)
		{
			Msg("! DX12: upload ring segment overflow (%llu + %llu > %llu, slot=%u)",
				offset, size, m_segEnd, m_slot);
			gpuAddr = 0;
			return nullptr;
		}
		m_offset = offset + size;
		gpuAddr = m_buffer->GetGPUVirtualAddress() + offset;
		return (u8*)m_mapped + offset;
	}

	//--------------------------------------------------------------------------
	// 根签名 / 静态采样器
	//--------------------------------------------------------------------------
	static const UINT kNumSRVSlots = 16;
	static const UINT kNumCBVSlots = 14;	// b0..b13

	D3D12_STATIC_SAMPLER_DESC GetStaticSampler(UINT slot)
	{
		D3D12_STATIC_SAMPLER_DESC s = {};
		s.Filter = D3D12_FILTER_ANISOTROPIC;
		s.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		s.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		s.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		s.MipLODBias = 0.0f;
		s.MaxAnisotropy = 16;
		s.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
		s.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
		s.MinLOD = 0.0f;
		s.MaxLOD = D3D12_FLOAT32_MAX;
		s.ShaderRegister = slot;
		s.RegisterSpace = 0;
		s.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		return s;
	}

	bool Init(ID3D12Device* dev)
	{
		InstallDiagVEH();
		if (g_backend.valid) return true;

		// shader-visible CBV/SRV/UAV 堆：每个 draw 固定占用 14 像素 CBV + 14 顶点 CBV
		// + 16 像素 SRV + 16 顶点 SRV（=60 个描述符，根签名范围固定，即使大部分槽位
		// 填占位符也必须分配）。
		// zaton 这类含大量草丛/细节的关卡单帧 draw 数可达 ~7000，60*7000≈42 万，故取 900000
		// （约 29MB，每帧重置）。注意 D3D12 限制 shader-visible CBV/SRV/UAV 堆最多 1,000,000
		// 个描述符，超过会直接 CreateDescriptorHeap 失败。容量不足时 Alloc 失败 → 根描述符表
		// 不绑定 → Draw 读到上一 Draw 的残留描述符，表现为"花屏闪烁"。
		// slots=2：按 backbuffer 各一份（单堆无法容纳两份，受 1M 上限约束），
		// 避免 CPU 录制下一帧时覆盖 GPU 仍在读的描述符。
		if (!g_backend.cbvSrvUav.Create(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 900000, true, 2)) return false;
		if (!g_backend.sampler.Create(dev, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 256, true)) return false;
		if (!g_backend.ring.Create(dev, 256ull * 1024 * 1024)) return false;
		{
			// 记录上传环 VA 区间：DRED 报告的页错误 VA 若落在该区间即可直接判定
			// "GPU 访问了上传环"，便于解读挂起取证
			ID3D12Resource* rr = g_backend.ring.Resource();
			const UINT64 va = rr ? rr->GetGPUVirtualAddress() : 0;
			Msg("* DX12: upload ring VA [0x%llx, 0x%llx)", (unsigned long long)va,
				(unsigned long long)(va + 256ull * 1024 * 1024));
		}

		// GPU 面包屑缓冲由设备层创建（xrEngine/Device_create_render_dx12.cpp）

		// 持久堆槽位当前不回收：材质系统会在画质/灯光切换时销毁重建纹理 SRV，
		// 8192 在完整关卡几分钟内即可耗尽。放大到 65536（非 shader-visible，仅 CPU 句柄开销）。
		if (!g_backend.persistentSrv.Create(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 65536, false)) return false;
		if (!g_backend.persistentUav.Create(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2048, false)) return false;
		if (!g_backend.persistentRtv.Create(dev, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2048, false)) return false;
		if (!g_backend.persistentDsv.Create(dev, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1024, false)) return false;

		// ---- 一次性上传通道 ----
		{
			D3D12_COMMAND_QUEUE_DESC qd = {};
			qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
			if (FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_backend.uploadQueue)))) return false;
			if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_backend.uploadAlloc)))) return false;
			if (FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_backend.uploadAlloc.Get(), nullptr, IID_PPV_ARGS(&g_backend.uploadList)))) return false;
			g_backend.uploadList->Close();
			if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_backend.uploadFence)))) return false;
			g_backend.uploadEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
			g_backend.uploadFenceValue = 0;
		}

		g_backend.rtvDescriptorSize = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
		g_backend.dsvDescriptorSize = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

		// ---- 根签名 ----
		D3D12_ROOT_PARAMETER params[4] = {};
		// [0] CBV 描述符表（像素阶段）：b0..b13
		D3D12_DESCRIPTOR_RANGE cbvRange = {};
		cbvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
		cbvRange.NumDescriptors = kNumCBVSlots;
		cbvRange.BaseShaderRegister = 0;
		cbvRange.RegisterSpace = 0;
		cbvRange.OffsetInDescriptorsFromTableStart = 0;
		params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		params[0].DescriptorTable.NumDescriptorRanges = 1;
		params[0].DescriptorTable.pDescriptorRanges = &cbvRange;
		params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
		// [1] SRV 描述符表（像素阶段）：t0..t15
		D3D12_DESCRIPTOR_RANGE srvRange = {};
		srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
		srvRange.NumDescriptors = kNumSRVSlots;
		srvRange.BaseShaderRegister = 0;
		srvRange.RegisterSpace = 0;
		srvRange.OffsetInDescriptorsFromTableStart = 0;
		params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		params[1].DescriptorTable.NumDescriptorRanges = 1;
		params[1].DescriptorTable.pDescriptorRanges = &srvRange;
		params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
		// [2] SRV 描述符表（顶点阶段）：t0..t15
		// D3D11 的 VS/PS 是各自独立的寄存器空间，同一 t0 可分别绑定不同资源；
		// 例如 deffer_detail.vs 读 StructuredBuffer(t0) 做草实例，而其 PS 的 t0 是纹理。
		// 单表(ALL) 会让 VS 拿到像素端的纹理描述符 → 实例数据全错（GBV 报
		// "SRV Dimension Expected: BUFFER, In Descriptor: TEXTURE2D, Shader Stage: VERTEX"）。
		D3D12_DESCRIPTOR_RANGE srvRangeVS = srvRange;
		params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		params[2].DescriptorTable.NumDescriptorRanges = 1;
		params[2].DescriptorTable.pDescriptorRanges = &srvRangeVS;
		params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

		// [3] CBV 描述符表（顶点阶段）：b0..b13
		// 与 [2] 同理：D3D11 的 VS/PS 常量缓冲是各自独立的寄存器空间，同一个 b0
		// 在 VS 和 PS 上可以（而且经常）绑定不同的 CB。原先只有一张 visibility=ALL
		// 的 CBV 表，绑定侧又写 cbVS[i] ? cbVS[i] : cbPS[i]，于是 PS 的 b0 被 VS 的
		// b0 顶掉：例如 phase_luminance 中 PS bloom_luminance_3 的 MiddleGray 被
		// VS stub_notransform_filter 的 screen_res 覆盖，曝光 scale 变成天文数字，
		// 表现为整屏纯白。拆表后两阶段各自独立。
		D3D12_DESCRIPTOR_RANGE cbvRangeVS = cbvRange;
		params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		params[3].DescriptorTable.NumDescriptorRanges = 1;
		params[3].DescriptorTable.pDescriptorRanges = &cbvRangeVS;
		params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

		xr_vector<D3D12_STATIC_SAMPLER_DESC> samplers;
		samplers.reserve(kNumSRVSlots);
		for (UINT i = 0; i < kNumSRVSlots; ++i)
			samplers.push_back(GetStaticSampler(i));

		D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
		rsDesc.NumParameters = 4;
		rsDesc.pParameters = params;
		rsDesc.NumStaticSamplers = (UINT)samplers.size();
		rsDesc.pStaticSamplers = samplers.data();
		rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

		ComPtr<ID3DBlob> sig, err;
		if (FAILED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)))
		{
			Msg("! DX12: D3D12SerializeRootSignature failed: %s", err ? (LPCSTR)err->GetBufferPointer() : "?");
			return false;
		}
		if (FAILED(dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&g_backend.rootSignature))))
		{
			Msg("! DX12: CreateRootSignature failed");
			return false;
		}

		InstallDiagVEH();
		g_backend.valid = true;
		Msg("* DX12: backend initialized (root sig + heaps + 256MB ring)");

		// ---- 1x1 黑色占位纹理 SRV（persistent 堆），用于 FlushPipeline 填充无效 SRV 槽 ----
		// 必须在 valid=true 之后创建：UploadTextureSubresource 内部调用 Ensure() 会再次进入 Init
		{
			D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
			D3D12_RESOURCE_DESC rd = {};
			rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			rd.Width = 1; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
			rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; rd.SampleDesc.Count = 1;
			if (SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
				D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&g_backend.dummyTex))))
			{
				UINT32 pixel = 0;
				UploadTextureSubresource(g_backend.dummyTex.Get(), 0, &pixel, 4, 1, 1, 1,
					DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM);
				D3D12_CPU_DESCRIPTOR_HANDLE cpu = {};
				if (g_backend.persistentSrv.AllocPersistent(1, cpu) && cpu.ptr)
				{
					g_backend.dummySrvCpu = cpu;
					D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
					sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
					sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
					sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
					sd.Texture2D.MipLevels = 1;
					dev->CreateShaderResourceView(g_backend.dummyTex.Get(), &sd, g_backend.dummySrvCpu);
			}
		}
	}

	// ---- 各维度空描述符（null SRV，persistent 堆）：FlushPipeline 为无效槽位
	// 按着色器反射出的维度选占位符。空描述符（pResource=nullptr）读取返回 0，
	// 与 D3D11 未绑定的语义一致；维度必须与着色器声明匹配，否则 GBV 报
	// "SRV resource dimensions differs from that expected by shader" 且读取为
	// 未定义行为（SSLR-only 时 s_env 期望 cube 却被填 2D 黑纹理即此类）。 ----
	{
		// vid_restart 会销毁重建设备：先清空旧句柄，防止分配失败时残留
		// 指向已销毁堆的陈旧描述符地址。
		ZeroMemory(g_backend.nullSrvCpu, sizeof(g_backend.nullSrvCpu));
		ZeroMemory(g_backend.fallbackCbvGpu, sizeof(g_backend.fallbackCbvGpu));
		ZeroMemory(g_backend.fallbackSrvGpu, sizeof(g_backend.fallbackSrvGpu));

		// vid_restart：丢弃引用旧设备资源的 pending 上传（其暂存字节一并释放）
		{
			std::lock_guard<std::mutex> g(s_upMutex);
			for (size_t i = s_upHead; i < s_uploads.size(); ++i)
				delete[] s_uploads[i].data;
			s_uploads.clear();
			s_upHead = 0;
			s_uploadPendingBytes = 0;
		}

		for (UINT dim = 1; dim <= D3D12_SRV_DIMENSION_TEXTURECUBEARRAY; ++dim)
		{
			if (dim == D3D12_SRV_DIMENSION_TEXTURE2D)
				continue;	// 2D 槽位沿用上面真实的 1x1 黑纹理（dummySrvCpu）

			D3D12_CPU_DESCRIPTOR_HANDLE cpu = {};
			if (!g_backend.persistentSrv.AllocPersistent(1, cpu) || !cpu.ptr)
				continue;

			D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
			sd.ViewDimension = (D3D12_SRV_DIMENSION)dim;
			if (dim == D3D12_SRV_DIMENSION_BUFFER)
			{
				// 结构化 buffer 的空描述符：Format 必须为 UNKNOWN 且给出非零 stride
				sd.Format = DXGI_FORMAT_UNKNOWN;
				sd.Buffer.FirstElement = 0;
				sd.Buffer.NumElements = 1;
				sd.Buffer.StructureByteStride = 16;
			}
			else
			{
				sd.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
				switch (dim)
				{
				case D3D12_SRV_DIMENSION_TEXTURE1D:		sd.Texture1D.MipLevels = 1; break;
				case D3D12_SRV_DIMENSION_TEXTURE1DARRAY:	sd.Texture1DArray.MipLevels = 1; sd.Texture1DArray.ArraySize = 1; break;
				case D3D12_SRV_DIMENSION_TEXTURE2DARRAY:	sd.Texture2DArray.MipLevels = 1; sd.Texture2DArray.ArraySize = 1; break;
				case D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY:	sd.Texture2DMSArray.ArraySize = 1; break;
				case D3D12_SRV_DIMENSION_TEXTURE3D:		sd.Texture3D.MipLevels = 1; break;
				case D3D12_SRV_DIMENSION_TEXTURECUBE:	sd.TextureCube.MipLevels = 1; break;
				case D3D12_SRV_DIMENSION_TEXTURECUBEARRAY:	sd.TextureCubeArray.MipLevels = 1; sd.TextureCubeArray.NumCubes = 1; break;
				default: break;	// TEXTURE2DMS 无字段
				}
			}
			sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			dev->CreateShaderResourceView(nullptr, &sd, cpu);
			g_backend.nullSrvCpu[dim] = cpu;
		}
	}

		// ---- 全零 64KB 常量缓冲 + 其 CBV（persistent 堆），供未绑定的 CBV 槽占位 ----
		{
			D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
			D3D12_RESOURCE_DESC bd = {};
			bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			bd.Width = 65536; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
			bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			if (SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
				D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&g_backend.dummyCb))))
			{
				void* pMapped = nullptr;
				D3D12_RANGE rr = { 0, 0 };
				if (SUCCEEDED(g_backend.dummyCb->Map(0, &rr, &pMapped)) && pMapped)
					memset(pMapped, 0, 65536);

				D3D12_CPU_DESCRIPTOR_HANDLE cpu = {};
				if (g_backend.persistentSrv.AllocPersistent(1, cpu) && cpu.ptr)
				{
					g_backend.dummyCbvCpu = cpu;
					D3D12_CONSTANT_BUFFER_VIEW_DESC cbd = {};
					cbd.BufferLocation = g_backend.dummyCb->GetGPUVirtualAddress();
					cbd.SizeInBytes = 65536;
					dev->CreateConstantBufferView(&cbd, g_backend.dummyCbvCpu);
				}
			}
		}
		return true;
	}

	void Shutdown()
	{
		g_backend.psoCache.clear();
		g_backend.rootSignature.Reset();
		g_backend.valid = false;
	}

	void BackendBeginFrame()
	{
		// 本帧用哪个 backbuffer，就用哪一份描述符堆 / 上传环段；两份交替使用，
		// 保证 CPU 录制本帧时不会覆盖 GPU 仍在读的上一帧区域。
		// （引擎侧的围栏等待保证"上次使用该 backbuffer 的那一帧"已执行完）
		++g_backend.frameSerial;
		const UINT slot = dx12::GetFrameIndex() & 1;
		g_backend.cbvSrvUav.SetSlot(slot);
		g_backend.cbvSrvUav.Reset();
		g_backend.sampler.SetSlot(slot);
		g_backend.sampler.Reset();
		g_backend.ring.SetSlot(slot);
		g_backend.ring.Reset();

		// ---- 兜底根表：堆刚 Reset 后立即分配（必然成功），本帧描述符堆耗尽导致
		// Alloc 失败时，FlushPipeline 把这些句柄绑给该 Draw——保证 4 张根表永远有
		// 合法句柄。若帧首首个 Draw 以零句柄执行，GPU 读描述符会页错误
		//（DRED 实测 VA=0x0）→ 队列停摆 → TDR。 ----
		{
			D3D12_CPU_DESCRIPTOR_HANDLE c = {};
			D3D12_GPU_DESCRIPTOR_HANDLE g = {};
			const UINT ds = g_backend.cbvSrvUav.DescriptorSize();
			if (g_backend.cbvSrvUav.Alloc(14, c, g))
			{
				g_backend.fallbackCbvGpu[slot] = g;
				if (g_backend.dummyCbvCpu.ptr)
					for (UINT i = 0; i < 14; ++i)
					{
						D3D12_CPU_DESCRIPTOR_HANDLE d = c;
						d.ptr += (SIZE_T)i * ds;
						dx12::GetD3D12Device()->CopyDescriptorsSimple(1, d, g_backend.dummyCbvCpu, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
					}
			}
			if (g_backend.cbvSrvUav.Alloc(16, c, g))
			{
				g_backend.fallbackSrvGpu[slot] = g;
				if (g_backend.dummySrvCpu.ptr)
					for (UINT i = 0; i < 16; ++i)
					{
						D3D12_CPU_DESCRIPTOR_HANDLE d = c;
						d.ptr += (SIZE_T)i * ds;
						dx12::GetD3D12Device()->CopyDescriptorsSimple(1, d, g_backend.dummySrvCpu, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
					}
			}
		}
		g_backend.frameActive = true;
	}

	UINT FrameSerial()
	{
		return g_backend.frameSerial;
	}

	std::mutex& ConstantBufferMutex()
	{
		static std::mutex s_cbMutex;
		return s_cbMutex;
	}

	std::recursive_mutex& LifecycleMutex()
	{
		static std::recursive_mutex s_lifeMutex;
		return s_lifeMutex;
	}

	//--------------------------------------------------------------------------
	// 缓冲存活登记 + 上传环区间判定（绑定路径的陈旧绑定防护，详见 dx12Backend.h）
	//--------------------------------------------------------------------------
	static std::mutex					s_liveBufMutex;
	static std::unordered_set<const void*>	s_liveBufs;

	void RegisterLiveBuffer(const void* buf)
	{
		if (!buf) return;
		std::lock_guard<std::mutex> g(s_liveBufMutex);
		s_liveBufs.insert(buf);
	}

	void UnregisterLiveBuffer(const void* buf)
	{
		if (!buf) return;
		std::lock_guard<std::mutex> g(s_liveBufMutex);
		s_liveBufs.erase(buf);
	}

	bool IsLiveBuffer(const void* buf)
	{
		if (!buf) return false;
		std::lock_guard<std::mutex> g(s_liveBufMutex);
		return s_liveBufs.find(buf) != s_liveBufs.end();
	}

	bool UploadRingContains(UINT64 va)
	{
		return g_backend.ring.Contains(va);
	}

	void BackendEndFrame()
	{
		// 容量体检：接近上限时提前暴露。描述符堆 Alloc 失败会让 Root 表保持上一次的
		// 残留描述符（花屏）；上传环越段则退化为垃圾数据/失败回退。
		// 只在用量偏高时输出，正常帧不产生日志。
		const UINT descUsed = g_backend.cbvSrvUav.Used();
		const UINT descCap = g_backend.cbvSrvUav.Capacity();
		if (descCap && (UINT64)descUsed * 10 >= (UINT64)descCap * 8)
			Msg("! DX12: [cap] descriptor heap usage %u / %u", descUsed, descCap);

		const UINT64 ringUsed = g_backend.ring.SlotUsed();
		if (ringUsed >= (96ull << 20))
			Msg("! DX12: [cap] upload ring segment usage %llu MB", ringUsed >> 20);

		g_backend.frameActive = false;
	}

	ID3D12RootSignature* GetRootSignature()
	{
		return g_backend.rootSignature.Get();
	}
}
