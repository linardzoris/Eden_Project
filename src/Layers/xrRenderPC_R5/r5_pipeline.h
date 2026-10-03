#pragma once

// M1: DX12 渲染管线基础设施
// - 着色器编译 (D3DCompileFromFile)
// - 根签名 + PSO 创建
// - 每帧 Begin/End 封装

namespace r5_pipeline
{
	bool Init();
	void Shutdown();

	// 每帧渲染入口：BeginFrame -> 各 pass -> EndFrame
	void BeginFrame();
	void EndFrame();

	// M2: 全屏三角形 pass
	void DrawFullscreenTriangle();

	// M3c: MVP 立方体 pass
	void DrawCube(float timeSec);
}
