#include "ComputePasses.h"
#include "NrColorFormat.h"

#include <algorithm>
#include <cmath>

#include "../common/Log.h"
#include "shaders/precompiled/ToneEncode.h"
#include "shaders/precompiled/ToneDecode.h"
#include "shaders/precompiled/ToneDecodeRatio.h"
#include "shaders/precompiled/MotionResample.h"
#include "shaders/precompiled/Visualize.h"
#include "shaders/precompiled/RatioMake.h"
#include "shaders/precompiled/RatioApply.h"

namespace DXL {

namespace {

// 和 HLSL 里的 cbuffer Params 一一对应。改一处必须改两处。
// 顺序必须和 HLSL 里的 cbuffer 一字不差 —— 根常量是按 DWORD 下标写进去的，
// 错一位就是静默读到别的字段。
struct Params {
	float scale;
	float gamma;
	uint32_t width;
	uint32_t height;
	uint32_t flags;
	// 可视化写出去之前再乘的倍数（saturate 之后才乘 —— 顺序反了会得到一张纯色图）
	float postScale;
	float stretchLo;
	float stretchHi;
	float colourStrength;
	float selfLayers;
};
static_assert(sizeof(Params) % 4 == 0, "根常量按 4 字节一个 DWORD 传");
constexpr uint32_t PARAM_DWORDS = sizeof(Params) / 4;

// 把资源格式映射成能当 SRV 采样的格式。UNKNOWN = 做不了。
//
// 深度这一路是必须的：旁听抄到的深度副本用的是游戏的原格式，而深度缓冲基本都是
// typeless 或 D* 格式 —— 那两类都建不出 SRV。
DXGI_FORMAT SrvFormatFor(DXGI_FORMAT format) noexcept {
	switch (format) {
	// Match DlssNrFilter's working texture format without changing the
	// borrowed resource or applying an extra sRGB conversion in the sampler.
	case DXGI_FORMAT_R8G8B8A8_TYPELESS:
	case DXGI_FORMAT_B8G8R8A8_TYPELESS:
		return NrColorViewFormat(format);
	// 深度 / 深度+模板。取深度那一半。
	case DXGI_FORMAT_R32G8X24_TYPELESS:
	case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
		return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
	case DXGI_FORMAT_R24G8_TYPELESS:
	case DXGI_FORMAT_D24_UNORM_S8_UINT:
		return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
	case DXGI_FORMAT_R32_TYPELESS:
	case DXGI_FORMAT_D32_FLOAT:
		return DXGI_FORMAT_R32_FLOAT;
	case DXGI_FORMAT_R16_TYPELESS:
	case DXGI_FORMAT_D16_UNORM:
		return DXGI_FORMAT_R16_UNORM;
	// 颜色 / 矢量：本来就能采样，原样用
	case DXGI_FORMAT_R11G11B10_FLOAT:
	case DXGI_FORMAT_R16G16_FLOAT:
	case DXGI_FORMAT_R16G16_SNORM:
	case DXGI_FORMAT_R16G16B16A16_FLOAT:
	case DXGI_FORMAT_R32G32_FLOAT:
	case DXGI_FORMAT_R8G8B8A8_UNORM:
	case DXGI_FORMAT_B8G8R8A8_UNORM:
	case DXGI_FORMAT_R10G10B10A2_UNORM:
	case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
	case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
	case DXGI_FORMAT_R16_UNORM:
	case DXGI_FORMAT_R32_FLOAT:
		return format;
	default:
		return DXGI_FORMAT_UNKNOWN;
	}
}

template <typename T>
void SafeRelease(T*& ptr) noexcept {
	if (ptr) {
		ptr->Release();
		ptr = nullptr;
	}
}

}  // namespace

ComputePasses::~ComputePasses() {
	for (ID3D12DescriptorHeap*& heap : _heaps) SafeRelease(heap);
	SafeRelease(_encodePso);
	SafeRelease(_decodePso);
	SafeRelease(_decodeRatioPso);
	SafeRelease(_resamplePso);
	SafeRelease(_visualizePso);
	SafeRelease(_ratioMakePso);
	SafeRelease(_ratioApplyPso);
	SafeRelease(_rootSignature);
}

bool ComputePasses::Initialize(ID3D12Device* device) noexcept {
	if (IsReady()) return true;
	if (!device) {
		_lastError = "没有 device";
		return false;
	}
	_device = device;

	// 根签名：一组根常量（b0）+ 一张表（t0 的 SRV、u0 的 UAV）。
	// 不建常量缓冲 —— 三个数走根常量就够，少一个资源少一个出错面。
	D3D12_DESCRIPTOR_RANGE ranges[2]{};
	ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	// t0 = 主输入，t1 = 第二输入（只有"按比例还原"读它）
	ranges[0].NumDescriptors = 2;
	ranges[0].BaseShaderRegister = 0;   // t0
	ranges[0].OffsetInDescriptorsFromTableStart = 0;
	ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
	ranges[1].NumDescriptors = 1;
	ranges[1].BaseShaderRegister = 0;   // u0
	ranges[1].OffsetInDescriptorsFromTableStart = 2;

	D3D12_ROOT_PARAMETER params[2]{};
	params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	params[0].Constants.ShaderRegister = 0;   // b0
	params[0].Constants.RegisterSpace = 0;
	params[0].Constants.Num32BitValues = PARAM_DWORDS;
	params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[1].DescriptorTable.NumDescriptorRanges = 2;
	params[1].DescriptorTable.pDescriptorRanges = ranges;
	params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

	D3D12_ROOT_SIGNATURE_DESC rootDesc{};
	rootDesc.NumParameters = 2;
	rootDesc.pParameters = params;
	// 静态采样器：声明在根签名里就够，不占描述符堆。只有重采样 pass 用。
	// s0 = 双线性（矢量、可视化），s1 = 最近邻（深度）。都是 clamp。
	// **ComparisonFunc 必须显式给** —— 零初始化出来的 0 不是合法的枚举值，
	// 序列化会直接失败。
	D3D12_STATIC_SAMPLER_DESC samplers[2]{};
	for (uint32_t i = 0; i < 2; ++i) {
		samplers[i].Filter = i == 0 ? D3D12_FILTER_MIN_MAG_MIP_LINEAR
			: D3D12_FILTER_MIN_MAG_MIP_POINT;
		samplers[i].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		samplers[i].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		samplers[i].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		samplers[i].ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
		samplers[i].MaxLOD = D3D12_FLOAT32_MAX;
		samplers[i].ShaderRegister = i;
		samplers[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	}
	rootDesc.NumStaticSamplers = 2;
	rootDesc.pStaticSamplers = samplers;
	// 计算管线不需要输入布局
	rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

	ID3DBlob* blob = nullptr;
	ID3DBlob* error = nullptr;
	HRESULT hr = D3D12SerializeRootSignature(
		&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error);
	if (FAILED(hr)) {
		D5_LOG_ERROR(L"色调往返：根签名序列化失败 0x%08X（%hs）", hr,
			error ? (const char*)error->GetBufferPointer() : "");
		if (blob) blob->Release();
		if (error) error->Release();
		_lastError = "根签名序列化失败";
		return false;
	}
	hr = _device->CreateRootSignature(0, blob->GetBufferPointer(),
		blob->GetBufferSize(), IID_PPV_ARGS(&_rootSignature));
	blob->Release();
	if (error) error->Release();
	if (FAILED(hr)) {
		D5_LOG_ERROR(L"色调往返：CreateRootSignature 失败 0x%08X", hr);
		_lastError = "CreateRootSignature 失败";
		return false;
	}

	auto makePso = [&](const void* bytecode, size_t size,
		ID3D12PipelineState** out, const wchar_t* name) {
		D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
		desc.pRootSignature = _rootSignature;
		desc.CS.pShaderBytecode = bytecode;
		desc.CS.BytecodeLength = size;
		const HRESULT result =
			_device->CreateComputePipelineState(&desc, IID_PPV_ARGS(out));
		if (FAILED(result)) {
			D5_LOG_ERROR(L"色调往返：PSO %s 创建失败 0x%08X", name, result);
			return false;
		}
		(*out)->SetName(name);
		return true;
	};
	// 重采样建不出来不算致命 —— 它和色调往返服务的是两条不同的路
	if (!makePso(g_motionResampleCs, sizeof(g_motionResampleCs), &_resamplePso,
			L"D5Q.Resample")) {
		D5_LOG_WARN(L"计算 pass：重采样 PSO 建不出来，"
			L"backbuffer 那条路只能继续用零矢量/零深度");
	}
	if (!makePso(g_visualizeCs, sizeof(g_visualizeCs), &_visualizePso,
			L"D5Q.Visualize")) {
		D5_LOG_WARN(L"计算 pass：可视化 PSO 建不出来，debug 视图不可用");
	}
	// 降分辨率跑 DLSSNR 那条路要这两个。建不出来就只能全分辨率跑（更贵但正确），
	// 调用方靠 CanScaledPath() 判断，不会静默降级成错的画面。
	if (!makePso(g_ratioMakeCs, sizeof(g_ratioMakeCs), &_ratioMakePso,
			L"D5Q.RatioMake") ||
		!makePso(g_ratioApplyCs, sizeof(g_ratioApplyCs), &_ratioApplyPso,
			L"D5Q.RatioApply")) {
		D5_LOG_WARN(L"计算 pass：比值放大 PSO 建不出来，"
			L"降分辨率跑 DLSSNR 那条路不可用（会退回全分辨率）");
	}
	if (!makePso(g_toneDecodeRatioCs, sizeof(g_toneDecodeRatioCs),
			&_decodeRatioPso, L"D5Q.Tone.DecodeRatio")) {
		D5_LOG_WARN(L"计算 pass：按比例还原 PSO 建不出来，会退回逆变换曲线"
			L"（放大倍数从 1 变成 4 起，闪烁会更明显）");
	}
	if (!makePso(g_toneEncodeCs, sizeof(g_toneEncodeCs), &_encodePso,
			L"D5Q.Tone.Encode") ||
		!makePso(g_toneDecodeCs, sizeof(g_toneDecodeCs), &_decodePso,
			L"D5Q.Tone.Decode")) {
		_lastError = "计算 PSO 创建失败";
		SafeRelease(_encodePso);
		SafeRelease(_decodePso);
	SafeRelease(_decodeRatioPso);
	SafeRelease(_resamplePso);
	SafeRelease(_visualizePso);
		return false;
	}

	// 描述符堆必须是 shader-visible（表里的句柄要给 GPU 用）
	D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
	heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	heapDesc.NumDescriptors = 3;
	heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	for (ID3D12DescriptorHeap*& heap : _heaps) {
		if (FAILED(_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&heap)))) {
			D5_LOG_ERROR(L"色调往返：描述符堆创建失败");
			_lastError = "描述符堆创建失败";
			return false;
		}
		// 起个名字：设备挂掉时 DRED 的页错误输出只认得有名字的对象，
		// 无名的那些只会打成 "(无名)" —— 那就等于没查到。
		heap->SetName(L"D5Q.ComputePasses.Heap");
	}
	_descriptorStride = _device->GetDescriptorHandleIncrementSize(
		D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	D5_LOG_INFO(L"计算 pass 就绪：色调往返 + 重采样 + 可视化（%u 个轮转描述符堆）",
		HEAP_COUNT);
	return true;
}

bool ComputePasses::Record(
	ID3D12GraphicsCommandList* list,
	ID3D12Resource* source, ID3D12Resource* target,
	uint32_t width, uint32_t height, float scale, float gamma, Pass pass,
	uint32_t flags, ID3D12Resource* source2,
	float postScale, float stretchLo, float stretchHi, float colourStrength, float selfLayers) noexcept {
	if (!list || !source || !target || !width || !height) return false;
	ID3D12PipelineState* pso =
		pass == Pass::Encode ? _encodePso :
		pass == Pass::Decode ? _decodePso :
		pass == Pass::DecodeRatio ? _decodeRatioPso :
		pass == Pass::Visualize ? _visualizePso :
		pass == Pass::RatioMake ? _ratioMakePso :
		pass == Pass::RatioApply ? _ratioApplyPso : _resamplePso;
	if (!pso || !_rootSignature) return false;

	// **轮转堆**：这条命令列表由游戏提交，我们不知道它什么时候跑完。
	// 每次都写同一个堆的话，还在飞的那一帧的描述符会被下一帧覆盖。
	ID3D12DescriptorHeap* heap = _heaps[_nextHeap % HEAP_COUNT];
	++_nextHeap;

	D3D12_CPU_DESCRIPTOR_HANDLE cpu = heap->GetCPUDescriptorHandleForHeapStart();
	D3D12_GPU_DESCRIPTOR_HANDLE gpu = heap->GetGPUDescriptorHandleForHeapStart();

	// 视图的格式**显式跟着资源写**，不传 nullptr 让它自己推 —— 我们处理的是
	// R11G11B10_FLOAT 这种没有 typeless 亲戚的格式，推错了就是静默读到垃圾。
	// **SRV 的格式不能照抄资源的格式。** 深度缓冲往往是 typeless（旁听抄到的那张是
	// R32G8X24_TYPELESS，fmt=19），而 typeless 建不出 SRV；D32_FLOAT 这类深度格式
	// 也不在 SRV 允许的列表里。必须映射成同族的"可采样"格式。
	// 映射不出来就不做 —— 拿错格式采样是静默读到垃圾，比不做更糟。
	const DXGI_FORMAT viewFormat = SrvFormatFor(source->GetDesc().Format);
	if (viewFormat == DXGI_FORMAT_UNKNOWN) {
		static uint32_t complained = 0;
		if (complained++ < 4) {
			D5_LOG_WARN(L"计算 pass：格式 %u 没法当 SRV 采样（typeless 或深度格式），"
				L"这一趟跳过", (unsigned)source->GetDesc().Format);
		}
		return false;
	}
	D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
	srv.Format = viewFormat;
	srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srv.Texture2D.MipLevels = 1;
	_device->CreateShaderResourceView(source, &srv, cpu);

	// t1：第二输入。没有就填成和 t0 一样 —— 描述符表里每一格都必须是有效的，
	// 哪怕那个 pass 根本不读它（不填的话驱动会读到未初始化的描述符）。
	D3D12_CPU_DESCRIPTOR_HANDLE srv2Handle = cpu;
	srv2Handle.ptr += _descriptorStride;
	{
		ID3D12Resource* const second = source2 ? source2 : source;
		D3D12_SHADER_RESOURCE_VIEW_DESC srv2 = srv;
		srv2.Format = SrvFormatFor(second->GetDesc().Format);
		if (srv2.Format == DXGI_FORMAT_UNKNOWN) return false;
		_device->CreateShaderResourceView(second, &srv2, srv2Handle);
	}

	D3D12_CPU_DESCRIPTOR_HANDLE uavHandle = cpu;
	uavHandle.ptr += _descriptorStride * 2;
	D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
	uav.Format = target->GetDesc().Format;
	uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
	_device->CreateUnorderedAccessView(target, nullptr, &uav, uavHandle);

	ID3D12DescriptorHeap* heaps[]{ heap };
	list->SetDescriptorHeaps(1, heaps);
	list->SetComputeRootSignature(_rootSignature);
	list->SetPipelineState(pso);

	Params constants{};
	constants.scale = scale > 0.0f ? scale : 1.0f;
	constants.gamma = gamma;
	constants.width = width;
	constants.height = height;
	constants.flags = flags;
	constants.postScale = postScale > 0.0f ? postScale : 1.0f;
	constants.stretchLo = stretchLo;
	constants.stretchHi = stretchHi;
	constants.colourStrength = colourStrength;
	constants.selfLayers = std::isfinite(selfLayers) ? std::clamp(selfLayers, 1.0f, 3.0f) : 1.0f;
	// **把真正送进 shader 的常量打出来。** "参数没生效"和"参数生效了但值不对"
	// 在画面上分不开 —— 实测白点 8.0 和 2.0 出来的图几乎一样暗，差 4 倍的参数
	// 看不出区别，那就该怀疑它根本没到 shader，而不是继续调数值。
	{
		// 只在**值变了**的时候打。按次数限流会漏掉"用户改了设置之后"的那一次，
		// 而那恰恰是最想看的一次。
		static float lastScale = -1.0f;
		static float lastGamma = -1.0f;
		static float lastPost = -1.0f;
		static uint32_t lastFlags = 0xFFFFFFFFu;
		if (constants.scale != lastScale || constants.gamma != lastGamma ||
			constants.postScale != lastPost || constants.flags != lastFlags) {
			lastScale = constants.scale;
			lastGamma = constants.gamma;
			lastPost = constants.postScale;
			lastFlags = constants.flags;
			D5_LOG_INFO(L"计算 pass[%u]：白点=%.3f gamma=%.3f 写出倍数=%.3f "
				L"拉伸[%.3f..%.3f] %ux%u flags=0x%X",
				(unsigned)pass, constants.scale, constants.gamma,
				constants.postScale, constants.stretchLo, constants.stretchHi,
				constants.width, constants.height, constants.flags);
		}
	}
	list->SetComputeRoot32BitConstants(0, PARAM_DWORDS, &constants, 0);
	list->SetComputeRootDescriptorTable(1, gpu);

	list->Dispatch((width + THREADS - 1) / THREADS,
		(height + THREADS - 1) / THREADS, 1);
	return true;
}

}  // namespace DXL
