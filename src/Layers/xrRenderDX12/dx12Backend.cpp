#include "stdafx.h"
#include <d3d12sdklayers.h>
#include <dbghelp.h>
#include "DX12CommonTypes.h"
#include "dx12Backend.h"

#pragma comment(lib, "dbghelp.lib")

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

	void Ensure()
	{
		if (g_backend.valid) return;
		Init(GetD3D12Device());
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
			Msg("! DX12 [%s]: device removed reason 0x%08x", tag ? tag : "", (unsigned)drr);

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
		g_backend.uploadQueue->ExecuteCommandLists(1, lists);
		g_backend.uploadFenceValue++;
		g_backend.uploadQueue->Signal(g_backend.uploadFence.Get(), g_backend.uploadFenceValue);
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
		UINT width, UINT height, UINT depth, DXGI_FORMAT fmt, DXGI_FORMAT footprintFormat)
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

		// 目标资源当前为 ALL_SHADER_RESOURCE → COPY_DEST
		D3D12_RESOURCE_BARRIER b = {};
		b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		b.Transition.pResource = dst;
		b.Transition.StateBefore = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
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
		b.Transition.StateAfter = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
		list->ResourceBarrier(1, &b);
		list->Close();

		ID3D12CommandList* lists[] = { list };
		g_backend.uploadQueue->ExecuteCommandLists(1, lists);
		DumpDeviceErrors("upload");
		g_backend.uploadFenceValue++;
		g_backend.uploadQueue->Signal(g_backend.uploadFence.Get(), g_backend.uploadFenceValue);
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
		g_backend.uploadQueue->ExecuteCommandLists(1, lists);
		g_backend.uploadFenceValue++;
		g_backend.uploadQueue->Signal(g_backend.uploadFence.Get(), g_backend.uploadFenceValue);
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
	bool DescriptorHeap::Create(ID3D12Device* dev, D3D12_DESCRIPTOR_HEAP_TYPE type, UINT capacity, bool shaderVisible)
	{
		m_size = dev->GetDescriptorHandleIncrementSize(type);
		m_capacity = capacity;
		m_offset = 0;

		D3D12_DESCRIPTOR_HEAP_DESC desc = {};
		desc.Type = type;
		desc.NumDescriptors = capacity;
		desc.Flags = shaderVisible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
		desc.NodeMask = 0;

		if (FAILED(dev->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_heap))))
		{
			Msg("! DX12: CreateDescriptorHeap failed (type=%d, count=%u)", (int)type, capacity);
			return false;
		}

		m_cpuStart = m_heap->GetCPUDescriptorHandleForHeapStart();
		if (shaderVisible)
			m_gpuStart = m_heap->GetGPUDescriptorHandleForHeapStart();
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
		if (m_persistOffset + count > m_capacity)
		{
			Msg("! DX12: persistent descriptor heap overflow (%u + %u > %u)", m_persistOffset, count, m_capacity);
			return false;
		}
		cpu = CPU(m_persistOffset);
		m_persistOffset += count;
		return true;
	}

	D3D12_CPU_DESCRIPTOR_HANDLE DescriptorHeap::CPU(UINT index) const	{
		D3D12_CPU_DESCRIPTOR_HANDLE h = m_cpuStart;
		h.ptr += (SIZE_T)index * m_size;
		return h;
	}

	D3D12_GPU_DESCRIPTOR_HANDLE DescriptorHeap::GPU(UINT index) const
	{
		D3D12_GPU_DESCRIPTOR_HANDLE h = m_gpuStart;
		h.ptr += (UINT64)index * m_size;
		return h;
	}

	//--------------------------------------------------------------------------
	// UploadRing
	//--------------------------------------------------------------------------
	bool UploadRing::Create(ID3D12Device* dev, UINT64 size)
	{
		m_size = size;

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
		return true;
	}

	void* UploadRing::Alloc(UINT64 size, UINT64 align, D3D12_GPU_VIRTUAL_ADDRESS& gpuAddr)
	{
		Ensure();
		UINT64 offset = (m_offset + (align - 1)) & ~(align - 1);
		if (offset + size > m_size)
		{
			Msg("! DX12: upload ring overflow (%llu + %llu > %llu)", offset, size, m_size);
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

		// shader-visible CBV/SRV/UAV 堆：每个 draw 需固定 14 CBV + 16 SRV 表（根签名范围固定），
		// 4096 在单帧 ~136 draw 时即溢出；按最坏场景 ~6600 draw/帧 扩到 200000（约 6MB），每帧重置。
		if (!g_backend.cbvSrvUav.Create(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 200000, true)) return false;
		if (!g_backend.sampler.Create(dev, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 256, true)) return false;
		if (!g_backend.ring.Create(dev, 256ull * 1024 * 1024)) return false;

		if (!g_backend.persistentSrv.Create(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 8192, false)) return false;
		if (!g_backend.persistentUav.Create(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 512, false)) return false;
		if (!g_backend.persistentRtv.Create(dev, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 512, false)) return false;
		if (!g_backend.persistentDsv.Create(dev, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 512, false)) return false;

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
		D3D12_ROOT_PARAMETER params[2] = {};
		// [0] CBV 描述符表：b0..b13
		D3D12_DESCRIPTOR_RANGE cbvRange = {};
		cbvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
		cbvRange.NumDescriptors = kNumCBVSlots;
		cbvRange.BaseShaderRegister = 0;
		cbvRange.RegisterSpace = 0;
		cbvRange.OffsetInDescriptorsFromTableStart = 0;
		params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		params[0].DescriptorTable.NumDescriptorRanges = 1;
		params[0].DescriptorTable.pDescriptorRanges = &cbvRange;
		params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		// [1] SRV 描述符表：t0..t15
		D3D12_DESCRIPTOR_RANGE srvRange = {};
		srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
		srvRange.NumDescriptors = kNumSRVSlots;
		srvRange.BaseShaderRegister = 0;
		srvRange.RegisterSpace = 0;
		srvRange.OffsetInDescriptorsFromTableStart = 0;
		params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		params[1].DescriptorTable.NumDescriptorRanges = 1;
		params[1].DescriptorTable.pDescriptorRanges = &srvRange;
		params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

		xr_vector<D3D12_STATIC_SAMPLER_DESC> samplers;
		samplers.reserve(kNumSRVSlots);
		for (UINT i = 0; i < kNumSRVSlots; ++i)
			samplers.push_back(GetStaticSampler(i));

		D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
		rsDesc.NumParameters = 2;
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
		g_backend.cbvSrvUav.Reset();
		g_backend.sampler.Reset();
		g_backend.ring.Reset();
		g_backend.frameActive = true;
	}

	void BackendEndFrame()
	{
		g_backend.frameActive = false;
	}

	ID3D12RootSignature* GetRootSignature()
	{
		return g_backend.rootSignature.Get();
	}
}
