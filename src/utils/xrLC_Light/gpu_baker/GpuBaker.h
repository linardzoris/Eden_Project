#pragma once

// GPU ray-tracing baker module (CUDA/OptiX).
// Currently hosts a toolchain probe only; the OptiX path (acceleration structure
// build + anyhit alpha-energy tracing) plugs into this folder when the OptiX SDK
// is available.

// Returns the number of usable CUDA devices (0 = none available). On success
// fills name_out with the primary device name and multiprocessors with its SM count.
int gpu_baker_probe(char* name_out, int name_size, int* multiprocessors);
