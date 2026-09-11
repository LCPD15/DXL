// CUDA runtime API 最小 stub（#72）。
//
// TensorRT 头文件硬依赖 <cuda_runtime_api.h>（NvInferRuntimeBase.h:24），
// 本机没有 CUDA Toolkit —— 真正的 CUDA 调用我们走 nvcuda.dll 驱动 API
// （GetProcAddress，见 SegMaskFilter.cpp），不需要 runtime 库。
//
// 这里只放**声明**（extern，无定义）+ 类型/枚举：TRT 头里引用这些符号的
// 内联函数体（plugin 路径）不会被编译进我们的目标文件——只要我们不实例化
// 那些 plugin 类，链接期就没有未解析符号。纯虚 Impl 的调用全部落进
// nvinfer_11.dll。我们真正用到的 enqueueV3(cudaStream_t) 只需要
// cudaStream_t 这个指针类型。
#pragma once

#include <cstddef>
#include <cstdint>

// ---- 类型 ----
struct CUstream_st;
using cudaStream_t = CUstream_st*;
using CUstream = cudaStream_t;

// cudaEvent_t：TRT 头里 getInputConsumedEvent/setInputConsumedEvent 用到
//（NvInferImpl.h:430）。同样只作不透明句柄传递。
struct CUevent_st;
using cudaEvent_t = CUevent_st*;

// auto-reset 语义无关紧要：TRT 只把它当不透明句柄传下去
enum cudaError {
	cudaSuccess = 0,
	cudaErrorMemoryAllocation = 2,
};

// TRT plugin 内联体里引用的设备属性（值照抄 runtime，防比较逻辑错位）
enum cudaDeviceAttr {
	cudaDevAttrMemoryPoolsSupported = 108,
};

enum cudaMemcpyKind {
	cudaMemcpyHostToHost = 0,
	cudaMemcpyHostToDevice = 1,
	cudaMemcpyDeviceToHost = 2,
	cudaMemcpyDeviceToDevice = 3,
};

// 不透明：只有指针传递，没有字段访问（TRT 头不会解引用它）
struct cudaDeviceProp {
	char _opaque[1024];
};

enum cudaMemoryType { cudaMemoryTypeHost = 0, cudaMemoryTypeDevice = 1 };
struct cudaPointerAttributes {
	int device;
	cudaMemoryType memoryType;
	void* devicePointer;
	void* hostPointer;
};

// ---- 声明（无定义：未引用不链接） ----
extern "C" {
cudaError cudaMalloc(void** devPtr, size_t size);
cudaError cudaFree(void* devPtr);
cudaError cudaMallocAsync(void** devPtr, size_t size, cudaStream_t stream);
cudaError cudaMemcpyAsync(void* dst, const void* src, size_t count,
	cudaMemcpyKind kind, cudaStream_t stream);
cudaError cudaDeviceSynchronize();
cudaError cudaGetDeviceProperties(cudaDeviceProp* prop, int device);
cudaError cudaPointerGetAttributes(cudaPointerAttributes* attributes,
	const void* ptr);
}
