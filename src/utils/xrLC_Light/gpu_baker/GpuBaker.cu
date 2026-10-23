// GPU baker toolchain probe: validates nvcc -> sm_89 compilation and the CUDA
// runtime end-to-end (device query + one trivial kernel launch).
#include "GpuBaker.h"

#include <cuda_runtime.h>
#include <cstdio>

// Trivial kernel: proves the device compilation path works
__global__ void gpu_baker_fill_kernel(float* dst, float value, int count)
{
	int index = blockIdx.x * blockDim.x + threadIdx.x;
	if (index < count)
		dst[index] = value;
}

int gpu_baker_probe(char* name_out, int name_size, int* multiprocessors)
{
	int device_count = 0;
	if (cudaGetDeviceCount(&device_count) != cudaSuccess || device_count == 0)
		return 0;

	cudaDeviceProp prop;
	if (cudaGetDeviceProperties(&prop, 0) != cudaSuccess)
		return 0;

	if (name_out && name_size > 0)
	{
		std::snprintf(name_out, name_size, "%s", prop.name);
	}
	if (multiprocessors)
		*multiprocessors = prop.multiProcessorCount;

	// One real launch to verify the runtime and the kernel binary on this GPU
	const int count = 256;
	float* scratch = nullptr;
	if (cudaMalloc(&scratch, count * sizeof(float)) != cudaSuccess)
		return 0;
	gpu_baker_fill_kernel<<<1, count>>>(scratch, 1.0f, count);
	const cudaError_t launch = cudaDeviceSynchronize();
	cudaFree(scratch);
	if (launch != cudaSuccess)
		return 0;

	return device_count;
}
