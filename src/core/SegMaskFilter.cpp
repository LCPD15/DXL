// DXL semantic segmentation. Capture is recorded on the NR command list and
// consumed only after that exact list's real queue submission fence completes.
// One capture owns allocator/readback/metadata until inference finishes. RGBA
// composition is separate (SemanticMask.h); publication copies coherent snapshots.
#include "SegMaskFilter.h"
#include "SegmentationRuntime.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <share.h>

#include "../common/Log.h"

// TRT 头：需要 cuda_runtime_api.h —— third_party/cuda-stub（真调用走
// nvcuda.dll 驱动 API，不链 runtime 库）。
#include "NvInfer.h"
#include "NvInferRuntime.h"

namespace DXL {

namespace {

constexpr uint32_t INPUT_WH = 640;
constexpr uint32_t PROTO_WH = 160;    // stride-4（#71）
constexpr float CONF_TH = 0.35f;
constexpr float IOU_TH = 0.45f;
constexpr int32_t TRT_RT_VERSION = NV_TENSORRT_VERSION;

// ---- #79 语义分组：COCO 80 类 → 12 整合组（UI 每组一个开关+强度）----
// 组序 = UI 网格序。全部 80 类都有着落（无 255 漏项）：
//   0 人物 | 1 车辆 | 2 动物 | 3 街具 | 4 运动 | 5 食物 | 6 餐具
//   7 家具 | 8 电器 | 9 卫浴 | 10 配饰 | 11 杂物
// SEM_GROUP_COUNT 在 SegMaskFilter.h（#81 起 18 = COCO 12 + ADE 场景 6）。
constexpr uint8_t kClassGroup[80] = {
	0,                                                    // person
	1, 1, 1, 1,                                           // bicycle car motorcycle airplane
	1, 1, 1, 1,                                           // bus train truck boat
	3, 3, 3, 3, 3,                                        // traffic light fire hydrant stop sign parking meter bench
	2, 2, 2, 2, 2, 2, 2, 2, 2, 2,                        // bird cat dog horse sheep cow elephant bear zebra giraffe
	10, 10, 10, 10, 10,                                   // backpack umbrella handbag tie suitcase
	4, 4, 4, 4, 4, 4, 4, 4, 4, 4,                        // frisbee skis snowboard sports ball kite baseball bat glove skateboard surfboard tennis racket
	6, 6, 6, 6, 6, 6,                                     // bottle wine glass cup fork knife spoon
	6,                                                    // bowl
	5, 5, 5, 5, 5, 5, 5, 5, 5, 5,                        // banana apple sandwich orange broccoli carrot hot dog pizza donut cake
	7, 7, 11,                                             // chair couch potted plant
	7,                                                    // bed
	7,                                                    // dining table
	9,                                                    // toilet
	8, 8, 8,                                              // tv laptop mouse
	8,                                                    // remote
	8, 8,                                                 // keyboard cell phone
	8, 8, 8,                                              // microwave oven toaster
	9, 8,                                                 // sink refrigerator（冰箱归电器：厨房家电）
	11, 11, 11,                                           // book clock vase
	11, 11, 11, 11,                                       // scissors teddy bear hair drier toothbrush
};
static_assert(sizeof(kClassGroup) == 80, "80 类全覆盖");
static_assert(kClassGroup[58] == 11 && kClassGroup[59] == 7, "COCO potted plant and bed order");

// ---- ADE20K 150 类 → 场景组（#81，表由 gen_ade_group_table.py 生成）----
// 只用场景组 12..17（12 建筑/13 植被/14 天空/15 水域/16 地形/17 其他场景）：
// 映射到 0..11 的类（person/animal/car/家具/电器…）在表里也标着 COCO 同概念
// 组号 —— 但**解码时跳过**（COCO 实例模型已认领这些像素，前景优先，
// ADE 只补 COCO 给不了的背景语义）。表留全映射：语义清楚 + 换模型时直接改。
// 序号 = yolo26s-sem 输出类图的 train ID（ade20k.yaml 顺序）。
constexpr uint8_t kAdeGroup[150] = {
	12, 12, 14, 17, 13, 17, 16,  7, 12, 13,   // wall, building, sky, floor, tree, ceiling, road, bed, windowpane, grass
	 7, 16,  0, 16, 12,  7, 16, 13, 17,  7,   // cabinet, sidewalk, person, earth, door, table, mountain, plant, curtain, chair
	 1, 15, 11,  7,  7, 12, 15,  7, 17, 13,   // car, water, painting, sofa, shelf, house, sea, mirror, rug, field
	 7,  7, 12,  7, 16,  7,  8,  9, 12, 17,   // armchair, seat, fence, desk, rock, wardrobe, lamp, bathtub, railing, cushion
	17, 11, 12,  3,  7,  7, 16,  9, 12, 12,   // base, box, column, signboard, chest of drawers, counter, sand, sink, skyscraper, fireplace
	 8, 12, 16, 17, 16, 11,  7, 17, 12, 17,   // refrigerator, grandstand, path, stairs, runway, case, pool table, pillow, screen door, stairway
	15, 12,  7, 17,  7,  9, 13, 11, 16,  3,   // river, bridge, bookcase, blind, coffee table, toilet, flower, book, hill, bench
	 7,  8, 13,  7,  8,  7,  1,  7,  8, 12,   // countertop, stove, palm, kitchen island, computer, swivel chair, boat, bar, arcade machine, hovel
	 1, 17,  8,  1, 12,  8, 12,  3, 12,  8,   // bus, towel, light, truck, tower, chandelier, awning, streetlight, booth, television receiver
	 1, 16, 10,  3, 16, 12, 12,  7,  6,  7,   // airplane, dirt track, apparel, pole, land, bannister, escalator, ottoman, bottle, buffet
	11, 12,  1,  1, 15, 17, 12,  8, 11, 15,   // poster, stage, van, ship, fountain, conveyor belt, canopy, washer, plaything, swimming pool
	 7, 11, 11, 15, 12, 10,  1,  7,  8,  4,   // stool, barrel, basket, waterfall, tent, bag, minibike, cradle, oven, ball
	 5, 17, 17, 11,  8,  6,  2,  1, 15,  8,   // food, step, tank, trade name, microwave, pot, animal, bicycle, lake, dishwasher
	17, 17, 11, 17,  8, 11,  3,  6, 11,  8,   // screen, blanket, sculpture, hood, sconce, vase, traffic light, tray, ashcan, fan
	12,  8,  6,  8, 11,  9,  8,  6, 11, 11,   // pier, crt screen, plate, monitor, bulletin board, shower, radiator, glass, clock, flag
};
static_assert(sizeof(kAdeGroup) == 150, "ADE20K 150 类全覆盖");

// ---- 驱动 API（nvcuda.dll，System32 自带；#70 部署形态）----
using CUresult = int;
// CUDA 调用失败不能静默 return false：装载期（_trtReady 还没置位，含暖场）
// 静默的话 WorkerLoop 每 2s 把 EnsureTrt 重跑一遍 —— 每遍重新反序列化引擎 +
// cuMemAlloc，旧的全部没释放，越漏越慢（实测就是这么定位到的）。
// 装载期失败 → Fail()（一次性放弃，之后重试只是查个 atomic 就返回）；
// 就绪后的帧路径失败 → 只丢这一帧（下一帧再来）。
#define CU_CALL(result) do { \
	const CUresult _r = (result); \
	if (_r != 0) { \
		const bool setup = !_trtReady.load(std::memory_order_acquire); \
		D5_LOG_WARN(L"语义蒙版：CUDA 调用失败 code=%d（%hs）—— %s", int(_r), #result, \
			setup ? L"装载失败，语义蒙版关闭（见上文）" : L"这一帧放弃"); \
		if (setup) Fail(L"CUDA 调用失败（错误码见日志）"); \
		return false; \
	} \
} while (0)

using PFN_cuInit = CUresult(*)(unsigned);
using PFN_cuDeviceGet = CUresult(*)(int*, int);
using PFN_cuDeviceGetCount = CUresult(*)(int*);
using PFN_cuDeviceGetLuid = CUresult(*)(char*, unsigned*, int);
using PFN_cuDevicePrimaryCtxRetain = CUresult(*)(void**, int);
using PFN_cuDevicePrimaryCtxRelease = CUresult(*)(int);
using PFN_cuCtxSetCurrent = CUresult(*)(void*);
using PFN_cuStreamCreate = CUresult(*)(void**, unsigned);
using PFN_cuStreamDestroy = CUresult(*)(void*);
using PFN_cuMemAlloc = CUresult(*)(void**, size_t);
using PFN_cuMemFree = CUresult(*)(void*);
using PFN_cuMemcpyHtoDAsync = CUresult(*)(void*, const void*, size_t, void*);
using PFN_cuMemcpyDtoHAsync = CUresult(*)(void*, void*, size_t, void*);
using PFN_cuStreamSynchronize = CUresult(*)(void*);
using PFN_cuCtxGetCurrent = CUresult(*)(void**);

// 文件级单例（三个函数各放一个 function-local static 是三份独立拷贝 ——
// ReleaseTrt 那份的函数表永远是空的，显存就漏了。就一份，谁都用它）。
struct DriverApi {
	PFN_cuInit cuInit = nullptr;
	PFN_cuDeviceGet cuDeviceGet = nullptr;
	PFN_cuDeviceGetCount cuDeviceGetCount = nullptr;
	PFN_cuDeviceGetLuid cuDeviceGetLuid = nullptr;
	PFN_cuDevicePrimaryCtxRetain cuDevicePrimaryCtxRetain = nullptr;
	PFN_cuDevicePrimaryCtxRelease cuDevicePrimaryCtxRelease = nullptr;
	PFN_cuCtxSetCurrent cuCtxSetCurrent = nullptr;
	PFN_cuStreamCreate cuStreamCreate = nullptr;
	PFN_cuStreamDestroy cuStreamDestroy = nullptr;
	PFN_cuMemAlloc cuMemAlloc = nullptr;
	PFN_cuMemFree cuMemFree = nullptr;
	PFN_cuMemcpyHtoDAsync cuMemcpyHtoDAsync = nullptr;
	PFN_cuMemcpyDtoHAsync cuMemcpyDtoHAsync = nullptr;
	PFN_cuStreamSynchronize cuStreamSynchronize = nullptr;
	PFN_cuCtxGetCurrent cuCtxGetCurrent = nullptr;

	bool Load() noexcept {
		HMODULE dll = GetModuleHandleW(L"nvcuda.dll");
		if (!dll) dll = LoadLibraryW(L"nvcuda.dll");
		if (!dll) return false;
		#define GET(n) n = reinterpret_cast<PFN_##n>(GetProcAddress(dll, #n)); \
			if (!n) return false
		// _v2：CUDA 4.0 改签名那批符号（内存/流销毁/主上下文释放）。
		// 裸名是给史前 ABI 留的 v1 入口 —— CUDA 12 驱动上实测直接报 201
		// INVALID_CONTEXT（56456/56228 两局：cuMemAlloc 裸名死，cuStreamCreate
		// 这些没有 v2 之分的裸名全活）。cuda.h 把这批 #define 到 _v2 ——
		// TRT / cuda-python 实际调的本来就是 _v2，裸名只有我们在用。
		#define GET_V2(n) n = reinterpret_cast<PFN_##n>(GetProcAddress(dll, #n "_v2")); \
			if (!n) return false
		GET(cuInit);
		GET(cuDeviceGet);
		GET(cuDeviceGetCount);
		GET(cuDeviceGetLuid);
		GET(cuDevicePrimaryCtxRetain);
		GET(cuCtxSetCurrent);
		GET(cuStreamCreate);
		GET(cuStreamSynchronize);
		GET(cuCtxGetCurrent);
		GET_V2(cuMemAlloc);
		GET_V2(cuMemFree);
		GET_V2(cuMemcpyHtoDAsync);
		GET_V2(cuMemcpyDtoHAsync);
		GET_V2(cuStreamDestroy);
		GET_V2(cuDevicePrimaryCtxRelease);
		#undef GET
		#undef GET_V2
		return true;
	}
};
DriverApi g_cu;

bool DriverApiReady() noexcept {
	static const bool ready = g_cu.Load(); // Publish the whole table, never a partially loaded first symbol.
	return ready;
}

// TRT 11 的 ILogger：纯虚类。**文件级单例**：TRT 拿的是指针，logger 必须
// 比 runtime 活得久 —— function-local 挂在栈上就是退出崩溃。
class TrtLogger : public nvinfer1::ILogger {
public:
	void log(Severity severity, const char* msg) noexcept override {
		if (severity == Severity::kERROR || severity == Severity::kINTERNAL_ERROR) {
			D5_LOG_WARN(L"TRT: %hs", msg);
		}
	}
};
TrtLogger g_trtLogger;

// readback 支持的每像素字节（不支持的格式 RequestFrame 整帧放弃）
uint32_t BytesPerPixel(DXGI_FORMAT format) noexcept {
	switch (format) {
	case DXGI_FORMAT_R8G8B8A8_UNORM:
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
	case DXGI_FORMAT_B8G8R8A8_UNORM:
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
	case DXGI_FORMAT_R10G10B10A2_UNORM:
	case DXGI_FORMAT_R11G11B10_FLOAT:
		return 4;
	case DXGI_FORMAT_R16G16B16A16_FLOAT:
		return 8;
	case DXGI_FORMAT_R32G32B32A32_FLOAT:
		return 16;
	default:
		return 0;
	}
}

}  // namespace

//=========================== 生命周期 ===========================

bool SegMaskFilter::Initialize(ID3D12Device* device, ID3D12CommandQueue* queue,
	HMODULE selfModule) noexcept {
	if (_worker.joinable()) return _device == device;
	if (!device || !queue) return false;
	_stop.store(false);
	_device = device;
	_queue = queue;
	_selfModule = selfModule;
	if (_device) _device->AddRef();
	if (_queue) _queue->AddRef();
	if (FAILED(_device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
			IID_PPV_ARGS(&_fence)))) {
		Fail(L"语义蒙版：fence 建不出来");
		return false;
	}
	_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	if (!_fenceEvent) { Fail(L"语义蒙版：事件创建失败"); return false; }
	try { _worker = std::thread(&SegMaskFilter::WorkerLoop, this); }
	catch (...) { Fail(L"语义蒙版：worker 启动失败"); return false; }
	return true;
}

void SegMaskFilter::Fail(const wchar_t* msg) noexcept {
	if (!_trtFailed.exchange(true, std::memory_order_acq_rel)) {
		wcsncpy_s(_lastError, msg, _TRUNCATE);
		_lastError[255] = 0;
		D5_LOG_WARN(L"语义蒙版不可用：%s", msg);
	}
}

void SegMaskFilter::TeardownForExit() noexcept {
	_stop.store(true, std::memory_order_release);
	if (_worker.joinable()) _worker.join();
    {
        std::lock_guard lock(_captureMutex);
        if (_retainForExit.load() || _pendingList) {
            D5_LOG_WARN(L"语义蒙版退出：采集仍在未提交列表中或外部 GPU 未完成，保留 GPU 资源");
            return;
        }
    }
    const uint64_t lastCapture = _copyFenceValue[0].load();
    if (_fence && lastCapture && _fence->GetCompletedValue() < lastCapture) {
        if (SUCCEEDED(_fence->SetEventOnCompletion(lastCapture, _fenceEvent)))
            WaitForSingleObject(_fenceEvent, 2000);
        if (_fence->GetCompletedValue() < lastCapture) {
            D5_LOG_WARN(L"语义蒙版退出：GPU 采集尚未完成，保留资源直到进程退出");
            return; // Never release/reuse storage while a device may still write it.
        }
    }
	// worker 的退出路径里已经拆了 TRT / debug 窗；这里只兜底（幂等）
	ReleaseTrt();
	if (_fenceEvent) { CloseHandle(_fenceEvent); _fenceEvent = nullptr; }
	for (uint32_t i = 0; i < RB_SLOTS; ++i) {
		if (_readback[i]) { _readback[i]->Release(); _readback[i] = nullptr; }
	}
	if (_copyList) { _copyList->Release(); _copyList = nullptr; }
	if (_copyAlloc) { _copyAlloc->Release(); _copyAlloc = nullptr; }
	if (_fence) { _fence->Release(); _fence = nullptr; }
	if (_queue) { _queue->Release(); _queue = nullptr; }
	if (_device) { _device->Release(); _device = nullptr; }
}

//=========================== TRT 装载（worker 线程） ===========================

bool SegMaskFilter::EnsureTrt() noexcept {
	if (_trtReady.load(std::memory_order_acquire)) return true;
	if (_trtFailed.load(std::memory_order_acquire)) return false;

	// Runtime + plans belong to the same package, never the game's DLL search path.
	wchar_t selfPath[32768]{};
	const DWORD selfLength = _selfModule ? GetModuleFileNameW(_selfModule, selfPath, DWORD(std::size(selfPath))) : 0;
	if (!selfLength || selfLength >= std::size(selfPath)) {
		Fail(L"语义蒙版：拿不到 core 自己的路径");
		return false;
	}
	const std::filesystem::path beside =
		std::filesystem::path(selfPath).parent_path();
	const auto selected = SelectSegmentationRuntime(beside);
	const auto planPath = selected.library.parent_path() / L"models" / L"yolo11n-seg.plan";
	_leanRuntime = selected.lean;
	D5_LOG_INFO(L"Semantic TensorRT backend=%s runtime=%s headerVersion=%d YOLO=%s; automatic runtime fallback disabled",
		selected.Name(), selected.library.c_str(), TRT_RT_VERSION, planPath.c_str());
	_nvinfer = LoadLibraryExW(selected.library.c_str(), nullptr,
		LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
	if (!_nvinfer) {
		D5_LOG_WARN(L"Semantic TensorRT %s load failed: path=%s Win32=%lu; no fallback to another runtime",
			selected.Name(), selected.library.c_str(), GetLastError());
		Fail(_leanRuntime ? L"TensorRT Lean 装载失败：请使用完整的 Lean 运行时和匹配模型包（不自动退回 Full）"
			: L"找不到或无法装载 nvinfer_11.dll（放在 core dll 旁边）—— 语义蒙版功能关闭");
		return false;
	}
	// createInferRuntime_INTERNAL：唯一要 GetProcAddress 的符号，
	// 之后全走 NvInferRuntime.h 的内联虚表（#70 验证过不需要 .lib）。
	using CreateRuntimeFn = void* (*)(void*, int32_t);
	const auto createRuntime = reinterpret_cast<CreateRuntimeFn>(
		GetProcAddress(_nvinfer, "createInferRuntime_INTERNAL"));
	if (!createRuntime) {
		Fail(L"TensorRT 运行时没有 createInferRuntime_INTERNAL —— 版本不匹配");
		return false;
	}
	using GetVersionFn = int32_t (*)();
	const auto getVersion = reinterpret_cast<GetVersionFn>(GetProcAddress(_nvinfer, "getInferLibVersion"));
	wchar_t loadedPath[32768]{};
	GetModuleFileNameW(_nvinfer, loadedPath, DWORD(std::size(loadedPath)));
	D5_LOG_INFO(L"Semantic TensorRT loaded: backend=%s actual=%s libraryVersion=%d headerVersion=%d",
		selected.Name(), loadedPath, getVersion ? getVersion() : -1, TRT_RT_VERSION);

	// CUDA 主上下文（TRT enqueue 要在有效上下文里跑；非默认流 —— 文档要求）
	if (!DriverApiReady()) {
		Fail(L"nvcuda.dll 不可用（驱动太老？）");
		return false;
	}
	CU_CALL(g_cu.cuInit(0));
	{
		int device = -1, count = 0;
        CU_CALL(g_cu.cuDeviceGetCount(&count));
        const LUID wanted = _device->GetAdapterLuid();
        for (int i = 0; i < count; ++i) {
            int candidate = -1; char luid[8]{}; unsigned nodeMask = 0;
            if (g_cu.cuDeviceGet(&candidate, i) == 0 &&
                g_cu.cuDeviceGetLuid(luid, &nodeMask, candidate) == 0 &&
                !memcmp(luid, &wanted, sizeof(wanted))) { device = candidate; break; }
        }
        if (device < 0) { Fail(L"语义蒙版：CUDA 没有匹配游戏适配器的设备"); return false; }
        _cuDevice = device;
		void* ctx = nullptr;
		CU_CALL(g_cu.cuDevicePrimaryCtxRetain(&ctx, device));
		_cuCtx = ctx;
		CU_CALL(g_cu.cuCtxSetCurrent(ctx));
		void* stream = nullptr;
		CU_CALL(g_cu.cuStreamCreate(&stream, 0));
		_cuStream = stream;
	}

	_runtime = createRuntime(&g_trtLogger, TRT_RT_VERSION);
	if (!_runtime) {
		Fail(L"createInferRuntime 失败 —— TensorRT 运行时与 core API 版本不匹配");
		return false;
	}
	auto* runtime = static_cast<nvinfer1::IRuntime*>(_runtime);

	// plan（~13MB，一次读完）
	std::vector<uint8_t> plan;
	{
		FILE* f = _wfsopen(planPath.c_str(), L"rb", _SH_DENYNO);
		if (!f) {
			Fail(L"找不到 models\\yolo11n-seg.plan —— 语义蒙版功能关闭");
			return false;
		}
		fseek(f, 0, SEEK_END);
		const long size = ftell(f);
		fseek(f, 0, SEEK_SET);
		plan.resize(size > 0 ? size_t(size) : 0);
		const bool ok = !plan.empty() &&
			fread(plan.data(), 1, plan.size(), f) == plan.size();
		fclose(f);
		if (!ok) {
			Fail(L"plan 文件读不全");
			return false;
		}
	}
	_engine = runtime->deserializeCudaEngine(plan.data(), plan.size());
	if (!_engine) {
		Fail(_leanRuntime ? L"Lean 模型无法装载：请使用配套的运行时、兼容模型和 GPU；不自动退回 Full"
			: L"plan 反序列化失败 —— TensorRT 版本/GPU 架构和 plan 不匹配");
		return false;
	}
	auto* engine = static_cast<nvinfer1::ICudaEngine*>(_engine);
	_context = engine->createExecutionContext();
	if (!_context) {
		Fail(L"创建执行上下文失败");
		return false;
	}
	auto* context = static_cast<nvinfer1::IExecutionContext*>(_context);

	// TRT 内部会 push/pop 线程的 current context：到这里为止 deserialize 都
	// 成功了（那需要活上下文），紧接着的 cuMemAlloc 却报 201 INVALID_CONTEXT
	//（56456 那局；Python 干净进程同序复现不出 —— 进程环境差异）。探一次
	// 点名 + 无条件重申：SetCurrent 幂等、纳秒级，别赌它还挂着。
	{
		void* cur = nullptr;
		g_cu.cuCtxGetCurrent(&cur);
		if (cur != _cuCtx) {
			D5_LOG_WARN(L"语义蒙版：current context 被摘了（现在=%p 期望=%p）"
				L"—— 重申", cur, _cuCtx);
		}
		CU_CALL(g_cu.cuCtxSetCurrent(_cuCtx));
	}

	// IO 绑定：输入就一个（images），输出两个（检测头 / proto）。
	// 按 mode 绑：INPUT -> _dInput；OUTPUT 按出现顺序 -> _dOut0/_dOut1
	//（shape 从 engine 问，不硬编码 —— 换 plan 不用改这里）。
	constexpr size_t IN_BYTES = size_t(INPUT_WH) * INPUT_WH * 3 * sizeof(float);
	int32_t nOut = 0;
	for (int32_t i = 0; i < engine->getNbIOTensors(); ++i) {
		const char* name = engine->getIOTensorName(i);
		const nvinfer1::Dims dims = engine->getTensorShape(name);
		const nvinfer1::DataType dtype = engine->getTensorDataType(name);
		const nvinfer1::TensorIOMode mode = engine->getTensorIOMode(name);
		if (dtype != nvinfer1::DataType::kFLOAT) {
			Fail(L"plan 的 tensor 不是 fp32 —— 和预期不符");
			return false;
		}
		size_t vol = 1;
		for (int d = 0; d < dims.nbDims; ++d) {
			if (dims.d[d] <= 0) {
				Fail(L"plan 有动态维度 —— 请用固定 shape 的 plan");
				return false;
			}
			vol *= size_t(dims.d[d]);
		}
		const size_t bytes = vol * sizeof(float);
		void* devPtr = nullptr;
		if (mode == nvinfer1::TensorIOMode::kINPUT) {
			if (bytes != IN_BYTES) {
				Fail(L"输入尺寸和 640x640x3 不符");
				return false;
			}
			CU_CALL(g_cu.cuMemAlloc(&devPtr, bytes));
			_dInput = devPtr;
		} else {
			if (nOut >= 2) {
				Fail(L"plan 输出超过 2 个 —— 布局和预期不符");
				return false;
			}
			CU_CALL(g_cu.cuMemAlloc(&devPtr, bytes));
			if (nOut == 0) {
				_dOut0 = devPtr;
				_hOut0.resize(vol);
				_out0Vol = uint32_t(vol);
			} else {
				_dOut1 = devPtr;
				_hOut1.resize(vol);
				_out1Vol = uint32_t(vol);
			}
			++nOut;
		}
		context->setTensorAddress(name, devPtr);
	}
	if (!_dInput || nOut != 2) {
		Fail(L"plan 的 IO 布局和预期不符（要 1 入 2 出）");
		return false;
	}
	_hInput.resize(size_t(INPUT_WH) * INPUT_WH * 3);
	// anchors 数 = out0Vol / 116（排布 #71 钉死：4+80+32）
	_anchors = _out0Vol / 116;
	if (_anchors == 0 || _out0Vol % 116 != 0 ||
		_out1Vol != 32 * PROTO_WH * PROTO_WH) {
		Fail(L"输出 shape 和 yolo11n-seg 预期不符");
		return false;
	}

	// 预热一次（tactic/kernel 编译别算进第一帧的耗时）
	if (!RunInference()) {
		return false;
	}
	_trtReady.store(true, std::memory_order_release);
	D5_LOG_INFO(L"语义蒙版就绪：yolo11n-seg 引擎已装载"
		L"（输入 640x640，anchors %u，person 阈值 %.2f）",
		_anchors, CONF_TH);
	return true;
}

// ---- 第二引擎（#81）：yolo26s-sem ADE20K 场景解析 ----
// 可选：plan 缺了只 warn 一次并降级（COCO 12 组照跑），不 Fail。
// IO：1 入（共用 _dInput —— ADE20K/COCO 同为 Ultralytics 640 letterbox
// /114/255 归一，输入完全一致，letterbox 只做一次喂两个模型）
// + 1 出（640x640 uint8 逐像素 train-ID 类图，argmax 已烘进图）。
bool SegMaskFilter::EnsureAdeEngine() noexcept {
	if (_adeReady || _adeFailed) return _adeReady;
	if (!_trtReady.load(std::memory_order_acquire)) return false;   // 主引擎先就绪
	// 只报一次降级：plan 不在就安静降级（和"缺 dll"同一个策略）
	static bool toldMissing = false;
	wchar_t selfPath[32768]{};
	const DWORD selfLength = _selfModule ? GetModuleFileNameW(_selfModule, selfPath, DWORD(std::size(selfPath))) : 0;
	if (!selfLength || selfLength >= std::size(selfPath)) {
		return false;
	}
	const std::filesystem::path planPath =
		std::filesystem::path(selfPath).parent_path() / L"models" /
		L"yolo26s-sem-ade20k.plan";
	auto* runtime = static_cast<nvinfer1::IRuntime*>(_runtime);
	if (!runtime || !_nvinfer) return false;
	D5_LOG_INFO(L"Semantic TensorRT ADE: backend=%s plan=%s", _leanRuntime ? L"Lean" : L"Full", planPath.c_str());

	std::vector<uint8_t> plan;
	{
		FILE* f = _wfsopen(planPath.c_str(), L"rb", _SH_DENYNO);
		if (!f) {
			if (!toldMissing) {
				toldMissing = true;
				D5_LOG_WARN(L"语义蒙版：models\\yolo26s-sem-ade20k.plan 不在 —— "
					L"场景组（建筑/植被/天空/水域/地形）降级关闭，COCO 12 组照跑");
			}
			_adeFailed = true;   // 本局不再试（WorkerLoop 每帧都调这里）
			return false;
		}
		fseek(f, 0, SEEK_END);
		const long size = ftell(f);
		fseek(f, 0, SEEK_SET);
		plan.resize(size > 0 ? size_t(size) : 0);
		const bool ok = !plan.empty() &&
			fread(plan.data(), 1, plan.size(), f) == plan.size();
		fclose(f);
		if (!ok) {
			_adeFailed = true;
			return false;
		}
	}
	_engine2 = runtime->deserializeCudaEngine(plan.data(), plan.size());
	if (!_engine2) {
		if (!toldMissing) {
			toldMissing = true;
			D5_LOG_WARN(L"yolo26s-sem plan 反序列化失败（GPU 架构/nvinfer 版本"
				L"不匹配）—— 场景组降级关闭");
		}
		_adeFailed = true;
		return false;
	}
	auto* engine2 = static_cast<nvinfer1::ICudaEngine*>(_engine2);
	_context2 = engine2->createExecutionContext();
	auto* context2 = static_cast<nvinfer1::IExecutionContext*>(_context2);
	// 引擎已建：之后的失败路径全走 failAde —— 裸 return 的话 WorkerLoop 下一帧
	// 重跑这里，反序列化一个新引擎盖掉旧指针（泄漏重试环，#72 同款）。
	auto failAde = [&]() -> bool {
		delete context2;          // delete nullptr 是 no-op
		_context2 = nullptr;
		delete engine2;
		_engine2 = nullptr;
		if (_dOut2) { g_cu.cuMemFree(_dOut2); _dOut2 = nullptr; }
		_hOut2.clear();
		_adeFailed = true;        // 本局不再试
		return false;
	};
	if (!context2) return failAde();

	// IO 绑定：入 = 共用 _dInput（先确认 shape 一致）；出 = 类图（uint8 允许）。
	bool inputBound = false;
	bool outputBound = false;
	for (int32_t i = 0; i < engine2->getNbIOTensors(); ++i) {
		const char* name = engine2->getIOTensorName(i);
		const nvinfer1::Dims dims = engine2->getTensorShape(name);
		const nvinfer1::DataType dtype = engine2->getTensorDataType(name);
		const nvinfer1::TensorIOMode mode = engine2->getTensorIOMode(name);
		size_t vol = 1;
		bool dynamic = false;
		for (int d = 0; d < dims.nbDims; ++d) {
			if (dims.d[d] <= 0) dynamic = true;
			else vol *= size_t(dims.d[d]);
		}
		if (dynamic) return failAde();
		if (mode == nvinfer1::TensorIOMode::kINPUT) {
			const size_t bytes = vol * sizeof(float);
			if (bytes != size_t(INPUT_WH) * INPUT_WH * 3 * sizeof(float)) {
				return failAde();
			}
			context2->setTensorAddress(name, _dInput);   // 共用，不再分配
			inputBound = true;
		} else {
			if (dtype != nvinfer1::DataType::kUINT8) return failAde();   // 类图 uint8
			if (vol != size_t(INPUT_WH) * INPUT_WH) return failAde();
			const size_t bytes = vol * sizeof(uint8_t);
			void* devPtr = nullptr;
			CU_CALL(g_cu.cuMemAlloc(&devPtr, bytes));
			_dOut2 = devPtr;
			_hOut2.resize(vol);
			context2->setTensorAddress(name, devPtr);
			outputBound = true;
		}
	}
	if (!inputBound || !outputBound || !_dOut2) return failAde();

	// 预热一次（同主引擎：tactic 编译别算进第一帧）
	{
		if (_cuCtx) CU_CALL(g_cu.cuCtxSetCurrent(_cuCtx));
		if (!context2->enqueueV3(reinterpret_cast<cudaStream_t>(_cuStream))) {
			return failAde();
		}
		CU_CALL(g_cu.cuStreamSynchronize(_cuStream));
	}
	_adeReady = true;
	D5_LOG_INFO(L"语义蒙版：yolo26s-sem 场景引擎已装载（输出 640x640 类图，"
		L"150 类 → 6 场景组 12..17）");
	return true;
}

void SegMaskFilter::ReleaseTrt() noexcept {
	if (_cuCtx && g_cu.cuCtxSetCurrent) g_cu.cuCtxSetCurrent(_cuCtx);
	if (_cuStream && g_cu.cuStreamSynchronize) g_cu.cuStreamSynchronize(_cuStream);
	// 顺序：context -> engine -> runtime -> 显存 -> 流/上下文 -> 模块。
	// 幂等（TeardownForExit 在没初始化成功时也会走到）。
	// TRT 11 析构 public 纯虚 + 内联 default —— 直接 delete。
	if (_context) {
		delete static_cast<nvinfer1::IExecutionContext*>(_context);
		_context = nullptr;
	}
	if (_engine) {
		delete static_cast<nvinfer1::ICudaEngine*>(_engine);
		_engine = nullptr;
	}
	// #81 第二引擎同一套顺序（共用 runtime/dInput，不重复释放）
	if (_context2) {
		delete static_cast<nvinfer1::IExecutionContext*>(_context2);
		_context2 = nullptr;
	}
	if (_engine2) {
		delete static_cast<nvinfer1::ICudaEngine*>(_engine2);
		_engine2 = nullptr;
	}
	_adeReady = false;
	if (_runtime) {
		delete static_cast<nvinfer1::IRuntime*>(_runtime);
		_runtime = nullptr;
	}
	if (_dInput) { g_cu.cuMemFree(_dInput); _dInput = nullptr; }
	if (_dOut0) { g_cu.cuMemFree(_dOut0); _dOut0 = nullptr; }
	if (_dOut1) { g_cu.cuMemFree(_dOut1); _dOut1 = nullptr; }
	if (_dOut2) { g_cu.cuMemFree(_dOut2); _dOut2 = nullptr; }
	if (_cuStream) { g_cu.cuStreamDestroy(_cuStream); _cuStream = nullptr; }
	if (_cuCtx) {
		g_cu.cuCtxSetCurrent(nullptr);   // 先脱离再释放主上下文
		if (_cuDevice >= 0) g_cu.cuDevicePrimaryCtxRelease(_cuDevice);
		_cuDevice = -1;
		_cuCtx = nullptr;
	}
	if (_nvinfer) {
		FreeLibrary(_nvinfer);
		_nvinfer = nullptr;
	}
}

//=========================== worker ===========================

void SegMaskFilter::WorkerLoop() noexcept {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    while (!_stop.load()) {
        if (!_active.load()) { _frameWanted.store(false); Sleep(10); continue; }
        if (!EnsureTrt()) { Sleep(50); continue; }
        // Single-model mode: never load or enqueue the ADE scene model, including old profiles.
        if (_debugHwnd && !_debugWant.load()) ShowWindow(_debugHwnd, SW_HIDE);
        const uint32_t slot = 0;
        if (!_copyInFlight[slot].load(std::memory_order_acquire)) {
            _frameWanted.store(true); Sleep(5); continue;
        }
        const uint64_t value = _copyFenceValue[slot].load(std::memory_order_acquire);
        if (!value) { Sleep(2); continue; } // recorded but not submitted yet
        const uint64_t completed = _fence->GetCompletedValue();
        if (completed == UINT64_MAX) { Fail(L"语义蒙版：D3D12 设备丢失"); break; }
        if (completed < value) {
            if (SUCCEEDED(_fence->SetEventOnCompletion(value, _fenceEvent)))
                WaitForSingleObject(_fenceEvent, 20);
            continue; // Timeout NEVER grants ownership of a GPU resource.
        }
        void* mapped = nullptr;
        const uint32_t w = _frameWidth, h = _frameHeight;
        D3D12_RANGE read{0, size_t(_rbPitch) * h};
        if (SUCCEEDED(_readback[slot]->Map(0, &read, &mapped)) && mapped) {
            _rgbaFrame.resize(size_t(w) * h * 4);
            UnpackFrameToRgba(static_cast<const uint8_t*>(mapped),
                _rbPitch, w, h, _frameFormat, _rgbaFrame.data());
            D3D12_RANGE written{0, 0};
            _readback[slot]->Unmap(0, &written);
            LetterboxAndInfer(_rgbaFrame.data(), w, h);
        }
        _copyInFlight[slot].store(false, std::memory_order_release);
        _frameWanted.store(true);
    }
    _frameWanted.store(false);
    _trtReady.store(false);
    if (_debugDc && _debugHwnd) ReleaseDC(_debugHwnd, _debugDc);
    _debugDc = nullptr;
    if (_debugHwnd) { DestroyWindow(_debugHwnd); _debugHwnd = nullptr; }
    if (_debugBmp) { DeleteObject(_debugBmp); _debugBmp = nullptr; _debugBits = nullptr; }
    ReleaseTrt(); // CUDA context/resources are dismantled on their owning worker.
    CoUninitialize();
}

bool SegMaskFilter::CopyLatest(SemanticMaskSnapshot& out) noexcept {
    std::unique_lock lock(_publishMutex, std::try_to_lock);
    if (!lock.owns_lock()) return false;
    if (_published.version == out.version) return true;
    out = _published;
    return true;
}

//=========================== 帧解包 / letterbox / 推理 ===========================

void SegMaskFilter::UnpackFrameToRgba(const uint8_t* src, uint32_t pitch,
	uint32_t w, uint32_t h, DXGI_FORMAT format, uint8_t* rgba) noexcept {
	// 只支持 backbuffer 常见格式（BytesPerPixel 挡掉的不进来）。
	// mask 是语义信息，通道顺序/色彩精度都不敏感 —— 8bit RGB 足够。
	for (uint32_t y = 0; y < h; ++y) {
		const uint8_t* row = src + size_t(y) * pitch;
		uint8_t* dstRow = rgba + size_t(y) * w * 4;
		for (uint32_t x = 0; x < w; ++x) {
			uint8_t* d = dstRow + size_t(x) * 4;
			const uint32_t p = *reinterpret_cast<const uint32_t*>(
				row + size_t(x) * (BytesPerPixel(format)));
			switch (format) {
			case DXGI_FORMAT_R8G8B8A8_UNORM:
			case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
				d[0] = uint8_t(p);
				d[1] = uint8_t(p >> 8);
				d[2] = uint8_t(p >> 16);
				d[3] = 255;
				break;
			case DXGI_FORMAT_B8G8R8A8_UNORM:
			case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
				d[0] = uint8_t(p >> 16);
				d[1] = uint8_t(p >> 8);
				d[2] = uint8_t(p);
				d[3] = 255;
				break;
			case DXGI_FORMAT_R10G10B10A2_UNORM:
				// 10 位 -> 8 位：取高 8 位（视觉够用，mask 不吃精度）
				d[0] = uint8_t((p & 0x3FF) >> 2);
				d[1] = uint8_t(((p >> 10) & 0x3FF) >> 2);
				d[2] = uint8_t(((p >> 20) & 0x3FF) >> 2);
				d[3] = 255;
				break;
            case DXGI_FORMAT_R11G11B10_FLOAT: {
                auto uf = [](uint32_t bits, unsigned mantissaBits) {
                    const uint32_t e = bits >> mantissaBits, m = bits & ((1u << mantissaBits)-1);
                    if (e == 31) return m ? 0.0f : 1.0f;
                    return e ? std::ldexp(float((1u << mantissaBits) + m), int(e)-15-int(mantissaBits))
                        : std::ldexp(float(m), -14-int(mantissaBits));
                };
                d[0]=MaskByte(uf(p&0x7FFu,6));d[1]=MaskByte(uf((p>>11)&0x7FFu,6));d[2]=MaskByte(uf(p>>22,5));d[3]=255;
                break;
            }
            case DXGI_FORMAT_R32G32B32A32_FLOAT: {
                const float* px = reinterpret_cast<const float*>(row + size_t(x)*16);
                for (int c=0;c<3;++c) d[c]=MaskByte(px[c]);d[3]=255;break;
            }

			case DXGI_FORMAT_R16G16B16A16_FLOAT: {
				const uint16_t* px = reinterpret_cast<const uint16_t*>(
					row + size_t(x) * 8);
				for (int c = 0; c < 3; ++c) {
					const float f = HalfToFloat(px[c]);
					d[c] = MaskByte(f);
				}
				d[3] = 255;
				break;
			}
			default:
				d[0] = d[1] = d[2] = 0;
				d[3] = 255;
				break;
			}
		}
	}
}

float SegMaskFilter::HalfToFloat(uint16_t h) noexcept {
	const uint32_t sign = (h >> 15) & 1;
	const uint32_t exp = (h >> 10) & 0x1F;
	const uint32_t man = h & 0x3FF;
	uint32_t bits;
	if (exp == 0) {
		bits = sign << 31;   // 非规格化 -> 0（mask 用途足够）
	} else if (exp == 31) {
		bits = (sign << 31) | 0x7F800000 | (man << 13);
	} else {
		bits = (sign << 31) | ((exp - 15 + 127) << 23) | (man << 13);
	}
	float out;
	memcpy(&out, &bits, 4);
	return out;
}

bool SegMaskFilter::LetterboxAndInfer(const uint8_t* rgba,
	uint32_t w, uint32_t h) noexcept {
	// CPU letterbox：等比缩到 640 内容区 + pad 114/255（Ultralytics 标准）。
	// 降采样 nearest（源坐标取整）：分割 mask 是低频信息，mask 边界在
	// 160 网格上本来就只有 4px 精度 —— 双线性在这里是白付的代价。
	const float scale = float(INPUT_WH) / float(w > h ? w : h);
	const uint32_t contentW = w < h
		? uint32_t(float(w) * scale + 0.5f) : INPUT_WH;
	const uint32_t contentH = h < w
		? uint32_t(float(h) * scale + 0.5f) : INPUT_WH;
	// **CHW 平面布局**（TRT 输入 (1,3,640,640) = #71 Python 喂的 bchw 同款）：
	// 三个 640x640 平面各管一个通道。之前写成交错 HWC（像素跨步 3）——
	// 模型把交错流当平面读，整图变彩条噪波，yolo 满屏乱框（debug 小窗
	// 100% 红 = 全屏 person；testapp 无人物场景本该全绿，就是这一局查出的）。
	float* const out = _hInput.data();
	float* const planeR = out;
	float* const planeG = out + size_t(INPUT_WH) * INPUT_WH;
	float* const planeB = out + 2 * size_t(INPUT_WH) * INPUT_WH;
	std::fill(out, out + size_t(INPUT_WH) * INPUT_WH * 3, 114.0f / 255.0f);
	for (uint32_t y = 0; y < contentH && y < INPUT_WH; ++y) {
		const uint32_t sy = uint32_t(float(y) / scale);
		const uint32_t syc = SemanticSourceRow(sy < h ? sy : h - 1, h, _frameFlipY);
		const uint8_t* sRow = rgba + size_t(syc) * w * 4;
		float* rRow = planeR + size_t(y) * INPUT_WH;
		float* gRow = planeG + size_t(y) * INPUT_WH;
		float* bRow = planeB + size_t(y) * INPUT_WH;
		for (uint32_t x = 0; x < contentW && x < INPUT_WH; ++x) {
			const uint32_t sx = uint32_t(float(x) / scale);
			const uint32_t sxc = sx < w ? sx : w - 1;
			const uint8_t* s = sRow + size_t(sxc) * 4;
			rRow[x] = s[0] / 255.0f;
			gRow[x] = s[1] / 255.0f;
			bRow[x] = s[2] / 255.0f;
		}
	}
	// 推理和 decode 分开调：decode 依赖 _frameWidth（RequestFrame 跑过才有），
	// 焰场（EnsureTrt）里没有帧 —— 老版把 DecodeAndPublish 塞进 RunInference，
	// 暖场 decode 因 _frameWidth==0 静默 false，EnsureTrt 整个跟着 false，
	// WorkerLoop 每 2s 重装一遍引擎（见上面 CU_CALL 的注释，就是这局查的）。
	_inferenceGroups = _groupEnabled.load();
	if (!RunInference()) return false;
	return DecodeAndPublish(_hOut0.data(), _hOut1.data());
}

// _hInput -> TRT -> D2H。计时含 D2H（状态区要报的真实成本）。
// 只跑 GPU 链路 —— decode 由调用方接着做（见 LetterboxAndInfer 的注释）。
bool SegMaskFilter::RunInference() noexcept {
	// 每个批次前重申 current context：TRT 的调用可能把它摘掉
	//（见 EnsureTrt 里 createExecutionContext 后那段），别赌还挂着。
	if (_cuCtx) CU_CALL(g_cu.cuCtxSetCurrent(_cuCtx));
	auto* context = static_cast<nvinfer1::IExecutionContext*>(_context);
	LARGE_INTEGER t0, t1, freq;
	QueryPerformanceCounter(&t0);
	CU_CALL(g_cu.cuMemcpyHtoDAsync(_dInput, _hInput.data(),
		_hInput.size() * sizeof(float), _cuStream));
	if (!context->enqueueV3(reinterpret_cast<cudaStream_t>(_cuStream))) {
		D5_LOG_WARN(L"语义蒙版：enqueueV3 返回 false —— 这一帧放弃");
		return false;
	}
	CU_CALL(g_cu.cuMemcpyDtoHAsync(_hOut0.data(), _dOut0,
		_hOut0.size() * sizeof(float), _cuStream));
	CU_CALL(g_cu.cuMemcpyDtoHAsync(_hOut1.data(), _dOut1,
		_hOut1.size() * sizeof(float), _cuStream));
	// #81 第二引擎：同一条流接着排（输入共用 _dInput，两个模型读同一份，
	// 无写冲突；顺序执行，D2H 各归各）。_adeReady=false 时零开销。
	if (_adeReady && (_inferenceGroups & 0x3F000u)) {
		auto* context2 = static_cast<nvinfer1::IExecutionContext*>(_context2);
		if (!context2->enqueueV3(reinterpret_cast<cudaStream_t>(_cuStream))) {
			D5_LOG_WARN(L"语义蒙版：场景引擎 enqueueV3 返回 false —— 这帧只用 COCO");
			// 两个标志一起置：只放 _adeReady 的话 WorkerLoop 每帧重跑
			// EnsureAdeEngine 反序列化 —— #72 踩过的引擎泄漏重试环。
			_adeReady = false;
			_adeFailed = true;
		} else {
			CU_CALL(g_cu.cuMemcpyDtoHAsync(_hOut2.data(), _dOut2,
				_hOut2.size() * sizeof(uint8_t), _cuStream));
		}
	}
	CU_CALL(g_cu.cuStreamSynchronize(_cuStream));
	QueryPerformanceCounter(&t1);
	QueryPerformanceFrequency(&freq);
	_lastInferenceMs = float(t1.QuadPart - t0.QuadPart) * 1000.0f /
		float(freq.QuadPart);
	return true;
}

//=========================== 解码 + 发布 ===========================

bool SegMaskFilter::DecodeAndPublish(const float* det, const float* proto) noexcept {
	// det: (116, anchors)。排布（#71 数据反推钉死）：
	// 4 box + 80 cls(**已 sigmoid**，别再套) + 32 coef(logit)。
	const uint32_t anchors = _anchors;
	const float* boxes = det;                     // 4 x anchors
	const float* classes = det + 4 * anchors;     // 80 x anchors（概率）
	const float* coefs = det + 84 * anchors;      // 32 x anchors（logit）

	// 多类实例收集 + NMS（#79 语义分组）：
	// 每个 anchor 取 80 类 argmax → conf + 类别 → 整合组（kClassGroup）；
	// 组开关过滤（worker 侧：全类解码的 proto 合成按实例数走，12 组全开 +
	// 繁忙场景会到几十个实例；过滤后典型 1~3 组。开关变化 ≤33ms 生效）；
	// NMS 只比同组实例（车和人的框本来就该重叠，跨组抑制会误杀）。
	struct Det { float x1, y1, x2, y2, conf; int anchor; uint8_t group; };
    std::vector<Det> candidates;
    candidates.reserve(256);
    const uint32_t groupMask = _inferenceGroups;
    for (uint32_t a = 0; a < anchors; ++a) {
        uint8_t bestCls = 0; float conf = 0;
        for (uint32_t c = 0; c < 80; ++c) {
            const float p = classes[size_t(c) * anchors + a];
            if (p > conf) { conf = p; bestCls = uint8_t(c); }
        }
        if (!std::isfinite(conf) || conf < CONF_TH) continue;
        const uint8_t group = kClassGroup[bestCls];
        if (!(groupMask & (1u << group))) continue;
        const float cx = boxes[a], cy = boxes[anchors + a];
        const float bw = boxes[2 * anchors + a], bh = boxes[3 * anchors + a];
        if (!std::isfinite(cx) || !std::isfinite(cy) || !std::isfinite(bw) ||
            !std::isfinite(bh) || bw <= 0 || bh <= 0) continue;
        candidates.push_back({cx-bw*.5f, cy-bh*.5f, cx+bw*.5f, cy+bh*.5f, conf, int(a), group});
    }
    std::sort(candidates.begin(), candidates.end(), [](const Det& a, const Det& b) { return a.conf > b.conf; });
    Det dets[64]; uint32_t nDets = 0;
    for (const Det& d : candidates) {
        bool drop = false;
        for (uint32_t j = 0; j < nDets; ++j) {
            const Det& o = dets[j];
            if (o.group != d.group) continue;
            const float ix = std::max(0.0f, std::min(d.x2,o.x2)-std::max(d.x1,o.x1));
            const float iy = std::max(0.0f, std::min(d.y2,o.y2)-std::max(d.y1,o.y1));
            const float inter = ix*iy;
            const float denom = (d.x2-d.x1)*(d.y2-d.y1)+(o.x2-o.x1)*(o.y2-o.y1)-inter;
            if (denom > 0 && inter/denom > IOU_TH) { drop = true; break; }
        }
        if (!drop) dets[nDets++] = d;
        if (nDets == 64) break;
    }

	// 双图（worker 中转，#78 软边 + #79 分组）：
	//   _workerTemp  coverage 0..255 灰度（0=满强度、255=环境、中间=软边）
	//   _workerGroup 组 ID（0..11=组、255=无组）—— 消费端按组查强度。
	// 两条按同一像素同步写；发布也走同一版本号。
	const uint32_t fw = _frameWidth, fh = _frameHeight;
	if (!fw || !fh) return false;
	_workerTemp.assign(size_t(fw) * fh, 255);
	_workerGroup.assign(size_t(fw) * fh, 255);
	// 640 画布 -> 原图 的缩放（letterbox 的逆）
	const float toFrame = float(fw > fh ? fw : fh) / float(INPUT_WH);
	for (uint32_t i = 0; i < nDets; ++i) {
		const Det& d = dets[i];
		// proto 合成（#71 修好的链）：coef(logit) @ proto(160x160) -> sigmoid。
		// **#78：保留连续 sigmoid（0..1 软边）**，不再 >0.5 阈值化 ——
		// proto 头自带亚像素边缘信息，二值化把它整个丢掉。
		float mask160[PROTO_WH * PROTO_WH];
		for (uint32_t p = 0; p < PROTO_WH * PROTO_WH; ++p) {
			float acc = 0.0f;
			for (uint32_t c = 0; c < 32; ++c) {
				acc += coefs[c * anchors + d.anchor] * proto[c * PROTO_WH * PROTO_WH + p];
			}
			mask160[p] = 1.0f / (1.0f + std::exp(-acc));
		}
		// 检测框裁剪（proto mask 常溢出框外一点）：框内像素才看 mask。
		// 640 坐标 -> 原图坐标 -> proto 160 网格（proto 覆盖整个 640 画布，#71）。
		const int bx1 = d.x1 > 0 ? int(d.x1 * toFrame) : 0;
		const int by1 = d.y1 > 0 ? int(d.y1 * toFrame) : 0;
		const int bx2 = d.x2 * toFrame < float(fw) ? int(d.x2 * toFrame) : int(fw);
		const int by2 = d.y2 * toFrame < float(fh) ? int(d.y2 * toFrame) : int(fh);
		// **#78 框边羽化**：硬裁剪在 proto 高值贴框边时显出直线锯齿；
		// 框边 feather 像素内 0..1 渐变。取框短边/4 和屏宽/200 的较小者
		//（1080p 约 5px，框越窄羽化越少，避免小框整个被羽没）。
		const float fwid = float(bx2 - bx1), fhgt = float(by2 - by1);
		const float feather = fwid > 0 && fhgt > 0
			? (fwid < fhgt ? fwid : fhgt) / 4.0f : 0.0f;
		const float featherF = feather < float(fw) / 200.0f
			? feather : float(fw) / 200.0f;
		const float gScale = float(PROTO_WH) / float(INPUT_WH);   // 640 -> 160
		for (int y = by1; y < by2; ++y) {
			if (y < 0 || y >= int(fh)) continue;
			// 框边羽化系数（上下边）：featherF 像素内 0..1
			const float ty = featherF > 0.0f
				? (float(y - by1) + 0.5f) / featherF : 1.0f;
			const float by2f = featherF > 0 ? (float(by2 - y) - 0.5f) / featherF : 1;
			const float edgeY = ty < 1.0f ? ty
				: (by2f < 1.0f ? by2f : 1.0f);
			for (int x = bx1; x < bx2; ++x) {
				if (x < 0 || x >= int(fw)) continue;
				// 同样处理左右边
				const float tx = featherF > 0.0f
					? (float(x - bx1) + 0.5f) / featherF : 1.0f;
				const float bx2f = featherF > 0 ? (float(bx2 - x) - 0.5f) / featherF : 1;
				const float edgeX = tx < 1.0f ? tx
					: (bx2f < 1.0f ? bx2f : 1.0f);
				const float edge = edgeX < edgeY ? edgeX : edgeY;
				if (edge <= 0.0f) continue;
				// 原图像素 -> 640 画布 -> proto 160 网格（**像素中心**，#78：
				// +0.5f —— 之前整数角对齐系统性偏半格，边缘偏移可见）
				const float canvasX = (float(x) + 0.5f) / toFrame;
				const float canvasY = (float(y) + 0.5f) / toFrame;
				const float gx = canvasX * gScale - 0.5f;
				const float gy = canvasY * gScale - 0.5f;
				// **#78 双线性采样 160 网格**（最近邻在 1080p 下每格 ≈12px 硬块）。
				// 越界夹边（框外 proto 无意义，框由羽化接管）。
				auto clampPos = [&](float v) {
					return v < 0.0f ? 0.0f : v > float(PROTO_WH - 1) ? float(PROTO_WH - 1) : v;
				};
				const float sx = clampPos(gx), sy = clampPos(gy);
				const int x0 = int(sx), y0 = int(sy);
				const int x1i = x0 + 1 < PROTO_WH ? x0 + 1 : PROTO_WH - 1;
				const int y1i = y0 + 1 < PROTO_WH ? y0 + 1 : PROTO_WH - 1;
				const float fx = sx - float(x0), fy = sy - float(y0);
				const float m00 = mask160[size_t(y0) * PROTO_WH + x0];
				const float m10 = mask160[size_t(y0) * PROTO_WH + x1i];
				const float m01 = mask160[size_t(y1i) * PROTO_WH + x0];
				const float m11 = mask160[size_t(y1i) * PROTO_WH + x1i];
				const float bilinear =
					(1.0f - fx) * (1.0f - fy) * m00 + fx * (1.0f - fy) * m10 +
					(1.0f - fx) * fy * m01 + fx * fy * m11;
				// 组度 t = sigmoid(双线性) × 框羽化；多实例重叠 winner-takes-group：
				// t 更大的实例赢（组 ID 跟着换），不再"后写的盖前写的"。
				const float t = bilinear * edge;
				uint8_t& covPx = _workerTemp[size_t(y) * fw + x];
				uint8_t& grpPx = _workerGroup[size_t(y) * fw + x];
				const float cur = float(covPx) / 255.0f;   // 现值（1=环境）
				if (t > 1.0f - cur) {                      // 新实例更强 → 接管
					covPx = uint8_t((1.0f - t) * 255.0f + 0.5f);
					grpPx = d.group;
				}
			}
		}
	}

	// ---- #81 场景组合并：ADE 类图只认领 COCO 没认领的像素 ----
	// 前景优先：covPx != 255 的像素（COCO 实例已写，含软边渐变）原样保留，
	// ADE 只补 255（无实例）的像素 —— 背景语义（天空/树/建筑）不覆盖人物软边。
	// 类图是 640 画布上的逐像素 train ID（argmax 已烘进图，无置信度）：
	// 帧像素 → 640 画布（letterbox 逆变换，像素中心）→ 类 ID → kAdeGroup。
	// 只认场景组 12..17：映射到 0..11 的类（ADE 的 person/car/家具…）跳过
	//（COCO 实例模型才是这些像素的权威，前景优先）。组关了（enabled 位没置）
	// 不认领 —— 和 COCO 的 worker 侧过滤同一策略（≤33ms 生效）。
	if (_adeReady && (_inferenceGroups & 0x3F000u)) {
		const uint32_t groupMask = _inferenceGroups;
		// 帧像素 -> 640 画布：LetterboxAndInfer 的逆变换（内容放左上角，
		// 无居中 pad —— 和 COCO 解码的 toFrame 同一约定）。scale = 640/max(w,h)，
		// 帧内像素映射后必落在 [0,640)，画布 pad 区不会被采到。
		const float scale = float(INPUT_WH) / float(fw > fh ? fw : fh);
		for (uint32_t y = 0; y < fh; ++y) {
			uint8_t* covRow = _workerTemp.data() + size_t(y) * fw;
			uint8_t* grpRow = _workerGroup.data() + size_t(y) * fw;
			// 像素中心 +0.5f（#78 同款：整角对齐系统性偏半格）
			const float cy = (float(y) + 0.5f) * scale;
			const int gy = cy >= float(INPUT_WH - 1) ? int(INPUT_WH - 1) : int(cy);
			const uint8_t* clsRow = _hOut2.data() + size_t(gy) * INPUT_WH;
			for (uint32_t x = 0; x < fw; ++x) {
				if (covRow[x] != 255) continue;   // COCO 已认领（含软边）：前景优先
				const float cx = (float(x) + 0.5f) * scale;
				const int gx = cx >= float(INPUT_WH - 1) ? int(INPUT_WH - 1) : int(cx);
				const uint8_t trainId = clsRow[gx];
				if (trainId >= 150) continue;             // 255=忽略类 / 越界
				const uint8_t g = kAdeGroup[trainId];
				if (g < 12) continue;                     // COCO 同概念组：跳过
				if (!(groupMask & (1u << g))) continue;   // 组关了：这帧先不认领
				covRow[x] = 0;                            // 满覆盖（类图无置信度）
				grpRow[x] = g;
			}
		}
	}

    SemanticMaskSnapshot next;
    next.coverage = _workerTemp;
    next.groupIds = _workerGroup;
    next.width = fw; next.height = fh;
    next.sourceFlipY = _frameFlipY;
    RestoreSemanticSourceRows(next);
    next.version = _publishedVersion.load() + 1;
    next.publishedMs = _capturedMs; // Age the actual source frame, not the inference completion time.
    {
        std::lock_guard lock(_publishMutex);
        _published = std::move(next);
        _publishedWidth.store(fw); _publishedHeight.store(fh);
        _publishedVersion.store(_published.version, std::memory_order_release);
    }

	// 占比（#78 灰度加权）：按 1-t 加权 —— 软边像素算部分覆盖。
	float coverSum = 0.0f;
	for (uint8_t v : _workerTemp) {
		coverSum += 1.0f - float(v) / 255.0f;
	}
	_personPercent = coverSum * 100.0f / float(size_t(fw) * fh);
	if (_debugWant.load(std::memory_order_acquire)) UpdateDebugWindow();
	return true;
}

//=========================== debug 小窗 ===========================

void SegMaskFilter::UpdateDebugWindow() noexcept {
	const uint32_t fw = _frameWidth, fh = _frameHeight;
	if (!fw || !fh || _workerTemp.size() != size_t(fw) * fh) return;
	constexpr uint32_t DBG_W = 320;
	const uint32_t dbgH = uint32_t(float(fh) * DBG_W / fw);
	if (!dbgH) return;
	if (!_debugHwnd) {
		WNDCLASSW wc{};
		wc.lpfnWndProc = DefWindowProcW;
		wc.hInstance = GetModuleHandleW(nullptr);
		wc.lpszClassName = L"D5Q_SegMaskDebug";
		RegisterClassW(&wc);
		_debugHwnd = CreateWindowExW(
			WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
			wc.lpszClassName, L"D5Q SegMask", WS_POPUP,
			0, 0, int(DBG_W), int(dbgH), nullptr, nullptr, wc.hInstance, nullptr);
		if (!_debugHwnd) return;
		_debugW = DBG_W;
		_debugH = dbgH;
		_debugDc = GetDC(_debugHwnd);
		BITMAPINFO bi{};
		bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		bi.bmiHeader.biWidth = int(DBG_W);
		bi.bmiHeader.biHeight = -int(dbgH);   // 顶向下
		bi.bmiHeader.biPlanes = 1;
		bi.bmiHeader.biBitCount = 32;
		bi.bmiHeader.biCompression = BI_RGB;
		_debugBmp = CreateDIBSection(_debugDc, &bi, DIB_RGB_COLORS,
			&_debugBits, nullptr, 0);
		if (!_debugBmp) return;
		ShowWindow(_debugHwnd, SW_SHOWNOACTIVATE);
	}
	ShowWindow(_debugHwnd, SW_SHOWNOACTIVATE);
	if (_debugW != DBG_W || _debugH != dbgH) return;   // 分辨率变了：下轮重建（罕见）
	if (!_debugBits) return;
	// 下采样覆盖图 -> **组着色**（#79）：按组 ID 查调色板、按 t（0..1 覆盖度）
	// 调亮度 —— 每个整合组一种颜色：一眼看出"这块是哪个组"（分割质量 +
	// 组身份同时可见）。无组（255）= 黑。调色板 12 色尽量拉开色相距离。
	const uint8_t* cover = _workerTemp.data();
	const uint8_t* groups = _workerGroup.data();
	const float sx = float(fw) / DBG_W;
	const float sy = float(fh) / _debugH;
	// 18 组调色板（RGB）：COCO 12 色沿用；场景 6 组加后半
	//（建筑 砖红 / 植被 深绿 / 天空 天蓝 / 水域 湖蓝 / 地形 土黄 / 其他 灰褐）
	static const uint32_t PALETTE[SEM_GROUP_COUNT] = {
		0xE04040, 0xE08030, 0xD0C030, 0x50C050, 0x30B0B0, 0x4060E0,
		0x9040C0, 0xD040A0, 0xA06030, 0x909090, 0xF0F0F0, 0x608060,
		0xC06050, 0x308840, 0x60A8E8, 0x3878C8, 0xC8B060, 0x786858,
	};
	uint32_t* px = static_cast<uint32_t*>(_debugBits);
	for (uint32_t y = 0; y < _debugH; ++y) {
		const uint32_t syy = uint32_t(float(y) * sy);
		const uint8_t* covRow = cover + size_t(syy < fh ? syy : fh - 1) * fw;
		const uint8_t* grpRow = groups + size_t(syy < fh ? syy : fh - 1) * fw;
		for (uint32_t x = 0; x < _debugW; ++x) {
			const uint32_t sxx = uint32_t(float(x) * sx);
			const uint8_t c = covRow[sxx < fw ? sxx : fw - 1];
			const uint8_t g = grpRow[sxx < fw ? sxx : fw - 1];
			// t = 覆盖度（c=0 → 1，c=255 → 0）：组内实心、软边渐隐
			const float t = 1.0f - float(c) / 255.0f;
			uint32_t r = 0, gg = 0, b = 0;
			if (g < SEM_GROUP_COUNT) {
				const uint32_t col = PALETTE[g];
				r = uint32_t(float((col >> 16) & 0xFF) * t);
				gg = uint32_t(float((col >> 8) & 0xFF) * t);
				b = uint32_t(float(col & 0xFF) * t);
			}
			// 预乘：alpha=200 → 通道值*200/255（BGRA、AC_SRC_ALPHA）
			const uint32_t a = 200;
			r = r * a / 255; gg = gg * a / 255; b = b * a / 255;
			px[y * _debugW + x] = (a << 24) | (r << 16) | (gg << 8) | b;
		}
	}
	// 贴游戏窗口左下角
	RECT rc{};
	if (const HWND game = _gameHwnd.load(); game && GetWindowRect(game, &rc)) {
		SetWindowPos(_debugHwnd, HWND_TOPMOST,
			int(rc.left) + 16, int(rc.bottom) - int(_debugH) - 64,
			0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
	}
	HDC memDc = CreateCompatibleDC(_debugDc);
	HGDIOBJ old = SelectObject(memDc, _debugBmp);
	POINT ptSrc{ 0, 0 };
	SIZE size{ int(_debugW), int(_debugH) };
	BLENDFUNCTION blend{};
	blend.BlendOp = AC_SRC_OVER;
	blend.SourceConstantAlpha = 255;
	blend.AlphaFormat = AC_SRC_ALPHA;
	// pptDst 传 nullptr：位置用上面 SetWindowPos 的（游戏窗左下角）。
	// 传 &ptDst={0,0} 会被 ULW 再挪回屏幕 (0,0) —— SetWindowPos 白做。
	// **返回值必须查**：ULW 失败时窗口不显示任何内容（WS_EX_LAYERED 在首次
	// 成功前整个透明），失败不打日志就是"小窗开着但看不到"干瞪眼。
	if (!UpdateLayeredWindow(_debugHwnd, _debugDc, nullptr, &size,
			memDc, &ptSrc, 0, &blend, ULW_ALPHA)) {
		static bool warnedOnce = false;
		if (!warnedOnce) {
			warnedOnce = true;
			D5_LOG_WARN(L"语义蒙版 debug 小窗：UpdateLayeredWindow 失败"
				L" GetLastError=%u（小窗将不可见）", GetLastError());
		}
	}
	SelectObject(memDc, old);
	DeleteDC(memDc);
}

//=========================== present 线程入口 ===========================

bool SegMaskFilter::RecordFrame(ID3D12GraphicsCommandList* list,
    ID3D12Resource* source, D3D12_RESOURCE_STATES state, bool flipY) noexcept {
    if (!list || !source || !_device || !_fence || !_active.load() ||
        !_trtReady.load() || !_frameWanted.load()) return false;
    std::unique_lock lock(_captureMutex, std::try_to_lock);
    if (!lock.owns_lock() || _copyInFlight[0].load()) return false;
    const auto desc = source->GetDesc();
    const uint32_t w = uint32_t(desc.Width), h = desc.Height;
    const uint32_t bpp = BytesPerPixel(desc.Format);
    if (!w || !h || !bpp || desc.SampleDesc.Count != 1 || desc.DepthOrArraySize != 1) return false;
    const uint32_t pitch = (w * bpp + 255) & ~255u;
    if (!_readback[0] || w != _frameWidth || h != _frameHeight || desc.Format != _frameFormat) {
        ID3D12Resource* replacement = nullptr;
        D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rb{}; rb.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rb.Width = uint64_t(pitch) * h; rb.Height = 1; rb.DepthOrArraySize = 1;
        rb.MipLevels = 1; rb.SampleDesc.Count = 1; rb.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
            &rb, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&replacement)))) return false;
        if (_readback[0]) _readback[0]->Release();
        _readback[0] = replacement;
        _frameWidth = w; _frameHeight = h; _frameFormat = desc.Format; _rbPitch = pitch;
    }
    D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = source; b.Transition.StateBefore = state;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    if (state != D3D12_RESOURCE_STATE_COPY_SOURCE) list->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource = source;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource = _readback[0];
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint = {desc.Format, w, h, 1, pitch};
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    if (state != D3D12_RESOURCE_STATE_COPY_SOURCE) list->ResourceBarrier(1, &b);
    _copyFenceValue[0].store(0);
    _capturedMs = GetTickCount64();
    if (_frameFlipY != flipY) D5_LOG_INFO(L"Semantic capture: sourceFlipY=%u; inference upright, mask restored to source coordinates", flipY);
    _frameFlipY = flipY;
    _pendingList = list;
    _frameWanted.store(false);
    _copyInFlight[0].store(true, std::memory_order_release);
    return true;
}

void SegMaskFilter::NotifySubmitted(ID3D12CommandQueue* queue, UINT count,
    ID3D12CommandList* const* lists) noexcept {
    if (!queue || !lists || !_fence) return;
    std::lock_guard lock(_captureMutex);
    if (!_pendingList) return;
    for (UINT i = 0; i < count; ++i) {
        if (lists[i] != _pendingList) continue;
        const uint64_t value = ++_fenceValue;
        if (FAILED(queue->Signal(_fence, value))) {
            Fail(L"语义蒙版：提交完成 fence 失败，停止采集");
            _active.store(false); // Do not recycle resources whose completion is unknown.
            return;
        }
        _pendingList = nullptr;
        _copyFenceValue[0].store(value, std::memory_order_release);
        return;
    }
}

void SegMaskFilter::NotifyReset(ID3D12GraphicsCommandList* list) noexcept {
    std::lock_guard lock(_captureMutex);
    if (_pendingList != list) return;
    _pendingList = nullptr; // The recorded capture was discarded before submission.
    _copyInFlight[0].store(false, std::memory_order_release);
    _frameWanted.store(_active.load());
}

void SegMaskFilter::RequestFrame(ID3D12Resource* source,
    D3D12_RESOURCE_STATES state) noexcept {
    if (!source || !_device || !_queue || !_active.load() ||
        !_frameWanted.load() || _copyInFlight[0].load()) return;
    // One in-flight capture means this allocator is complete before Reset.
    if (!_copyList) {
        if (FAILED(_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&_copyAlloc))) || FAILED(_device->CreateCommandList(0,
            D3D12_COMMAND_LIST_TYPE_DIRECT, _copyAlloc, nullptr,
            IID_PPV_ARGS(&_copyList)))) return;
        _copyList->Close();
    }
    if (FAILED(_copyAlloc->Reset()) || FAILED(_copyList->Reset(_copyAlloc, nullptr))) return;
    const bool recorded = RecordFrame(_copyList, source, state);
    if (FAILED(_copyList->Close())) { NotifyReset(_copyList); return; }
    if (!recorded) return;
    ID3D12CommandList* lists[]{_copyList};
    _queue->ExecuteCommandLists(1, lists);
    NotifySubmitted(_queue, 1, lists);
}

} // namespace DXL
