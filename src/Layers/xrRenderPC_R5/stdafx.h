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
#define		RENDER	R_R5

#include "../../xrEngine/vis_common.h"
#include "../../xrEngine/IGame_Level.h"
#include "../../xrEngine/IGame_Persistent.h"

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
}
