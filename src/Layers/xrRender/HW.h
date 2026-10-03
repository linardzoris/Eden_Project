// HW.h: interface for the CHW class.
//
//////////////////////////////////////////////////////////////////////
#pragma once

#include "HWCaps.h"

#ifndef _EDITOR
#	include <renderdoc/api/app/renderdoc_app.h>
#endif

#ifndef _MAYA_EXPORT
#include "stats_manager.h"
#endif

struct SDL_Window;

#ifdef USE_DX12

// DX12 设备全局指针由引擎侧 HWRenderDevice/HWRenderContext/HWSwapchain 暴露为 void*。
// 这里直接转回 DX12 接口类型。RTarget/RDepth 在 DX12 中不再是简单视图指针，
// M0 骨架阶段仅保证类型可用；后续里程碑引入描述符堆后再完善。
struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct IDXGISwapChain3;

#define RDevice ((ID3D12Device*)Device.GetRenderDevice())
#define RContext ((ID3D12GraphicsCommandList*)Device.GetRenderContext())
#define RSwapchain ((IDXGISwapChain3*)Device.GetSwapchain())

// DX12 中 swapchain backbuffer 的 RTV 是 D3D12_CPU_DESCRIPTOR_HANDLE，
// M0 暂时沿用 void* 透传，R5 内部自行管理描述符堆。
#define RSwapchainTarget (Device.GetSwapchainTexture())
#define RTarget (Device.GetRenderTexture())
#define RDepth (Device.GetDepthTexture())

#elif defined(USE_DX11)
#define RContext ((ID3D11DeviceContext*)Device.GetRenderContext())
#define RDevice ((ID3D11Device*)Device.GetRenderDevice())
#define RSwapchainTarget ((ID3D11RenderTargetView*)Device.GetSwapchainTexture())
#define RTarget ((ID3D11RenderTargetView*)Device.GetRenderTexture())
#define RDepth ((ID3D11DepthStencilView*)Device.GetDepthTexture())
#define RSwapchain ((IDXGISwapChain*)Device.GetSwapchain())
#else
#define RContext ((IDirect3DDevice9*)Device.GetRenderContext())
#define RDevice ((IDirect3DDevice9*)Device.GetRenderDevice())
#define RSwapchainTarget ((IDirect3DSurface9*)Device.GetSwapchainTexture())
#define RTarget ((IDirect3DSurface9*)Device.GetRenderTexture())
#define RDepth ((IDirect3DSurface9*)Device.GetDepthTexture())
#define RSwapchain ((IDirect3DDevice9*)Device.GetSwapchain())
#endif

#define RFeatureLevel Device.GetFeatureLevel()
