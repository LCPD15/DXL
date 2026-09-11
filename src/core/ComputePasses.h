#pragma once

// 本项目的计算着色器们。目前三个 pass，共用一套根签名 / 描述符堆。
//
// 1-2) 线性 HDR <-> 0..1 的可逆往返（Encode / Decode）
// 3)   运动矢量重采样（Resample）
//
// **本项目的第一个 shader。** 起因是硬数据：在游戏的 DLSS evaluate 点上，颜色是
// tonemap 之前的线性 HDR（鬼武者实测 3~6），而 DLSSNR 内部按 1.0 = 白 工作 ——
// 回读到的输出整块钉在 0.9922（1.0 的截断值），屏幕上一片灰。DLSSNR 又没有
// "输入是 HDR" 这种开关可传（参数探针确认过），所以只能自己压、自己还原。
//
// 走 y = F(x) -> DLSSNR -> x' = F⁻¹(y') 这条路，F 会抵消掉，剩下的只有滤镜本身的
// 效果 —— 所以**不需要知道游戏真实的 tonemap 是什么**，曲线只要可逆就行。
//
// 设计上的几条：
//   · 字节码是**预编译**签进仓库的（scripts\build-shaders.cmd）。core 在别人的游戏
//     进程里跑，不该拖 d3dcompiler 也不该在游戏里跑编译器。
//   · 根签名只用**根常量 + 一张描述符表**，不建常量缓冲。参数就三个数，
//     多一个资源就多一个出错面。
//   · 描述符堆**轮转**。命令录进游戏的列表，我们不知道它什么时候执行完，
//     覆盖还在飞的描述符是经典的花屏来源。和命令槽轮转同一个道理。

#include <windows.h>
#include <d3d12.h>
#include <cstdint>

namespace DXL {

class ComputePasses {
public:
	// 内联的 Record* 用得到，所以放在 public
	enum class Pass {
		Encode, Decode, DecodeRatio, Resample, Visualize, RatioMake, RatioApply
	};

	// 和 HLSL 里的 FLAG_* 一一对应。改一处要改两处。
	static constexpr uint32_t FLAG_POINT_SAMPLE = 1u;
	static constexpr uint32_t FLAG_MOTION = 2u;
	static constexpr uint32_t FLAG_DIFF = 4u;
	static constexpr uint32_t FLAG_RAW = 8u;
	static constexpr uint32_t FLAG_STRETCH = 16u;
	static constexpr uint32_t FLAG_PURE_GAMMA = 32u;
	static constexpr uint32_t FLAG_BOX_DOWN = 64u;
	static constexpr uint32_t FLAG_COLOUR_DIRECT = 128u;

	~ComputePasses();
	ComputePasses() = default;
	ComputePasses(const ComputePasses&) = delete;
	ComputePasses& operator=(const ComputePasses&) = delete;

	bool Initialize(ID3D12Device* device) noexcept;
	bool IsReady() const noexcept { return _encodePso && _decodePso; }
	bool CanResample() const noexcept { return _resamplePso != nullptr; }
	const char* LastError() const noexcept { return _lastError; }

	// 把命令录进**给定的**命令列表（通常是游戏的）。
	//
	// source 必须已经在 NON_PIXEL_SHADER_RESOURCE，target 必须已经在
	// UNORDERED_ACCESS —— 状态由调用方负责，这里一条 barrier 都不下。
	// 理由：调用方（DlssNrFilter）本来就在做一长串 transition，把状态集中在一处
	// 管才看得清；这里再插几条只会让两边都不知道资源当前在什么状态上。
	//
	// **会覆盖这条列表上的描述符堆 / 根签名 / PSO。** 调用点必须是"游戏紧接着自己
	// 也要调 NGX EvaluateFeature"的那一刻 —— 它本来就预期这些状态在那里被打乱。
	// curveFlags：0 = Hybrid + sRGB，FLAG_PURE_GAMMA = 纯幂函数。
	// 编码和还原必须传同一个 curveFlags；还原会重算 EncodeCurve 作为参考。
	bool RecordEncode(
		ID3D12GraphicsCommandList* list,
		ID3D12Resource* source, ID3D12Resource* target,
		uint32_t width, uint32_t height, float scale, float gamma,
		uint32_t curveFlags = 0) noexcept {
		return Record(list, source, target, width, height, scale, gamma,
			Pass::Encode, curveFlags);
	}
	bool RecordDecode(
		ID3D12GraphicsCommandList* list,
		ID3D12Resource* source, ID3D12Resource* target,
		uint32_t width, uint32_t height, float scale, float gamma,
		uint32_t curveFlags = 0) noexcept {
		return Record(list, source, target, width, height, scale, gamma,
			Pass::Decode, curveFlags);
	}

	// 默认还原：在线性域比较 model / encoded proxy，把模型改动映射回原图。
	// model 与 proxy 相同时保留原图，包括编码压缩过的高光。
	bool RecordDecodeRatio(
		ID3D12GraphicsCommandList* list,
		ID3D12Resource* original, ID3D12Resource* filtered,
		ID3D12Resource* target,
		uint32_t width, uint32_t height, float scale, float gamma,
		uint32_t curveFlags = 0, float colourStrength = 1.0f, float selfLayers = 1.0f) noexcept {
		return Record(list, original, target, width, height, scale, gamma,
			Pass::DecodeRatio, curveFlags, filtered, 1.0f, 0.0f, 0.0f, colourStrength, selfLayers);
	}

	// 把运动矢量重采样到 target 的分辨率（width/height 是**目标**尺寸）。
	//
	// 数值不做换算 —— 游戏的矢量在归一化 UV 空间，UV 与分辨率无关。换完分辨率之后
	// 把 MVecScale 改成新分辨率就对了。调用方负责确认这个约定成立
	// （判据：游戏给的 MVecScale 等于矢量纹理自己的尺寸）。
	bool RecordMotionResample(
		ID3D12GraphicsCommandList* list,
		ID3D12Resource* source, ID3D12Resource* target,
		uint32_t width, uint32_t height) noexcept {
		return Record(list, source, target, width, height, 1.0f, 1.0f,
			Pass::Resample);
	}

    // General linear resample, including continuous semantic strength (RGBA).
    bool RecordLinearResample(ID3D12GraphicsCommandList* list,
        ID3D12Resource* source, ID3D12Resource* target, uint32_t width, uint32_t height) noexcept {
        return Record(list, source, target, width, height, 1.0f, 1.0f, Pass::Resample);
    }

	// 深度重采样。**必须最近邻** —— 双线性会在轮廓边缘插出场景里不存在的"中间深度"，
	// disocclusion 判定会被那些假边缘骗到。矢量场是平滑的，所以那边用双线性。
	bool RecordDepthResample(
		ID3D12GraphicsCommandList* list,
		ID3D12Resource* source, ID3D12Resource* target,
		uint32_t width, uint32_t height) noexcept {
		return Record(list, source, target, width, height, 1.0f, 1.0f,
			Pass::Resample, FLAG_POINT_SAMPLE);
	}

	// 把深度或矢量画成假彩色。gain 对深度是幂曲线的指数，对矢量是偏移增益。
	//
	// **outScale 和 gain 是两件不同的事，绝不能合成一个数。** gain 参与
	// saturate 之前的运算（决定图长什么样），outScale 在 saturate 之后整体放大
	// （抵消游戏的 tonemap）。当年把它们合在一起，结果是每个像素都被 saturate 成
	// 1.0 —— 一张纯色图，而它看起来正好像我们当时怀疑的那个结论。
	bool RecordVisualize(
		ID3D12GraphicsCommandList* list,
		ID3D12Resource* source, ID3D12Resource* target,
		uint32_t width, uint32_t height, float gain, bool motion,
		float outScale = 1.0f) noexcept {
		return Record(list, source, target, width, height, gain, 1.0f,
			Pass::Visualize, motion ? FLAG_MOTION : 0u, nullptr, outScale);
	}

	// 画 |filtered - input| * gain。回答"滤镜到底改了多少" —— 全黑 = 什么都没做。
	// 这是个**能证伪**的判据，比盯着最终画面猜"好像变了一点"可靠得多。
	bool RecordVisualizeDiff(
		ID3D12GraphicsCommandList* list,
		ID3D12Resource* filtered, ID3D12Resource* input,
		ID3D12Resource* target,
		uint32_t width, uint32_t height, float gain,
		float outScale = 1.0f) noexcept {
		return Record(list, filtered, target, width, height, gain, 1.0f,
			Pass::Visualize, FLAG_DIFF, input, outScale);
	}

	// 原样显示一张彩色图（乘增益）。**别拿深度那一档来干这件事** ——
	// 深度分支会做 pow(R, Scale)，把彩色图画成灰图，看起来像"输入本身是灰的"。
	//
	// lo/hi 给出的时候按这个区间把对比度拉满 —— 编码后的图实测挤在 0.72~0.85，
	// 不拉伸的话即使一切正常也是一张看不出结构的灰图。
	bool RecordVisualizeRaw(
		ID3D12GraphicsCommandList* list,
		ID3D12Resource* source, ID3D12Resource* target,
		uint32_t width, uint32_t height, float gain,
		float outScale = 1.0f, float lo = 0.0f, float hi = 0.0f) noexcept {
		const bool stretch = hi > lo + 1e-4f;
		return Record(list, source, target, width, height, gain, 1.0f,
			Pass::Visualize, FLAG_RAW | (stretch ? FLAG_STRETCH : 0u),
			nullptr, outScale, lo, hi);
	}

	// **盒式降采样。** width/height 是目标（低）分辨率，srcOverDst 是源/目标的比例
	// （比如缩到 60% 时传 1/0.6 = 1.667）。双线性只混 2x2，非整数比例会漏采样、
	// 把高频折叠成摩尔纹 —— 而这张图是要喂给 DLSSNR 的，输入质量决定上限。
	bool RecordBoxDownsample(
		ID3D12GraphicsCommandList* list,
		ID3D12Resource* source, ID3D12Resource* target,
		uint32_t width, uint32_t height, float srcOverDst) noexcept {
		return Record(list, source, target, width, height, srcOverDst, 1.0f,
			Pass::Resample, FLAG_BOX_DOWN);
	}

	// 低分辨率的"改动量"做成比值图。filtered / input 都是低分辨率。
	bool RecordRatioMake(
		ID3D12GraphicsCommandList* list,
		ID3D12Resource* filtered, ID3D12Resource* input,
		ID3D12Resource* target,
		uint32_t width, uint32_t height) noexcept {
		return Record(list, filtered, target, width, height, 1.0f, 1.0f,
			Pass::RatioMake, 0, input);
	}

	// 全分辨率原图 x 放大后的比值。width/height 是**全**分辨率。
	bool RecordRatioApply(
		ID3D12GraphicsCommandList* list,
		ID3D12Resource* original, ID3D12Resource* ratio,
		ID3D12Resource* target,
		uint32_t width, uint32_t height, float colourStrength = 1.0f, float selfLayers = 1.0f) noexcept {
		return Record(list, original, target, width, height, 1.0f, 1.0f,
			Pass::RatioApply, 0, ratio, 1.0f, 0.0f, 0.0f, colourStrength, selfLayers);
	}
	bool RecordColourBlend(ID3D12GraphicsCommandList* list,
		ID3D12Resource* original, ID3D12Resource* filtered, ID3D12Resource* target,
		uint32_t width, uint32_t height, float colourStrength, float selfLayers = 1.0f) noexcept {
		return Record(list, original, target, width, height, 1.0f, 1.0f,
			Pass::RatioApply, FLAG_COLOUR_DIRECT, filtered, 1.0f, 0.0f, 0.0f, colourStrength, selfLayers);
	}

	bool CanVisualize() const noexcept { return _visualizePso != nullptr; }
	// 降分辨率那条路能不能走。**不能走时调用方必须退回全分辨率，不许静默降级** ——
	// 少了 RatioApply 就没人把结果写回全分辨率，画面会是一块低分辨率的图。
	bool CanScaledPath() const noexcept {
		return _ratioMakePso && _ratioApplyPso && _resamplePso;
	}
	bool CanDecodeRatio() const noexcept { return _decodeRatioPso != nullptr; }

private:
	// 轮转的描述符堆数量。
	//
	// **不能只有 4 个。** 一帧里可能连着录 2~3 个 pass（编码 / 解码 / 可视化），
	// 而命令列表是**游戏**提交的 —— 它手上可能压着两三帧没执行。4 个堆只够一帧半，
	// 后一帧会把前一帧还在被 GPU 读的描述符覆盖掉，表现是偶发花屏，而且**不会报错**
	// （调试层管不到"描述符被覆盖"这种时序问题）。16 个给足余量，一个堆两个描述符，
	// 内存代价可以忽略。
	// Evaluate mode now admits only one GPU-in-flight list through EvaluateGpuGate;
	// the ring is sufficient for the passes within that list, not a fence substitute.
	static constexpr uint32_t HEAP_COUNT = 16;
	// 和 HLSL 里的 numthreads 必须一致
	static constexpr uint32_t THREADS = 8;

	bool Record(
		ID3D12GraphicsCommandList* list,
		ID3D12Resource* source, ID3D12Resource* target,
		uint32_t width, uint32_t height, float scale, float gamma, Pass pass,
		uint32_t flags = 0, ID3D12Resource* source2 = nullptr,
		float postScale = 1.0f, float stretchLo = 0.0f,
		float stretchHi = 0.0f, float colourStrength = 1.0f, float selfLayers = 1.0f) noexcept;

	ID3D12Device* _device = nullptr;
	ID3D12RootSignature* _rootSignature = nullptr;
	ID3D12PipelineState* _encodePso = nullptr;
	ID3D12PipelineState* _decodePso = nullptr;
	ID3D12PipelineState* _decodeRatioPso = nullptr;
	ID3D12PipelineState* _resamplePso = nullptr;
	ID3D12PipelineState* _visualizePso = nullptr;
	ID3D12PipelineState* _ratioMakePso = nullptr;
	ID3D12PipelineState* _ratioApplyPso = nullptr;
	// 每个槽位三个描述符：t0 / t1 的 SRV 和 u0 的 UAV，顺序必须和根签名里的表一致。
	// t1 只有"按比例还原"用得到，别的 pass 那一格填成和 t0 一样（不读就无所谓）。
	ID3D12DescriptorHeap* _heaps[HEAP_COUNT]{};
	uint32_t _nextHeap = 0;
	uint32_t _descriptorStride = 0;
	const char* _lastError = "";
};

}  // namespace DXL
