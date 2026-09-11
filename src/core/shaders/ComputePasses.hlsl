// Shared D3D12 compute passes. Build with build/build_shaders.ps1.
// Evaluate inputs are scene-linear: encode with Hybrid+sRGB (or the legacy
// clamp+power curve), run NR, and transport only its linear residual back to
// the original. Present inputs already contain the game's display image.
// Resource transitions and lifetime are managed by the caller.

Texture2D<float4> Source : register(t0);
// 第二路输入。只有"按比例还原"那个 pass 用得到（它要同时看原件和滤镜输出）。
Texture2D<float4> Source2 : register(t1);
RWTexture2D<float4> Target : register(u0);
// 静态采样器：声明在根签名里，不占描述符堆的位置。
SamplerState LinearClamp : register(s0);
SamplerState PointClamp : register(s1);

cbuffer Params : register(b0)
{
	// 各 pass 复用。色调往返里它是**白点**：多少线性数值算作漫反射白（要除以它）。
	// 重采样里不用；可视化里是增益（矢量）或伽马指数（深度）。
	float Scale;
	// 压缩之后再过的 gamma（1/Gamma 次幂）。2.2 ≈ 让信号看起来像一张正常的 SDR 图。
	//
	// **换成"按比例搬运"之后这条曲线是免费的**：它只决定滤镜*看到*什么，
	// 不影响还原精度（y' = y 时 x' = x 与曲线无关）。所以该挑的标准从"好不好逆"
	// 变成了"像不像一张照片" —— 而 DLSSNR 是按最终画面设计的滤镜。
	// 没有 gamma 时暗部基本是线性的（y ≈ kx），整个中低调挤在很窄一段里，
	// 滤镜看到一张低对比的图，自然觉得没什么可做（实测：效果几乎看不出来）。
	float Gamma;
	uint Width;
	uint Height;
	uint Flags;
	// **可视化写出去之前再乘的倍数。必须和上面的 Scale 分开。**
	//
	// debug 图要写进游戏 tonemap **之前**的 HDR 缓冲，所以得放大才不会被 tonemap
	// 压成一团。原来把这个倍数并进 Scale 里 —— 而 Scale 是在 saturate **之前**参与
	// 运算的，于是 0.79 的编码值乘 19.57 得 15.5，saturate 之后变成 **1.0，每个像素
	// 都一样**。"压缩后的输入"那张图从加上 tonemap 补偿那天起就是一张纯色图，
	// 而它看起来正好像"输入一片灰"这个我们本来就在怀疑的结论 —— 最坏的一种假证据。
	//
	// 补偿必须在 saturate **之后**：显示值先夹到 0..1，再整体放大。
	float PostScale;
	// 可视化的对比拉伸区间（仅 FLAG_STRETCH）。编码图的值挤在 0.7~0.9 这种窄带里时，
	// 直接显示看不出结构；按实测的 lo/hi 拉满 0..1 才能看出"喂进去的到底是什么"。
	float StretchLo;
	float StretchHi;
	float ColourStrength;
	float SelfLayers;
};

// Same composition principle as OptiScaler NR's colour-strength control:
// blend original chroma carrying the result's luminance with the existing RGB
// result. Keep strength 1 as a direct return, preserving the previous pipeline.
float3 RestoreColour(float3 original, float3 result)
{
	if (ColourStrength >= 1.0f) return result;
	const float3 weights = float3(0.2126f, 0.7152f, 0.0722f);
	original = max(original, 0.0f);
	const float oldL = dot(original, weights);
	const float newL = max(dot(result, weights), 0.0f);
	// Black has no chroma. Preserve it; avoid amplifying noise near zero.
	const float3 nativeColour = oldL > 1e-6f ? original * (newL / oldL) : original;
	return lerp(nativeColour, result, saturate(ColourStrength));
}

// Repeat the already-computed change, after colour restoration. This is a
// spatial residual gain, not another neural evaluation. One layer is the old
// result exactly; HDR is not clamped to SDR white and alpha is handled separately.
float3 ComposeLayers(float3 original, float3 result)
{
	result = RestoreColour(original, result);
	if (SelfLayers <= 1.0f) return result;
	return max(original + (result - original) * min(SelfLayers, 3.0f), 0.0f);
}

// Flags 的位。C++ 侧有同名常量，改一处要改两处。
static const uint FLAG_POINT_SAMPLE = 1u;   // 重采样用最近邻而不是双线性
static const uint FLAG_MOTION = 2u;         // 可视化按"运动矢量"解读而不是"深度"
// 差值放大：画 |filtered - encoded| * Scale。用来回答"滤镜到底改了多少"——
// 肉眼在最终画面上看不出小改动，但差值图能。
static const uint FLAG_DIFF = 4u;
// 原样显示：直接把源当彩色图画出来（乘一个增益）。
// **必须有这么一档** —— 不加的话就只能走深度分支，那条会把 R 通道当深度做 pow，
// 画出来是一张灰图。图 1/2/3 那三张"喂进去一片灰"就是这么来的：
// 不是输入真的灰，是可视化把它变灰了。**诊断工具自己撒谎最难查。**
static const uint FLAG_RAW = 8u;
// 显示前按 [StretchLo, StretchHi] 拉满 0..1。窄带信号（比如编码后的 0.7~0.9）
// 不拉伸就是一张看不出结构的灰图。
static const uint FLAG_STRETCH = 16u;
// 编码曲线不走 Reinhard，只走纯幂函数（clamp 到白点）。
//
// **按比例还原让曲线变成自由参数**（y'=y 时 x'=x 与曲线无关），所以该按"保留多少
// 相对对比度"来挑，而不是"好不好逆"。Reinhard 在归一值 n 处保留 1/(1+n)，
// n=1.53（白点校准到 0.6 那一点）时只剩 39%，再乘 gamma 的 1/2.2 就只有 18%。
// 纯幂函数在**任何**亮度上都保留 1/Gamma = 45%，且没有随亮度变化的挤压。
// 代价是高于白点的部分被裁平 —— 那些区域比值还原会得到 ~1，也就是原样不动，
// 不会出错，只是不被滤镜处理。
static const uint FLAG_PURE_GAMMA = 32u;
// 降采样时按**目标像素覆盖的源区域**取平均（盒式），不是只做一次双线性。
//
// 为什么必须有：双线性只混 2x2 个源像素。缩到 50% 时那正好等于正确的footprint，
// 但缩到 70% 这种非整数比例就漏采样 —— 高频会折叠成摩尔纹，而那张图正是要喂给
// DLSSNR 的。滤镜的输入质量决定上限，这里省不得。
static const uint FLAG_BOX_DOWN = 64u;

// 逆变换在 y -> 1 时会炸。滤镜完全可能输出正好 1.0（我们就是因为它饱和在 1.0 才要
// 做这件事的），所以必须夹住。1 - 1/8192 对应还原出 8191 —— 远超任何真实场景亮度，
// 又不会变成 inf 污染整张图。
static const float MAX_ENCODED = 1.0f - 1.0f / 8192.0f;

// sRGB 编码（标准 sRGB 分段曲线）。DLSSNR 训练时吃的是 SDR 游戏缓冲携带的 sRGB，
// 所以要用 sRGB 而不是 Reinhard（OptiScaler 注释明确 "sRGB rather than a plain 2.2
// power: it is what an SDR game buffer actually carries, and the model was trained on
// those"）。Reinhard 暗部线性不提升（黑的地方很黑）、高亮饱和（亮的地方还是曝）；
// sRGB 暗部 12.92 倍提升、高亮用幂函数保留相对差异，正好修掉这两个症状。
float3 LinearToSrgb(float3 v)
{
	v = saturate(v);
	return lerp(v * 12.92,
		1.055 * pow(max(v, 1e-8), 1.0 / 2.4) - 0.055,
		step(0.0031308, v));
}

// sRGB 逆（给严格逆那条回退路径用）。
float3 SrgbToLinear(float3 v)
{
	v = saturate(v);
	return lerp(v / 12.92, pow((v + 0.055) / 1.055, 2.4), step(0.04045, v));
}

// sRGB 逆 + 高光线性外推。模型画面经亮度修正（含高光 headroom）后会超过 1，
// sRGB 只定义 [0,1]，超出的部分按 1.0 处的斜率线性延续，而不是 clip 掉高光。
// sRGB 逆在 v=1 处的斜率 ≈ 2.4/1.055 ≈ 2.275。
float3 SrgbToLinearExt(float3 v)
{
	const float3 lo = SrgbToLinear(min(v, 1.0f));
	const float3 hi = max(v - 1.0f, 0.0f) * 2.275f;
	return lo + hi;
}

// 软膝：保高光肩部。亮度 > 0.75 时 roll-off（指数渐近，不像 Reinhard 那样无限逼近
// 1 把高光挤成一条细带）；peak > 1 时按 peak 缩放（单标量，保 hue，避免单通道被
// saturate 裁掉导致 hue 旋转）。对齐 OptiScaler SoftKnee。
float3 SoftKnee(float3 display)
{
	const float displayLuma = dot(display, float3(0.2126, 0.7152, 0.0722));
	if (displayLuma > 0.75)
	{
		const float rolled = 0.75 + 0.25 * (1.0 - exp(-(displayLuma - 0.75) / 0.25));
		display *= rolled / displayLuma;
	}
	const float peak = max(display.r, max(display.g, display.b));
	if (peak > 1.0)
		display /= peak;
	return display;
}

// Hybrid（对齐 OptiScaler HybridCurve/HybridEncode，ReversibleMode=3）：soft knee 的
// exp roll-off 会把高光挤成 near-white 一条细带（OptiScaler 注释自己承认 scene 2 和
// scene 4 只差 0.001）；Neutwo 修高光但压缩中调。Hybrid 是两全：中调 identity（k 以下
// 原样），高光用 Neutwo 渐近到 1 但从不 clip，保留 gradation。这就是"亮的地方还是曝"
// 的正解 —— 不是白点问题，是 soft knee 的高光压缩缺陷。
float HybridCurve(float m)
{
	const float k = 0.75;   // 膝点：以下 identity，以上无 clip 的柔和滚降
	if (m <= k)
		return m;
	const float e = (m - k) / (1.0 - k);   // 超出膝点的部分，[0, inf)
	return k + (1.0 - k) * (e * rsqrt(e * e + 1.0));
}

float3 HybridEncode(float3 v)
{
	v = max(v, 0.0);
	const float m = max(v.r, max(v.g, v.b));
	if (m <= 1e-6)
		return v;
	// peak 通道取单标量保 hue；膝点以下标量=1（identity），以上 peak 落在 HybridCurve(m)<1。
	return v * (HybridCurve(m) / m);
}

// 压缩曲线抽出来，编码和"按比例还原"都要用同一条 —— 两处写各自一份迟早会漂。
float3 EncodeCurve(float3 linearColor)
{
	// **Scale 是"多少数值算白"，所以要除不是乘。**
	//
	// 这就是「纸白 203」那个旋钮的意思：填"这游戏里多少数值等于漫反射白"，
	// 除以它，白就落到 1.0。
	const float3 normalized = max(linearColor, 0.0f) / max(Scale, 1e-6f);
	// 纯幂函数那一档：不加饱和分母，直接夹到 1.0 再过 gamma。
	// 各亮度上保留同样比例的相对对比度，见 FLAG_PURE_GAMMA 的说明。
	if (Flags & FLAG_PURE_GAMMA)
	{
		const float3 clamped = min(normalized, 1.0f);
		return Gamma > 1.001f ? pow(clamped, 1.0f / Gamma) : clamped;
	}
	// 默认：hybrid（中调 identity + 高光 Neutwo 保 gradation）+ sRGB 编码。
	// 对齐 OptiScaler ReversibleMode=3 / DLSSNR 训练契约。
	return LinearToSrgb(HybridEncode(normalized));
}

[numthreads(8, 8, 1)]
void EncodeMain(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= Width || id.y >= Height) return;
	float4 color = Source.Load(int3(id.xy, 0));
	Target[id.xy] = float4(EncodeCurve(color.rgb), color.a);
}

// Decode model and proxy into the SAME linear domain before comparing them.
// The old headroom calculation subtracted sRGB proxy luma from scene-linear
// luma, then decoded that sum a second time. Even an unchanged NR output could
// turn scene grey 0.5 into 0.818 at white point 0.25.
float3 DecodeProxyTransfer(float3 encoded)
{
	if (Flags & FLAG_PURE_GAMMA)
		return Gamma > 1.001f ? pow(max(encoded, 0.0f), Gamma) : max(encoded, 0.0f);
	return SrgbToLinearExt(max(encoded, 0.0f));
}

// Anchor the reconstruction to the original, adding only the model's linear
// residual. Unchanged model == unchanged image, including compressed highlights.
// Expand by the encoder's local compression factor, NOT the derivative of its
// near-saturated inverse curve. This avoids amplifying tiny NR changes at white.
[numthreads(8, 8, 1)]
void DecodeRatioMain(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= Width || id.y >= Height) return;
	const float4 o = Source.Load(int3(id.xy, 0));
	const float3 original = max(o.rgb, 0.0f);
	const float3 proxy = DecodeProxyTransfer(EncodeCurve(original));
	const float3 model = DecodeProxyTransfer(Source2.Load(int3(id.xy, 0)).rgb);
	float3 expansion = max(Scale, 1e-6f);
	if (Flags & FLAG_PURE_GAMMA)
	{
		// This legacy encoder clips channels separately, so restore their
		// original headroom separately as well.
		expansion = lerp(expansion, original / max(proxy, 1e-6f), step(1e-6f, proxy));
	}
	else
	{
		// Hybrid applies one peak-based scale to RGB; keep that hue-preserving
		// scale when transporting the model's colour and structure residual.
		const float peak = max(proxy.r, max(proxy.g, proxy.b));
		if (peak > 1e-6f) expansion = max(original.r, max(original.g, original.b)) / peak;
	}
	const float3 result = max(original + (model - proxy) * expansion, 0.0f);
	Target[id.xy] = float4(ComposeLayers(original, result), o.a);
}

[numthreads(8, 8, 1)]
void DecodeMain(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= Width || id.y >= Height) return;
	float4 color = Source.Load(int3(id.xy, 0));
	// EncodeCurve 的（近似）严格逆。sRGB 逆之后乘回白点（编码那边是除）。
	// soft knee 的 peak 缩放丢信息，这里只近似 —— 这是回退路径，默认走 DecodeRatio
	// （按比例还原，恒等且与曲线无关）。
	float3 y = clamp(color.rgb, 0.0f, MAX_ENCODED);
	if (Flags & FLAG_PURE_GAMMA)
	{
		float3 c = Gamma > 1.001f ? pow(y, Gamma) : y;
		c = clamp(c, 0.0f, MAX_ENCODED);
		Target[id.xy] = float4(c * max(Scale, 1e-6f), color.a);
		return;
	}
	float3 normalized = SrgbToLinear(y);
	Target[id.xy] = float4(normalized * max(Scale, 1e-6f), color.a);
}

// 把一张图重采样到另一个分辨率。
//
// **矢量的数值不用换。** 游戏的矢量在归一化 UV 空间（实测鬼武者 MVecScale =
// (1286, 724) 正好等于渲染分辨率，这就是 UV 约定的标志），而 UV 是与分辨率无关的 ——
// 所以只做空间上的缩放，值原样搬。换分辨率之后把 MVecScale 改成新分辨率就对了。
//
// **深度必须走最近邻。** 深度在轮廓边缘是不连续的，双线性会在前景和背景之间插出
// 一个"中间深度"，那是场景里根本不存在的表面 —— disocclusion 判定会被这些假边缘骗到。
// 矢量场是平滑的，双线性没这个问题。所以同一个 pass 用 FLAG_POINT_SAMPLE 分开。
//
// 读写都按 float4 走：源可能是 R16G16_FLOAT（矢量）或单通道深度，读出来缺的通道补
// 0/1，写进去只会落目标格式有的通道。这样能和别的 pass 共用一套根签名。
[numthreads(8, 8, 1)]
void ResampleMain(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= Width || id.y >= Height) return;
	const float2 uv = (float2(id.xy) + 0.5f) / float2(Width, Height);
	if (Flags & FLAG_BOX_DOWN)
	{
		// **精确面积平均。** 目标像素 id 在源坐标里覆盖 [id*Scale, (id+1)*Scale)，
		// 按每个源像素与这个区间的**重叠长度**加权。Scale=1 时自动退化成恒等拷贝。
		//
		// 原来写的是"ceil(Scale) 个等权 bilinear 采样"，那是个陷阱：
		// Scale=1.0526（处理分辨率 95%）时 taps 就变成 2，等权铺开的 4 个 bilinear
		// 采样各自又混了 2x2 源像素 —— 净效果接近一次完整的 2x2 盒式模糊。
		// 也就是说**只要分辨率不是 100%，喂给 DLSSNR 的图就先被抹掉了像素级细节**，
		// 而那正是 DLSSNR 唯一能下手的东西。95% 和 40% 看起来一样没效果就是这么来的。
		//
		// 用 Load 取整数源像素、自己算权重，不用 SampleLevel —— bilinear 会在
		// 显式权重之外再偷偷混一层，等于把刚修掉的问题按比例请回来。
		const float2 srcSize = float2(Width, Height) * max(Scale, 1e-6f);
		const float2 lo = float2(id.xy) * Scale;
		const float2 hi = lo + Scale;
		const int2 first = int2(floor(lo));
		const int2 maxCoord = int2(srcSize) - 1;
		float4 sum = 0.0f;
		float total = 0.0f;
		// 覆盖区间最多跨 ceil(Scale)+1 个源像素。滑条下限 40% -> Scale<=2.5，
		// 所以 5 够用；写成定长循环让权重为 0 的 tap 自己被跳掉。
		[unroll] for (int y = 0; y < 5; ++y)
		{
			const int sy = first.y + y;
			const float wy =
				max(0.0f, min(hi.y, float(sy + 1)) - max(lo.y, float(sy)));
			if (wy <= 0.0f) continue;
			[unroll] for (int x = 0; x < 5; ++x)
			{
				const int sx = first.x + x;
				const float wx =
					max(0.0f, min(hi.x, float(sx + 1)) - max(lo.x, float(sx)));
				if (wx <= 0.0f) continue;
				const float w = wx * wy;
				const int2 c = clamp(int2(sx, sy), int2(0, 0), maxCoord);
				sum += Source.Load(int3(c, 0)) * w;
				total += w;
			}
		}
		Target[id.xy] = total > 0.0f
			? sum / total
			: Source.SampleLevel(LinearClamp, uv, 0);
		return;
	}
	Target[id.xy] = (Flags & FLAG_POINT_SAMPLE)
		? Source.SampleLevel(PointClamp, uv, 0)
		: Source.SampleLevel(LinearClamp, uv, 0);
}

// **把 DLSSNR 的改动做成"比值"，留在低分辨率上。**
//
// t0 = 滤镜输出（低分），t1 = 喂给滤镜的输入（低分）。
// 之后 RatioApplyMain 把这张比值图双线性放大，乘到**全分辨率原图**上。
//
// 为什么走比值而不是直接把低分结果插值放大：直接放大等于把原生细节换成插值细节，
// 画面会明显变软 —— 那就把 DLSSNR 的意义抵消掉了。而"改动量"本身是低频的
// （降噪/局部色调都是大尺度的调整），放大它几乎无损。
// 这和 evaluate 那条路上的"按比例搬运"是同一个思路，那边验证过：滤镜不动时
// 比值恒为 1，结果逐位等于原图。
[numthreads(8, 8, 1)]
void RatioMakeMain(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= Width || id.y >= Height) return;
	const float3 filtered = Source.Load(int3(id.xy, 0)).rgb;
	const float3 input = Source2.Load(int3(id.xy, 0)).rgb;
	// **分子分母都加同一个 epsilon**，而不是只给分母兜底。
	// 只保护分母的话，暗部（两者都接近 0）会算出很大的比值 —— 那是"雾蒙蒙"的来源。
	// 加在两边时两者都接近 0 时比值自然趋于 1，也就是"暗部不动"。
	// **eps 要小。** 第一版用 1/64 = 0.0156，而这游戏是夜景，画面大片在 0.02~0.2 ——
	// 0.0156 相对 0.05 就是 31%，比值被硬拉向 1，暗部的效果直接砍半。
	// 1/512 在同样位置只衰减 9%，而近黑处（0.001 量级）靠下面的 clamp 兜住就够了：
	// 那里就算被放大 2 倍，绝对值也只有 0.002，看不见。
	const float eps = 1.0f / 512.0f;
	const float3 ratio = (filtered + eps) / (input + eps);
	// 夹住：降噪不该让一个像素亮/暗超过一档
	Target[id.xy] = float4(clamp(ratio, 0.5f, 2.0f), 1.0f);
}

// 全分辨率原图 x 放大后的比值。
// t0 = 原图（全分辨率，Load），t1 = 比值图（低分辨率，双线性采样放大）。
[numthreads(8, 8, 1)]
void RatioApplyMain(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= Width || id.y >= Height) return;
	const float4 original = Source.Load(int3(id.xy, 0));
	if (Flags & 128u) {
		Target[id.xy] = float4(ComposeLayers(original.rgb, Source2.Load(int3(id.xy, 0)).rgb), original.a);
		return;
	}
	const float2 uv = (float2(id.xy) + 0.5f) / float2(Width, Height);
	const float3 ratio = Source2.SampleLevel(LinearClamp, uv, 0).rgb;
	Target[id.xy] = float4(ComposeLayers(original.rgb, max(original.rgb, 0.0f) * ratio), original.a);
}

// 把深度或矢量画成能看的东西。
//
// 存在的理由是**可证伪**：开关真深度/真矢量前后画面变了，你得能确认那个变化真的来自
// 深度/矢量，而不是别的东西在动。看不到输入就只能猜。
[numthreads(8, 8, 1)]
void VisualizeMain(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= Width || id.y >= Height) return;
	const float2 uv = (float2(id.xy) + 0.5f) / float2(Width, Height);
	const float4 s = Source.SampleLevel(LinearClamp, uv, 0);

	float3 rgb;
	if (Flags & FLAG_RAW)
	{
		rgb = s.rgb * Scale;
	}
	else if (Flags & FLAG_DIFF)
	{
		// t0 = 滤镜的输出，t1 = 喂进去的输入（都在压缩域）。
		// 画放大过的绝对差 —— 全黑就是"滤镜什么都没做"，这是个**能证伪**的判据，
		// 比盯着最终画面猜"好像变了一点"可靠得多。
		const float3 a = s.rgb;
		const float3 b = Source2.SampleLevel(LinearClamp, uv, 0).rgb;
		rgb = abs(a - b) * Scale;
	}
	else if (Flags & FLAG_MOTION)
	{
		// 中灰 = 不动；偏红 = 向右，偏绿 = 向下（屏幕坐标系）。
		// 用"中心 0.5 + 带符号偏移"而不是 abs，这样**方向**看得出来 ——
		// 矢量约定反了的话，镜头左右移动时颜色会往反方向偏，一眼能看出。
		rgb = float3(0.5f + s.x * Scale, 0.5f + s.y * Scale, 0.5f);
	}
	else
	{
		// 深度：reversed-Z 下绝大多数值贴着 0 或 1，直接当灰度显示是一片黑或一片白。
		// 过一条幂曲线把层次拉开（Scale 就是指数，比如 0.05）。
		const float d = saturate(s.x);
		const float v = pow(d, max(Scale, 1e-3f));
		rgb = float3(v, v, v);
	}
	// 窄带信号先拉满 0..1，否则看不出结构（编码图实测挤在 0.72~0.85）。
	if (Flags & FLAG_STRETCH)
	{
		const float span = max(StretchHi - StretchLo, 1e-4f);
		rgb = (rgb - StretchLo) / span;
	}
	// **先夹到 0..1（这才是"显示值"），再整体放大去抵消游戏的 tonemap。**
	// 两步的顺序反过来的话，放大倍数会把所有内容推过 1.0，saturate 之后剩一张纯色图。
	Target[id.xy] = float4(saturate(rgb) * max(PostScale, 1e-6f), 1.0f);
}

