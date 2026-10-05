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

// DX12 后端：共享层通过 wrapper 访问设备/上下文（D3D11 形态 API → D3D12 翻译）。
// wrapper 类型与全局实例见 xrRenderDX12/dx12Types.h。
class dx12Device;
class dx12Context;
extern dx12Device	DX12Device;
extern dx12Context	DX12Context;

struct IDXGISwapChain3;

#define RDevice (&DX12Device)
#define RContext (&DX12Context)
#define RSwapchain ((IDXGISwapChain3*)Device.GetSwapchain())

// DX12 的渲染目标在 R5 的 CRenderTarget 中管理；M0/阶段0 先置空，
// 阶段1 起由 r5_rendertarget 提供真实 wrapper。
#define RSwapchainTarget ((ID3DRenderTargetView*)nullptr)
#define RTarget ((ID3DRenderTargetView*)nullptr)
// 主深度：由 CRenderTarget::create() 在创建 rt_Position(R24G8/D24) 后，
// 把其 DSV 写入引擎的 RenderDSV，这里统一经 Device.GetDepthTexture() 取用。
#define RDepth ((ID3DDepthStencilView*)Device.GetDepthTexture())

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
