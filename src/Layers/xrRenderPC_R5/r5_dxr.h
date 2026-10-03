#pragma once

// M4b: DXR 光追立方体
// - BLAS/TLAS（首帧在命令列表内构建）
// - lib_6_3 光线生成/miss/closest-hit（gamedata raytrace_cube.lib.hlsl）
// - DispatchRays 输出到 G-buffer RT0（UAV），合成 pass 直通显示
// - 设备不支持 DXR 时 Ready()=false，渲染回退光栅路径

namespace r5_dxr
{
	bool Init();
	void Shutdown();
	bool Ready();

	// 每帧：首帧建 AS + 清 normal RT；随后 DispatchRays 写 G-buffer RT0
	void Render(float timeSec);
}
