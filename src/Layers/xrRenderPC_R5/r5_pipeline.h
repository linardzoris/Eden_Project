#pragma once

// M1: DX12 渲染管线基础设施
// - 着色器编译 (D3DCompileFromFile)
// - 根签名 + PSO 创建
// - 每帧 Begin/End 封装

#include <d3d12.h>

class R5Visual; // fwd
struct R5LevelWorld; // fwd

namespace r5_pipeline
{
	bool Init();
	void Shutdown();

	// 每帧渲染入口：BeginFrame -> 各 pass -> EndFrame
	void BeginFrame();
	void EndFrame();

	// M2: 全屏三角形 pass
	void DrawFullscreenTriangle();

	// M3c: MVP 立方体 pass（M4a: MRT 输出 G-buffer albedo+normal）
	void DrawCube(float timeSec);

	// M7: 场景级 G-buffer 管理（清空/屏障只做一次，避免多视觉互相清掉）
	void BeginScene();	// 清 G-buffer + 绑定管线
	void EndScene();	// G-buffer RT -> PS SRV

	// M5: 绘制一个静态视觉（复用 cube 根签名/PSO，绑定其 VB/IB）
	void DrawVisual(const R5Visual& v, float timeSec);

	// M8: 绘制静态世界（真实相机矩阵 + 世界几何）
	void DrawLevelWorld(const R5LevelWorld& world, float timeSec);

	// M4a: G-buffer 合成 pass（多 SRV 采样，Lambert 光照到 backbuffer）
	void DrawCompose();

	// M4b: DXR 数据暴露
	D3D12_GPU_VIRTUAL_ADDRESS GetCubeVB();
	D3D12_GPU_VIRTUAL_ADDRESS GetCubeIB32();
	ID3D12Resource* GetGBufferRT(int i);
	D3D12_GPU_DESCRIPTOR_HANDLE GetGBufferUAV();
	D3D12_CPU_DESCRIPTOR_HANDLE GetGBufferRTV(int i);
}
