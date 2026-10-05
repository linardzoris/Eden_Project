#pragma once

#include "../../xrEngine/stdafx.h"
#include "../../xrCore/API/xrAPI.h"
#include "../../xrEngine/Render.h"
#include "../../Include/xrRender/RenderDeviceRender.h"
#include "../../Include/xrRender/RenderFactory.h"
#include "../../Include/xrRender/UIRender.h"
#include "../../Include/xrRender/DrawUtils.h"
#include "../../Include/xrRender/UIShader.h"
#include "../../Include/xrRender/UISequenceVideoItem.h"
#include "../../Include/xrRender/WallMarkArray.h"
#include "../../Include/xrRender/StatsRender.h"
#include "../../Include/xrRender/StatGraphRender.h"
#include "../../Include/xrRender/FontRender.h"
#include "../../Include/xrRender/EnvironmentRender.h"
#include "../../Include/xrRender/RainRender.h"
#include "../../Include/xrRender/LensFlareRender.h"
#include "../../Include/xrRender/ThunderboltRender.h"
#include "../../Include/xrRender/ThunderboltDescRender.h"
#ifdef DEBUG
#include "../../Include/xrRender/ObjectSpaceRender.h"
#endif
#ifdef DEBUG_DRAW
#include "../../Include/xrRender/DebugRender.h"
#endif

#define		R_R1	1
#define		R_R2	2
#define		R_R4	4
#define		R_R5	5
// R5 复用 R4 渲染管线（deferred MRT / R2 路径），故 RENDER 取 R_R4，
// 使共享层与 R4 源码中的 (RENDER==R_R2)||(RENDER==R_R4) 分支全部生效。
#define		RENDER	R_R4

#include "../../xrEngine/vis_common.h"
#include "../../xrEngine/IGame_Level.h"
#include "../../xrEngine/IGame_Persistent.h"

// ---- 共享层接入（对齐 xrRenderPC_R4/stdafx.h 的包含顺序）----
#include "../xrRenderDX10/DxgiFormat.h"
#include "../xrRenderDX12/DX12CommonTypes.h"
#include "../xrRenderDX12/dx12Types.h"
#include "../xrRenderDX12/dx12Backend.h"

#include "../../xrParticles/psystem.h"

#include "../xrRender/HW.h"
#include "../xrRender/Shader.h"
#include "../xrRender/R_Backend.h"
#include "../xrRender/R_Backend_Runtime.h"
#include "../xrRender/ResourceManager.h"

#include "../../xrEngine/_d3d_extensions.h"
#include "../xrRender/blenders/Blender.h"
#include "../xrRender/blenders/Blender_CLSID.h"
#include "../xrRender/xrRender_console.h"

// ---- R4 渲染器外壳（DX12 编译）----
#include "../xrRender/Debug/dxGPUEventWrapper.h"
#include "../xrRenderPC_R4/r4.h"

IC	void jitter(CBlender_Compile& C) {
	C.r_dx10Texture("jitter0", JITTER(0));
	C.r_dx10Texture("jitter1", JITTER(1));
	C.r_dx10Texture("jitter2", JITTER(2));

	C.r_dx10Sampler("smp_jitter");
}

// DX12 设备全局指针（引擎侧定义）
extern void* HWRenderDevice;
extern void* HWRenderContext;
extern void* HWSwapchain;
extern void* RenderTexture;
extern void* RenderSRV;
extern void* RenderRTV;
extern void* RenderDSV;
extern void* SwapChainRTV;

// DX12 帧管理 + 资源访问（引擎侧 Device_create_render_dx12.cpp 中定义）
// extern "C" 避免 C++ 名称修饰，ENGINE_API 在 R5 中展开为 dllimport
#include <d3d12.h>

namespace dx12
{
	extern "C" ENGINE_API void BeginFrame();
	extern "C" ENGINE_API void EndFrame();
	extern "C" ENGINE_API void FramePresent(bool vsync);
	extern "C" ENGINE_API ID3D12GraphicsCommandList* GetCmdList();
	extern "C" ENGINE_API D3D12_CPU_DESCRIPTOR_HANDLE GetCurrentRTV();
	extern "C" ENGINE_API D3D12_CPU_DESCRIPTOR_HANDLE GetCurrentDSV();
	extern "C" ENGINE_API void SetRenderDSV(void* dsv);
}
