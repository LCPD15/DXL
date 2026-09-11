// 游戏内浮层的布局 + 绘制。纯 CPU + GDI，不碰 D3D。
//
// **布局只算一次。** `LayoutOverlay` 产出一串带矩形的 item，绘制和命中测试都读
// 同一份 —— 两处各算一遍矩形是这类 UI 最经典的 bug 源（点击位置和看到的对不上，
// 而且只在某些行、某些窗口尺寸下才错）。
//
// 抽成独立头文件的另一个原因和 OsdRaster.h 一样：能在
// tests\test-overlay-raster.cpp 里直接调、存成 BMP 用眼睛看。
// 浮层画在被注入的游戏进程里，那个环境下截图很不可靠（实测抓到的是壁纸）。

#pragma once

#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "SemGroups.h"

namespace DXL {

// 浮层能改的设置。**顺序就是界面从上到下的顺序**（用户指定的：
// 耗时 -> 开关 -> 两个 debug 开关 -> DLSSNR 参数 -> 关键信息）。
enum class OverlaySetting : int {
	// 两个总控开关。放最上面：玩家在游戏里最常做的事就是"开一下关一下看差别"，
	// 为这个切出去点界面是最没道理的。
	NrEnabled = 0,
	MasterEnabled,
	DebugDepth,
	DebugMotion,
	Preset,
	Style,
	Intensity,
	LocalTone,
	LocalStructure,
	SkinStructure,
	AutoMask,
	UiCorrection,
	RenderScale,
	// 棋盘格 ControlMask（#65）：实验验证 DLSSNR.ControlMask 吃我们喂的图
	//（画面显出棋盘格）。mask 是 RGBA、通道约定未公开（#67）——
	// 每通道一个滑块：单独拉满一个通道，看哪个能出完整棋盘格（gating 通道）。
	ControlMask,
	ControlMaskR,
	ControlMaskG,
	ControlMaskB,
	ControlMaskA,
	// 语义蒙版（#72/#79/#81）：总开关 + debug 小窗 + 18 整合组
	//（COCO 12 + ADE 场景 6，SEM_GROUP_COUNT 见 SemGroups.h）。
	// 组设置连号：SemGroup0..17（开关，网格勾选）、SemInt0..17（强度，
	// 勾选的组才展开成行）、SemBgInt（无组像素=背景的强度）。
	// Apply/Feed/布局都按 (setting - SemGroup0) / (setting - SemInt0) 算组号。
	SemanticMask,
	SemanticDebugView,
	SemGroup0, SemGroup1, SemGroup2, SemGroup3, SemGroup4, SemGroup5,
	SemGroup6, SemGroup7, SemGroup8, SemGroup9, SemGroup10, SemGroup11,
	SemGroup12, SemGroup13, SemGroup14, SemGroup15, SemGroup16, SemGroup17,
	SemInt0, SemInt1, SemInt2, SemInt3, SemInt4, SemInt5,
	SemInt6, SemInt7, SemInt8, SemInt9, SemInt10, SemInt11,
	SemInt12, SemInt13, SemInt14, SemInt15, SemInt16, SemInt17,
	SemBgInt,
	Count
};
constexpr int OVERLAY_SETTING_COUNT = int(OverlaySetting::Count);

enum class OverlayItemKind {
	Title,        // 标题栏（含右上角的 X）
	Close,        // 右上角那个 X 的可点区域
	Timing,       // 顶部的耗时读数
	Status,       // 状态区（分辨率/矢量来源/帧数）—— 耗时下面，永远显示
	Section,      // 分组标题
	Toggle,
	Check,        // #79 复选格（语义分组网格用）：小方框 + 名字，点整格翻转
	Slider,
	Select,
	Info          // 底部的说明 / 提示（空间不够时整段丢，所以状态不能放这）
};

struct OverlayItem {
	OverlayItemKind kind = OverlayItemKind::Info;
	int setting = -1;        // OverlaySetting，非控件为 -1
	int infoIndex = -1;      // Info 用：指向 OverlayState::info[]
	const wchar_t* label = nullptr;
	RECT row{};              // 整行（用于底色/hover）
	RECT control{};          // 可点区域（开关胶囊 / 滑条槽 / 下拉框 / X）
	float minValue = 0.0f;
	float maxValue = 1.0f;
	float step = 0.01f;
	int decimals = 2;
};

struct OverlayInfoLine {
	std::wstring text;
	bool warn = false;       // true = 用警示色（橙）画
};

// ---- 浮层三语（中 0 / 日 1 / 英 2）----
//
// 语言跟着工具界面走：UI 把选的语言写进 per-exe 配置（overlayLang），
// core 读出来灌进 OverlayState。浮层是 GDI 画的，没有 DOM 那种
// "查不到回退原文"的字典 —— 用 OT() 三元选一，漏了哪条就先把中文写上，
// 不可能因为语言把浮层弄坏。
inline const wchar_t* OT(int lang, const wchar_t* zh,
	const wchar_t* ja, const wchar_t* en) noexcept {
	return lang == 1 ? ja : lang == 2 ? en : zh;
}

struct OverlayState {
	// 统一用 float 存；Select 存的是整数选项下标，Toggle 存 0/1
	float values[OVERLAY_SETTING_COUNT]{};

	// 界面语言（0 中 / 1 日 / 2 英），跟着工具界面的选择走（见 OT）。
	int lang = 0;

	// 顶部耗时
	std::wstring timingMain = L"—";
	std::wstring timingSub;
	int timingLevel = 0;     // 0 好 / 1 偏高 / 2 太高 —— 决定颜色

	// 状态区（分辨率 / 矢量·深度来源 / 帧数）。放在耗时下面、开关上面 ——
	// **永远显示**。Info 那区空间不够时整段丢掉（LayoutOverlay 里写明的取舍：
	// 控件是刚需、说明是锦上添花），状态是每局都要看的数据，不能跟着陪葬。
	std::vector<OverlayInfoLine> status;

	// 底部关键信息
	std::vector<OverlayInfoLine> info;

	// #82 滚动：scroll = 内容上移的像素（标题/关闭钉顶不滚）；maxScroll =
	// 布局算出的上限（内容装得下时 0，滚轮无效）。Record 每次布局后回填
	// maxScroll；ProcessInput 夹 scroll 用的是上一帧值 —— 差一帧无感。
	int scroll = 0;
	int maxScroll = 0;

	// 交互态
	int hotIndex = -1;       // 鼠标下的 item 下标
	int activeIndex = -1;    // 正在拖动的 item 下标
	POINT cursor{ -1, -1 };  // 面板本地坐标；负数 = 不画光标
};

/* ---------------- 选项表 ---------------- */

// Select 的选项名。**数量由这里定死**，core 侧循环取值时也读它 ——
// 两边各写一份"有几项"迟早会漂。
inline const wchar_t* const* OverlayOptionsFor(
	OverlaySetting setting, int& count) noexcept {
	static const wchar_t* const PRESETS[] = {
		L"0 Default", L"1 Preset #1", L"2 Preset #2", L"3 Preset #3" };
	static const wchar_t* const STYLES[] = {
		L"0 Default", L"1 Natural", L"2 Cinematic" };
	switch (setting) {
	case OverlaySetting::Preset: count = 4; return PRESETS;
	case OverlaySetting::Style:  count = 3; return STYLES;
	default: count = 0; return nullptr;
	}
}

// 每条参数的作用说明。**悬浮在该行上时显示**（DrawOverlay 末尾的提示框）。
// 文案和工具界面 NR_PARAMS 里的 hint 保持一致 —— 两边各写一份迟早漂。
// 三语（lang 同 OT）：漏了哪条先把中文写上。
// #79 12 整合组双语名 —— 组序与 SegMaskFilter.cpp 的 kClassGroup 一致：
//   0 人物 | 1 车辆 | 2 动物 | 3 街具 | 4 运动 | 5 食物 | 6 餐具
//   7 家具 | 8 电器 | 9 卫浴 | 10 配饰 | 11 杂物（80 类全覆盖）
inline const wchar_t* SemGroupName(int g, int lang) noexcept {
	switch (g) {
	case 0: return OT(lang, L"人物", L"人物", L"People");
	case 1: return OT(lang, L"车辆", L"車両", L"Vehicles");
	case 2: return OT(lang, L"动物", L"動物", L"Animals");
	case 3: return OT(lang, L"街具", L"街頭設備", L"Street");
	case 4: return OT(lang, L"运动", L"スポーツ", L"Sports");
	case 5: return OT(lang, L"食物", L"食べ物", L"Food");
	case 6: return OT(lang, L"餐具", L"食器", L"Tableware");
	case 7: return OT(lang, L"家具", L"家具", L"Furniture");
	case 8: return OT(lang, L"电器", L"家電", L"Electronics");
	case 9: return OT(lang, L"卫浴", L"衛浴", L"Bathroom");
	case 10: return OT(lang, L"配饰", L"装身具", L"Accessories");
	case 11: return OT(lang, L"杂物", L"雑物", L"Misc");
	// #81 场景组（ADE20K 场景解析；建筑含古今建筑/墙/桥）
	case 12: return OT(lang, L"建筑", L"建築", L"Buildings");
	case 13: return OT(lang, L"植被", L"植生", L"Vegetation");
	case 14: return OT(lang, L"天空", L"空", L"Sky");
	case 15: return OT(lang, L"水域", L"水域", L"Water");
	case 16: return OT(lang, L"地形", L"地形", L"Terrain");
	case 17: return OT(lang, L"其他场景", L"その他シーン", L"Other scene");
	default: return L"—";
	}
}
inline const wchar_t* OverlayHintFor(int setting, int lang) noexcept {
	const bool ja = lang == 1, en = lang == 2;
	(void)ja; (void)en;
	// #79 语义组连号区段：比 26 个 case 标签可读，下标即组号
	const int s = setting;
	if (s >= int(OverlaySetting::SemGroup0) && s <= int(OverlaySetting::SemGroup17)) {
		return en ? L"Check to include this group in the mask; uncheck to treat it as background. Its intensity slider appears when checked."
			: L"勾选 = 该组进入蒙版；取消 = 当作背景。勾选后出现它的强度滑条。";
	}
	if (s >= int(OverlaySetting::SemInt0) && s <= int(OverlaySetting::SemInt17)) {
		return en ? L"Intensity for this group: RGBA = 255 x this on all channels (1 = identical to a checkerboard cell, 0 = nothing)."
			: L"该组强度：四个通道 = 255×这个值（1 = 与棋盘格应用格一致，0 = 不加）。";
	}
	switch (OverlaySetting(setting)) {
	case OverlaySetting::NrEnabled:
		return en ? L"Toggle the DLSSNR neural filter. Turning it off also saves its GPU cost for this session."
			: ja ? L"DLSSNR ニューラルフィルタのスイッチ。オフにすると今回のセッションではコストも節約できます。"
			: L"DLSSNR 神经滤镜的开关。关掉它这一局的 DLSSNR 开销也省下来。";
	case OverlaySetting::MasterEnabled:
		return en ? L"Master switch for all effects — same as Alt+D in game. Off stops DLSSNR and upscaling entirely."
			: ja ? L"全効果のマスタースイッチ。ゲーム内の Alt+D と同じです。オフで DLSSNR と超解像度も停止します。"
			: L"所有效果的总开关，和游戏里 Alt+D 是同一个。关掉它 DLSSNR 和超分全部停用。";
	case OverlaySetting::DebugDepth:
		return en ? L"Brightens/darkens the picture by game depth — use it to confirm the depth buffer is correct."
			: ja ? L"ゲームの深度で明暗をつけます。深度バッファが正しく取れているかの確認用。"
			: L"用游戏深度把画面按远近提亮/压暗 —— 用来确认深度抓对了没有。";
	case OverlaySetting::DebugMotion:
		return en ? L"Draws motion vectors (red = right, green = down) — use it to confirm real vectors are connected."
			: ja ? L"モーションベクトルを描画します（赤=右 緑=下）。実ベクトルが接続されているかの確認用。"
			: L"把运动矢量画出来（红=向右 绿=向下）—— 用来确认真矢量接上了没有。";
	case OverlaySetting::Preset:
		return en ? L"Model weights — different presets give different output styles. Maps to Model A / B / C."
			: ja ? L"モデルの重み。プリセットごとに出力スタイルが変わります。公式の Model A / B / C に対応。"
			: L"不同权重的模型，输出风格不一样。对应官方面板的 Model A / B / C。";
	case OverlaySetting::Style:
		return en ? L"Output style: 0 default / 1 natural / 2 cinematic."
			: ja ? L"出力スタイル：0 デフォルト / 1 ナチュラル / 2 シネマ。"
			: L"输出风格：0 默认 / 1 自然 / 2 电影感。";
	case OverlaySetting::Intensity:
		return en ? L"Overall intensity (0–1). At zero the picture is unchanged but the cost stays — it only scales with pixels."
			: ja ? L"総強度（0〜1）。ゼロでもコストは変わりません —— コストはピクセル数だけで決まります。"
			: L"总强度（0~1）。归零画面和不开一样，但开销照付 —— 开销只跟像素数走。";
	case OverlaySetting::LocalTone:
		return en ? L"Tone strength (0–2). Low frequencies: large-scale lighting and color response."
			: ja ? L"トーン強度（0〜2）。低周波：広範囲のライティングと色応答を担当。"
			: L"色调强度（0~2）。管低频：大范围的光照和色彩响应。";
	case OverlaySetting::LocalStructure:
		return en ? L"Structure strength (0–2). High frequencies: AO, contact shadows, reflections, subsurface scattering."
			: ja ? L"構造強度（0〜2）。高周波：AO、接触影、反射、サブサーフェス散乱などの細部を担当。"
			: L"结构强度（0~2）。管高频：环境光遮蔽、接触阴影、反射、次表面散射这类细节。";
	case OverlaySetting::SkinStructure:
		return en ? L"Skin structure strength (0–2). 0 = don't touch skin separately."
			: ja ? L"肌の構造強度（0〜2）。0 = 肌を個別には調整しない。"
			: L"皮肤结构强度（0~2）。单独控制皮肤上的结构。0 = 不单独干预皮肤。";
	case OverlaySetting::AutoMask:
		return en ? L"Semantic auto mask: the model recognizes characters and environment by itself and only enhances the environment."
			: ja ? L"セマンティック自動マスク：モデルがキャラと環境を自動判別し、環境のみを強化します。"
			: L"语义自动遮罩：模型自己识别画面里的角色和环境，只增强环境、不动角色。";
	case OverlaySetting::UiCorrection:
		return en ? L"UI correction. Needs a separate UI layer, which post-process injection cannot get — currently no effect."
			: ja ? L"UI 補正。別々の UI レイヤーが必要で、後処理注入では取得できません —— 現在は効果なし。"
			: L"UI 修正。⚠ 需要单独的 UI 图层，后处理注入拿不到 —— 现在开了暂无效果。";
	case OverlaySetting::RenderScale:
		return en ? L"Processing resolution: DLSSNR runs on this fraction of the picture. Cost scales with pixels — the only perf knob."
			: ja ? L"処理解像度：DLSSNR はこの割合の解像度で動きます。コストはピクセル数に比例 —— 唯一の省電力ツマミ。"
			: L"处理分辨率：DLSSNR 在这个比例的画面上跑。开销只跟像素数走，这是唯一能省性能的旋钮。";
	case OverlaySetting::ControlMask:
		return en ? L"Checkerboard control mask: feeds a checkerboard into DLSSNR.ControlMask (1 = apply, 0 = skip). Visible checkerboard on screen = the mask path works."
			: ja ? L"チェッカーコントロールマスク：DLSSNR.ControlMask にチェッカーを供給（1=適用、0=スキップ）。画面にチェッカーが見えればマスク経路は機能しています。"
			: L"棋盘格蒙版：往 DLSSNR.ControlMask 喂棋盘格（1=应用效果、0=跳过）。画面能看出棋盘格就说明蒙版通路是生效的。";
	// 通道约定（#67 用户实机逐通道隔离确认；官方未公开，别再猜）：
	//   R = 逐格总强度 —— 乘法链最外层（R=1 → 该格 NR 全关，GB 再调也无效果）
	//   G = 逐格局部色调（Local Tone）
	//   B = 逐格局部结构（Local Structure）
	//   A = 暂未观察到效果（官方演示每组只有 Structure/Tone 两钮；按无效记录）
	case OverlaySetting::ControlMaskR:
		return en ? L"Mask R: per-cell overall intensity — the outer multiplier. R=1 turns NR fully off in that cell; G/B have no effect while R=1."
			: ja ? L"マスク R：セルごとの総強度 —— 一番外側の掛け算。R=1 でそのセルの NR は完全オフ、その間 G/B は効きません。"
			: L"Mask R：逐格总强度 —— 乘法链最外层。R=1 时该格 NR 完全关闭，此时调 G/B 都无效。";
	case OverlaySetting::ControlMaskG:
		return en ? L"Mask G: per-cell Local Tone modulation. G=1 zeroes tone in that cell (structure untouched)."
			: ja ? L"マスク G：セルごとのローカルトーン調整。G=1 でそのセルのトーンをゼロ（構造はそのまま）。"
			: L"Mask G：逐格局部色调。G=1 时该格色调归零（结构不受影响）。";
	case OverlaySetting::ControlMaskB:
		return en ? L"Mask B: per-cell Local Structure modulation. B=1 zeroes structure in that cell (tone untouched)."
			: ja ? L"マスク B：セルごとのローカル構造調整。B=1 でそのセルの構造をゼロ（トーンはそのまま）。"
			: L"Mask B：逐格局部结构。B=1 时该格结构归零（色调不受影响）。";
	case OverlaySetting::ControlMaskA:
		return en ? L"Mask A: no effect observed so far (convention unpublished; the official demo only exposes Structure/Tone per group). Recorded as unused."
			: ja ? L"マスク A：今のところ効果なし（規約非公開。公式デモは各グループ Structure/Tone のみ）。未使用として記録。"
			: L"Mask A：暂未观察到效果（官方未公开；官方演示每组只有 Structure/Tone 两钮）。按无效记录。";
	case OverlaySetting::SemanticMask:
		return en ? L"Semantic mask: TensorRT segments the picture in real time (needs nvinfer_11.dll + models\\yolo11n-seg.plan beside the core dll; a second model models\\yolo26s-sem-ade20k.plan adds the 6 scene groups). COCO's 80 classes are consolidated into the 12 object groups below; the scene model adds Buildings/Vegetation/Sky/Water/Terrain. Each checked group gets RGBA = 255 x its intensity, everything else gets the background intensity. Takes priority over the checkerboard when both are on. ~5 ms per inference (both models)."
			: ja ? L"セマンティックマスク：TensorRT でリアルタイム分割（core dll の横に nvinfer_11.dll + models\\yolo11n-seg.plan が必要）。COCO 80 クラスを下の 12 グループに統合。チェックしたグループは RGBA = 255 × 強度、その他は背景強度。チェッカーと両方オンの時はこちらが優先。推論約 3 ms。"
			: L"语义蒙版：TensorRT 实时分割（要 core dll 旁边的 nvinfer_11.dll + models\\yolo11n-seg.plan；第二模型 models\\yolo26s-sem-ade20k.plan 加出 6 个场景组）。COCO 80 类整合成下面 12 个物体组；场景模型再加建筑/植被/天空/水域/地形。勾选的组 RGBA=255×该组强度，其余走背景强度。与棋盘格双开时语义优先。双模型推理约 5ms。";
	case OverlaySetting::SemanticDebugView:
		return en ? L"Small window at the game's bottom-left, colored per group (one color per consolidated group, brightness = coverage): instantly shows which group each area belongs to."
			: ja ? L"ゲーム画面左下に小窓。グループごとに色分け（統合グループごとに一色、明るさ = 被覆度）：どの領域がどのグループか一目で分かります。"
			: L"游戏窗口左下角小窗，按组着色（每个整合组一种颜色，亮度=覆盖度）：一眼看出哪块归哪个组。";
	case OverlaySetting::SemBgInt:
		return en ? L"Background intensity: pixels outside all checked groups get RGBA = 255 x this. 1 = pure white (identical to a checkerboard cell, full NR), 0 = nothing."
			: ja ? L"背景強度：チェックしたグループ以外のピクセルは RGBA = 255 × この値。1 = 純白（チェッカーのセルと同一、フル NR）、0 = 何も適用しません。"
			: L"背景强度：所有勾选组以外的像素 RGBA=255×这个值。1 = 纯白（与棋盘格应用格一致，满强度 NR），0 = 不加。";
	default:
		return nullptr;
	}
}

/* ---------------- 配色 / 尺寸 ---------------- */

struct OverlayStyle {
	COLORREF background = RGB(0x1c, 0x1c, 0x1c);
	COLORREF titleBar = RGB(0x26, 0x26, 0x26);
	COLORREF rowAlt = RGB(0x22, 0x22, 0x22);
	COLORREF hover = RGB(0x2e, 0x2e, 0x2e);
	COLORREF accent = RGB(0x76, 0xb9, 0x00);   // NVIDIA 绿
	COLORREF text = RGB(0xf2, 0xf2, 0xf2);
	COLORREF textDim = RGB(0xa8, 0xa8, 0xa8);
	COLORREF textMute = RGB(0x82, 0x82, 0x82);
	COLORREF track = RGB(0x4a, 0x4a, 0x4a);
	COLORREF warn = RGB(0xe0, 0xa0, 0x30);
	COLORREF bad = RGB(0xe0, 0x53, 0x3d);
	COLORREF line = RGB(0x38, 0x38, 0x38);
	COLORREF tooltipBg = RGB(0x2e, 0x2e, 0x2e);   // 悬浮提示框的底
	const wchar_t* face = L"Microsoft YaHei UI";
};

constexpr uint32_t OVERLAY_WIDTH = 480;
// **上限必须装得下全部内容，否则最底下的行在任何屏幕上都显示不出来。**
// 当前内容高（#79）：#78 基础上参数区 -2 行（删强度/反相）+ 语义分组区
//（标题 28 + 网格 3×26 + 组强度标题 28 + 勾选组行 × 34 + 背景 34）。
// 典型勾 1~3 组 ≈ 1300；全勾 12 组 ≈ 1700 —— LayoutOverlay 按 availableHeight
// 裁掉画不出的行（网格在最前，永远显示；强度行靠后，1080p 全勾会裁尾）。
// 旧值 760/848/952/1056/1090/1124 都裁过最后一行：裁掉的行命中测试跟着丢
//（LayoutOverlay 末尾把画不出来的 item pop 掉），1080p 全屏一样裁。
constexpr uint32_t OVERLAY_MAX_HEIGHT = 1300;

namespace overlay_detail {

constexpr int PAD = 16;
constexpr int TITLE_H = 40;
constexpr int TIMING_H = 76;
constexpr int SECTION_H = 28;
constexpr int ROW_H = 34;
constexpr int INFO_LINE_H = 20;
constexpr int CTRL_W = 190;      // 控件列宽度
constexpr int VALUE_W = 52;      // 滑块右边的数值列

}  // namespace overlay_detail

/* ---------------- 布局 ---------------- */

// 算出所有 item 的矩形和面板高度。**唯一一处算布局的地方。**
//
// availableHeight = 画面能给多高（通常是窗口客户区高度减一点边距）。
// **必须传进来，不能只靠一个编译期上限**：夹具窗口只有 681 高，而固定 691 的面板
// 直接就贴不上去 —— 症状是浮层"打开了但画面上什么都没有"，日志里才看得出原因。
inline void LayoutOverlay(const OverlayState& state,
	std::vector<OverlayItem>& items, uint32_t& outWidth, uint32_t& outHeight,
	uint32_t availableHeight = OVERLAY_MAX_HEIGHT,
	int* outMaxScroll = nullptr) noexcept {
	using namespace overlay_detail;
	items.clear();
	const int width = (int)OVERLAY_WIDTH;
	int y = 0;

	auto push = [&](OverlayItemKind kind, const wchar_t* label, int height) -> OverlayItem& {
		OverlayItem item;
		item.kind = kind;
		item.label = label;
		item.row = { 0, y, width, y + height };
		items.push_back(item);
		y += height;
		return items.back();
	};

	// 标题栏 + 右上角的 X
	push(OverlayItemKind::Title, L"DLSS5 Quick", TITLE_H);
	{
		OverlayItem close;
		close.kind = OverlayItemKind::Close;
		close.row = { width - TITLE_H, 0, width, TITLE_H };
		close.control = close.row;
		items.push_back(close);
	}

	// 最顶部：DLSSNR 耗时
	push(OverlayItemKind::Timing, OT(state.lang,
		L"DLSSNR 耗时", L"DLSSNR 処理時間", L"DLSSNR GPU time"), TIMING_H);

	// 耗时下面：状态区（分辨率 / 矢量·深度来源 / 帧数）。
	// 小字单行一条，不带标题 —— 四条状态配一个"状态"标题就臃肿了。
	// 行高用 INFO_LINE_H（同一套小字）。
	if (!state.status.empty()) {
		HDC measureDc = CreateCompatibleDC(nullptr);
		HFONT measureFont = measureDc ? CreateFontW(-12, 0, 0, 0, FW_NORMAL,
			FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
			CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
			L"Microsoft YaHei UI") : nullptr;
		HGDIOBJ oldMeasureFont = measureFont
			? SelectObject(measureDc, measureFont) : nullptr;
		for (size_t i = 0; i < state.status.size(); ++i) {
			// 状态行不折行（短数据），量一次即可；量不出回退单行高
			int lineHeight = INFO_LINE_H + 4;
			if (measureDc && measureFont) {
				RECT box{ 0, 0, width - PAD * 2, 0 };
				DrawTextW(measureDc, state.status[i].text.c_str(), -1, &box,
					DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
				if (box.bottom > 0) lineHeight = box.bottom + 6;
			}
			OverlayItem& item = push(OverlayItemKind::Status, nullptr, lineHeight);
			item.infoIndex = (int)i;   // Status 和 Info 一样用 infoIndex 指向自己的数组
		}
		if (measureFont) {
			SelectObject(measureDc, oldMeasureFont);
			DeleteObject(measureFont);
		}
		if (measureDc) DeleteDC(measureDc);
	}

	// 紧接着：两个总控开关。**这两个是游戏里最常按的**，所以在最上面。
	push(OverlayItemKind::Section, OT(state.lang,
		L"开关", L"スイッチ", L"Switches"), SECTION_H);
	struct ToggleDef { OverlaySetting setting; const wchar_t* label; };
	const ToggleDef MAIN_TOGGLES[] = {
		{ OverlaySetting::NrEnabled, L"DLSSNR" },
		{ OverlaySetting::MasterEnabled, OT(state.lang,
			L"总开关（所有效果）", L"マスター（全効果）", L"Master (all effects)") },
	};
	for (const ToggleDef& def : MAIN_TOGGLES) {
		OverlayItem& item = push(OverlayItemKind::Toggle, def.label, ROW_H);
		item.setting = int(def.setting);
		const int cy = (item.row.top + item.row.bottom) / 2;
		item.control = { width - PAD - 44, cy - 11, width - PAD, cy + 11 };
	}

	// 下方：两个 debug 开关
	push(OverlayItemKind::Section, OT(state.lang,
		L"Debug 视图", L"Debug ビュー", L"Debug view"), SECTION_H);
	const ToggleDef DEBUG_TOGGLES[] = {
		{ OverlaySetting::DebugDepth, OT(state.lang,
			L"深度（近亮远暗）", L"深度（近明遠暗）", L"Depth (near bright)") },
		{ OverlaySetting::DebugMotion, OT(state.lang,
			L"运动矢量（红=右 绿=下）", L"モーションベクトル（赤=右 緑=下）",
			L"Motion vectors (red=right)") },
	};
	for (const ToggleDef& def : DEBUG_TOGGLES) {
		OverlayItem& item = push(OverlayItemKind::Toggle, def.label, ROW_H);
		item.setting = int(def.setting);
		const int cy = (item.row.top + item.row.bottom) / 2;
		item.control = { width - PAD - 44, cy - 11, width - PAD, cy + 11 };
	}

	// 再下方：DLSSNR 参数
	push(OverlayItemKind::Section, OT(state.lang,
		L"DLSSNR 参数", L"DLSSNR パラメータ", L"DLSSNR parameters"), SECTION_H);

	struct ParamDef {
		OverlaySetting setting;
		const wchar_t* label;
		OverlayItemKind kind;
		float minValue, maxValue, step;
		int decimals;
	};
	const ParamDef PARAMS[] = {
		{ OverlaySetting::Preset,          L"Render Preset",     OverlayItemKind::Select, 0, 3, 1, 0 },
		{ OverlaySetting::Style,           L"Style",             OverlayItemKind::Select, 0, 2, 1, 0 },
		// 范围和工具界面一致（用户拍的板）：intensity 0~1；其余强度 0~2。
		{ OverlaySetting::Intensity,       L"Intensity",         OverlayItemKind::Slider, 0.0f, 1.0f, 0.05f, 2 },
		{ OverlaySetting::LocalTone,       L"Local Tone",        OverlayItemKind::Slider, 0.0f, 2.0f, 0.05f, 2 },
		{ OverlaySetting::LocalStructure,  L"Local Structure",   OverlayItemKind::Slider, 0.0f, 2.0f, 0.05f, 2 },
		{ OverlaySetting::SkinStructure,   L"Skin Structure",    OverlayItemKind::Slider, 0.0f, 2.0f, 0.05f, 2 },
		{ OverlaySetting::AutoMask,        L"Auto Mask",         OverlayItemKind::Toggle, 0, 1, 1, 0 },
		{ OverlaySetting::UiCorrection,    L"UI Correction",     OverlayItemKind::Toggle, 0, 1, 1, 0 },
		{ OverlaySetting::RenderScale,     OT(state.lang,
			L"处理分辨率", L"処理解像度", L"Processing res"),
			OverlayItemKind::Slider, 0.4f, 1.0f, 0.05f, 2 },
		// 棋盘格 ControlMask（#65）：验证过的实验转正式功能。开关 + 每通道强度。
		{ OverlaySetting::ControlMask,     OT(state.lang,
			L"棋盘格蒙版", L"チェッカーマスク", L"Checkerboard mask"),
			OverlayItemKind::Toggle, 0, 1, 1, 0 },
		// 通道约定（#67 用户实机确认；官方未公开）：R=总开关、G=色调、B=结构、A=无用。
		// 官方演示每组只有 Structure/Tone 两钮 —— 和 G/B 对应，A 无对应。
		{ OverlaySetting::ControlMaskR,    OT(state.lang,
			L"Mask R（总强度）", L"マスク R（総強度）", L"Mask R (intensity)"),
			OverlayItemKind::Slider, 0.0f, 1.0f, 0.05f, 2 },
		{ OverlaySetting::ControlMaskG,    OT(state.lang,
			L"Mask G（色调）", L"マスク G（トーン）", L"Mask G (tone)"),
			OverlayItemKind::Slider, 0.0f, 1.0f, 0.05f, 2 },
		{ OverlaySetting::ControlMaskB,    OT(state.lang,
			L"Mask B（结构）", L"マスク B（構造）", L"Mask B (structure)"),
			OverlayItemKind::Slider, 0.0f, 1.0f, 0.05f, 2 },
		{ OverlaySetting::ControlMaskA,    OT(state.lang,
			L"Mask A（无效）", L"マスク A（無効）", L"Mask A (unused)"),
			OverlayItemKind::Slider, 0.0f, 1.0f, 0.05f, 2 },
		// 语义蒙版（#72/#79/#81）：总开关 + debug 小窗。18 组勾选/强度在
		// PARAMS 循环之后单独排（网格 + 动态行，见下）。
		{ OverlaySetting::SemanticMask,     OT(state.lang,
			L"语义蒙版（分组）", L"セマンティックマスク（グループ）",
			L"Semantic mask (groups)"),
			OverlayItemKind::Toggle, 0, 1, 1, 0 },
		{ OverlaySetting::SemanticDebugView, OT(state.lang,
			L"语义蒙版 debug 视图", L"セマンティックマスク debug",
			L"Semantic mask debug view"),
			OverlayItemKind::Toggle, 0, 1, 1, 0 },
	};
	for (const ParamDef& def : PARAMS) {
		OverlayItem& item = push(def.kind, def.label, ROW_H);
		item.setting = int(def.setting);
		item.minValue = def.minValue;
		item.maxValue = def.maxValue;
		item.step = def.step;
		item.decimals = def.decimals;
		const int cy = (item.row.top + item.row.bottom) / 2;
		if (def.kind == OverlayItemKind::Toggle) {
			item.control = { width - PAD - 44, cy - 11, width - PAD, cy + 11 };
		} else if (def.kind == OverlayItemKind::Select) {
			item.control = { width - PAD - CTRL_W, cy - 12, width - PAD, cy + 12 };
		} else {
			// 滑条槽。右边留出 VALUE_W 给数值 —— **数值列宽度固定**，
			// 否则数字变长会把滑条挤短，拖动时取值反复横跳（工具界面上栽过一次）。
			item.control = { width - PAD - CTRL_W, cy - 9,
				width - PAD - VALUE_W, cy + 9 };
		}
	}

	// ---- 语义分组（#79/#81）：18 整合组勾选网格 + 动态强度行 ----
	// 用户拍板的方案 2（不滚动、不超屏）：组做成网格勾选（4 列 × 3 行，
	// 每格 = 复选框 + 组名），**勾选的组**才展开一条强度行 —— 没勾的组
	// 不占行。背景强度永远一行（无组像素 = 背景）。
	push(OverlayItemKind::Section, OT(state.lang,
		L"语义分组（勾选进入蒙版）", L"セマンティックグループ（チェックでマスク）",
		L"Semantic groups (check to mask)"), SECTION_H);
	{
		// #81 起 18 组（SEM_GROUP_COUNT）：4 列铺，末行不满补空 ——
		// 组号 = 网格序 = kClassGroup/kAdeGroup 的组序。
		constexpr int GRID_COLS = 4, CELL_H = 26;
		constexpr int GAP = 4;
		constexpr int GRID_ROWS = (SEM_GROUP_COUNT + GRID_COLS - 1) / GRID_COLS;
		const int cellW = (width - PAD * 2 - GAP * (GRID_COLS - 1)) / GRID_COLS;
		for (int g = 0; g < int(SEM_GROUP_COUNT); ++g) {
			const int gx = g % GRID_COLS, gy = g / GRID_COLS;
			OverlayItem item;
			item.kind = OverlayItemKind::Check;
			item.setting = int(OverlaySetting::SemGroup0) + g;
			item.label = SemGroupName(g, state.lang);
			item.row = { PAD + gx * (cellW + GAP), y + gy * CELL_H,
				PAD + gx * (cellW + GAP) + cellW, y + gy * CELL_H + CELL_H };
			// 复选框小方格在格子左端（点整格都行 —— 命中认 row）
			item.control = { item.row.left + 2, item.row.top + 5,
				item.row.left + 16, item.row.bottom - 5 };
			items.push_back(item);
		}
		y += CELL_H * GRID_ROWS;
	}
	// 勾选的组才展开强度行；背景强度永远在。行高/控件矩形和 PARAMS 同款。
	{
		bool any = false;
		for (int g = 0; g < int(SEM_GROUP_COUNT); ++g) {
			if (state.values[int(OverlaySetting::SemGroup0) + g] <= 0.5f) continue;
			if (!any) {
				any = true;
				push(OverlayItemKind::Section, OT(state.lang,
					L"组强度", L"グループ強度", L"Group intensity"), SECTION_H);
			}
			OverlayItem& item = push(OverlayItemKind::Slider,
				SemGroupName(g, state.lang), ROW_H);
			item.setting = int(OverlaySetting::SemInt0) + g;
			item.minValue = 0.0f;
			item.maxValue = 1.0f;
			item.step = 0.05f;
			item.decimals = 2;
			const int cy = (item.row.top + item.row.bottom) / 2;
			item.control = { width - PAD - CTRL_W, cy - 9,
				width - PAD - VALUE_W, cy + 9 };
		}
		OverlayItem& bg = push(OverlayItemKind::Slider, OT(state.lang,
			L"背景强度", L"背景強度", L"Background intensity"), ROW_H);
		bg.setting = int(OverlaySetting::SemBgInt);
		bg.minValue = 0.0f;
		bg.maxValue = 1.0f;
		bg.step = 0.05f;
		bg.decimals = 2;
		const int bcy = (bg.row.top + bg.row.bottom) / 2;
		bg.control = { width - PAD - CTRL_W, bcy - 9,
			width - PAD - VALUE_W, bcy + 9 };
	}

	// 最下方：关键信息
	//
	// **行高用 DC 精确量，不按字数估。** 估算要么裁掉文字（下限风险），
	// 要么在面板底部留一大片空白（上限浪费）—— 实测按 chars/26 估出来
	// 底下空了三十多像素。量一次的代价只有几微秒，而布局只在重画时算。
	//
	// **空间不够时整段丢掉，不要裁一半。** 说明文字被从中间切断比没有更糟：
	// 读的人不知道后面还有话。控件那部分是刚需，说明是锦上添花，所以牺牲后者。
	// #82 滚动：上限就是调用方给的可视高度（Record 传画面高-32）。
	// 旧的 OVERLAY_MAX_HEIGHT 硬上限参与 min 的写法删了 —— 有滚动之后
	// "装不下"从"裁掉"变成"滚下去"，硬上限只会白白逼出滚动条。
	const uint32_t limit = availableHeight ? availableHeight : OVERLAY_MAX_HEIGHT;
	// #82：说明区不再按空间丢 —— 丢一半比滚动到一半更糟。整段保留，
	// 装不下就滚。
	if (!state.info.empty()) {
		push(OverlayItemKind::Section, OT(state.lang,
			L"说明", L"説明", L"Notes"), SECTION_H);
		HDC measureDc = CreateCompatibleDC(nullptr);
		HFONT measureFont = measureDc ? CreateFontW(-12, 0, 0, 0, FW_NORMAL,
			FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
			CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
			L"Microsoft YaHei UI") : nullptr;
		HGDIOBJ oldMeasureFont = measureFont
			? SelectObject(measureDc, measureFont) : nullptr;
		for (size_t i = 0; i < state.info.size(); ++i) {
			int lineHeight = INFO_LINE_H * 2 + 6;   // 量不出来时的保守回退
			if (measureDc && measureFont) {
				RECT box{ 0, 0, width - PAD * 2, 0 };
				DrawTextW(measureDc, state.info[i].text.c_str(), -1, &box,
					DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
				lineHeight = box.bottom + 8;
			}
			OverlayItem& item = push(OverlayItemKind::Info, nullptr, lineHeight);
			item.infoIndex = (int)i;
		}
		if (measureFont) {
			SelectObject(measureDc, oldMeasureFont);
			DeleteObject(measureFont);
		}
		if (measureDc) DeleteDC(measureDc);
	}

	y += PAD / 2;
	outWidth = OVERLAY_WIDTH;
	// #82 滚动：面板高 = 内容装得下就全展开，装不下就铺满可视高度。
	// 旧的"把画不出的行 pop 掉"删了 —— pop 掉的行命中测试跟着丢，
	// 底下几组永远点不到（#79 全勾 12 组在 1080p 上就是这么丢的）；
	// 现在全部保留，滚下去就点得到。
	const uint32_t view = (std::min)(uint32_t(y), limit);
	outHeight = view;
	const int maxScroll = (std::max)(0, int(y) - int(view));
	if (outMaxScroll) *outMaxScroll = maxScroll;
	// 内容行上移 scroll（标题/关闭钉顶不滚）。row 和 control 一起挪 ——
	// 滑条槽/复选框画在 control 上，漏挪就是"点到 A 画出 B"。
	const int scroll = (std::min)((std::max)(0, state.scroll), maxScroll);
	if (scroll > 0) {
		for (OverlayItem& item : items) {
			if (item.kind == OverlayItemKind::Title ||
				item.kind == OverlayItemKind::Close) continue;
			OffsetRect(&item.row, 0, -scroll);
			OffsetRect(&item.control, 0, -scroll);
		}
	}
}

/* ---------------- 绘制 ---------------- */

namespace overlay_detail {

inline void FillRectColor(HDC dc, const RECT& rect, COLORREF color) {
	HBRUSH brush = CreateSolidBrush(color);
	FillRect(dc, &rect, brush);
	DeleteObject(brush);
}

inline void DrawTextIn(HDC dc, const RECT& rect, const wchar_t* text,
	COLORREF color, UINT format) {
	if (!text || !*text) return;
	SetTextColor(dc, color);
	RECT box = rect;
	DrawTextW(dc, text, -1, &box, format | DT_NOPREFIX);
}

}  // namespace overlay_detail

// bgra：顶向下，行距固定 OVERLAY_WIDTH*4。
inline bool RasterizeOverlay(const OverlayState& state,
	const std::vector<OverlayItem>& items, uint32_t width, uint32_t height,
	uint8_t* bgra, const OverlayStyle& style = {}) noexcept {
	using namespace overlay_detail;
	if (!bgra || !width || !height) return false;

	BITMAPINFO info{};
	info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	info.bmiHeader.biWidth = (LONG)OVERLAY_WIDTH;
	info.bmiHeader.biHeight = -(LONG)height;   // 负 = 顶向下，和纹理行序一致
	info.bmiHeader.biPlanes = 1;
	info.bmiHeader.biBitCount = 32;
	info.bmiHeader.biCompression = BI_RGB;

	HDC dc = CreateCompatibleDC(nullptr);
	if (!dc) return false;
	void* bits = nullptr;
	HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
	if (!bitmap || !bits) {
		if (bitmap) DeleteObject(bitmap);
		DeleteDC(dc);
		return false;
	}
	HGDIOBJ oldBitmap = SelectObject(dc, bitmap);

	HFONT fontBody = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
		DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
		DEFAULT_PITCH, style.face);
	HFONT fontBold = CreateFontW(-14, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
		DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
		DEFAULT_PITCH, style.face);
	HFONT fontBig = CreateFontW(-30, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
		DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
		DEFAULT_PITCH, style.face);
	HFONT fontSmall = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
		DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
		DEFAULT_PITCH, style.face);
	HGDIOBJ oldFont = SelectObject(dc, fontBody);
	SetBkMode(dc, TRANSPARENT);

	const RECT whole{ 0, 0, (LONG)width, (LONG)height };
	FillRectColor(dc, whole, style.background);

	int rowParity = 0;
	// #82 滚动：内容行上移后可能钻到标题带底下 —— 内容裁剪进标题带以下，
	// 标题/关闭（永远在最前两个）画完才设。裁剪一直管到悬浮提示；
	// 滚动条和光标画之前还回去（SelectClipRgn(nullptr)）。
	bool contentClip = false;
	for (size_t index = 0; index < items.size(); ++index) {
		const OverlayItem& item = items[index];
		if (item.row.top >= (LONG)height) break;
		if (!contentClip && item.kind != OverlayItemKind::Title &&
			item.kind != OverlayItemKind::Close) {
			IntersectClipRect(dc, 0, TITLE_H, (LONG)width, (LONG)height);
			contentClip = true;
		}
		const bool hot = (int)index == state.hotIndex;
		const int cy = (item.row.top + item.row.bottom) / 2;

		switch (item.kind) {
		case OverlayItemKind::Title: {
			FillRectColor(dc, item.row, style.titleBar);
			// 左边那道绿条：一眼认出是本工具，而不是游戏自己的 UI
			RECT bar{ 0, item.row.top, 4, item.row.bottom };
			FillRectColor(dc, bar, style.accent);
			SelectObject(dc, fontBold);
			RECT text{ PAD, item.row.top, item.row.right - TITLE_H, item.row.bottom };
			DrawTextIn(dc, text, OT(state.lang,
				L"DLSS5 Quick · 游戏内浮层", L"DLSS5 Quick · ゲーム内オーバーレイ",
				L"DLSS5 Quick · in-game overlay"),
				style.text, DT_SINGLELINE | DT_VCENTER);
			SelectObject(dc, fontSmall);
			RECT hint{ PAD, item.row.top, item.row.right - TITLE_H - 6,
				item.row.bottom };
			DrawTextIn(dc, hint, OT(state.lang,
				L"拖这里可移动 · ESC 关闭", L"ここをドラッグで移動 · ESC で閉じる",
				L"Drag to move · ESC to close"), style.textMute,
				DT_SINGLELINE | DT_VCENTER | DT_RIGHT);
			SelectObject(dc, fontBody);
			break;
		}
		case OverlayItemKind::Close: {
			if (hot) FillRectColor(dc, item.row, style.bad);
			SelectObject(dc, fontBold);
			DrawTextIn(dc, item.row, L"✕", hot ? style.text : style.textDim,
				DT_SINGLELINE | DT_VCENTER | DT_CENTER);
			SelectObject(dc, fontBody);
			break;
		}
		case OverlayItemKind::Timing: {
			COLORREF color = style.accent;
			if (state.timingLevel == 1) color = style.warn;
			if (state.timingLevel == 2) color = style.bad;
			// **三行堆叠，副标题不和大数字并排。**
			// 并排那版实测重叠了："12.40 ms" 比 "3.42 ms" 宽，
			// 固定的 x 偏移挡不住 —— 数字宽度会变的东西不能用固定偏移排版。
			SelectObject(dc, fontSmall);
			RECT caption{ PAD, item.row.top + 6, item.row.right - PAD,
				item.row.top + 22 };
			DrawTextIn(dc, caption, OT(state.lang,
				L"DLSSNR GPU 耗时", L"DLSSNR GPU 処理時間", L"DLSSNR GPU time"),
				style.textMute, DT_SINGLELINE);
			SelectObject(dc, fontBig);
			RECT main{ PAD, item.row.top + 20, item.row.right - PAD,
				item.row.top + 54 };
			DrawTextIn(dc, main, state.timingMain.c_str(), color, DT_SINGLELINE);
			SelectObject(dc, fontSmall);
			RECT sub{ PAD, item.row.top + 54, item.row.right - PAD,
				item.row.top + 72 };
			DrawTextIn(dc, sub, state.timingSub.c_str(), style.textMute,
				DT_SINGLELINE);
			SelectObject(dc, fontBody);
			RECT rule{ 0, item.row.bottom - 1, item.row.right, item.row.bottom };
			FillRectColor(dc, rule, style.line);
			rowParity = 0;
			break;
		}
		case OverlayItemKind::Section: {
			SelectObject(dc, fontSmall);
			RECT text{ PAD, item.row.top, item.row.right - PAD, item.row.bottom };
			DrawTextIn(dc, text, item.label, style.accent,
				DT_SINGLELINE | DT_VCENTER);
			SelectObject(dc, fontBody);
			rowParity = 0;
			break;
		}
		case OverlayItemKind::Toggle: {
			if (hot) FillRectColor(dc, item.row, style.hover);
			else if (rowParity & 1) FillRectColor(dc, item.row, style.rowAlt);
			RECT label{ PAD, item.row.top, item.control.left - 8, item.row.bottom };
			DrawTextIn(dc, label, item.label, style.text,
				DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
			const bool on = state.values[item.setting] > 0.5f;
			FillRectColor(dc, item.control, on ? style.accent : style.track);
			// 圆钮
			const int knob = 18;
			const int kx = on ? item.control.right - knob - 2 : item.control.left + 2;
			RECT knobRect{ kx, item.control.top + 2, kx + knob,
				item.control.bottom - 2 };
			FillRectColor(dc, knobRect, style.text);
			++rowParity;
			break;
		}
		case OverlayItemKind::Check: {
			// #79 复选格：小方框 + 组名。点整格翻转（命中认 row）。
			// hover 高亮整格；格子底不按 rowParity 交错（网格里交错显乱）。
			if (hot) FillRectColor(dc, item.row, style.hover);
			const bool on = state.values[item.setting] > 0.5f;
			// 方框：勾 = accent 填充 + 白勾；没勾 = track 描边感（1px line 边）
			FillRectColor(dc, item.control, on ? style.accent : style.track);
			if (on) {
				// 白勾（两笔矩形拼，GDI 无线段抗锯齿就这么画最稳）
				RECT s1{ item.control.left + 3, item.control.top + 7,
					item.control.left + 6, item.control.top + 9 };
				RECT s2{ item.control.left + 5, item.control.top + 5,
					item.control.right - 3, item.control.top + 8 };
				FillRectColor(dc, s1, style.text);
				FillRectColor(dc, s2, style.text);
			}
			RECT label{ item.control.right + 6, item.row.top,
				item.row.right - 2, item.row.bottom };
			DrawTextIn(dc, label, item.label,
				on ? style.text : style.textMute,
				DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
			break;
		}
		case OverlayItemKind::Select: {
			if (hot) FillRectColor(dc, item.row, style.hover);
			else if (rowParity & 1) FillRectColor(dc, item.row, style.rowAlt);
			RECT label{ PAD, item.row.top, item.control.left - 8, item.row.bottom };
			DrawTextIn(dc, label, item.label, style.text,
				DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
			FillRectColor(dc, item.control, style.rowAlt);
			RECT border{ item.control.left, item.control.top, item.control.right,
				item.control.top + 1 };
			FillRectColor(dc, border, style.line);
			int count = 0;
			const wchar_t* const* options =
				OverlayOptionsFor(OverlaySetting(item.setting), count);
			const int value = (std::max)(0, (std::min)(count - 1,
				(int)(state.values[item.setting] + 0.5f)));
			RECT text{ item.control.left + 10, item.control.top,
				item.control.right - 10, item.control.bottom };
			DrawTextIn(dc, text, (options && count) ? options[value] : L"—",
				style.text, DT_SINGLELINE | DT_VCENTER);
			SelectObject(dc, fontSmall);
			DrawTextIn(dc, text, OT(state.lang,
				L"点击切换", L"クリックで切替", L"click to cycle"), style.textMute,
				DT_SINGLELINE | DT_VCENTER | DT_RIGHT);
			SelectObject(dc, fontBody);
			++rowParity;
			break;
		}
		case OverlayItemKind::Slider: {
			if (hot) FillRectColor(dc, item.row, style.hover);
			else if (rowParity & 1) FillRectColor(dc, item.row, style.rowAlt);
			RECT label{ PAD, item.row.top, item.control.left - 8, item.row.bottom };
			DrawTextIn(dc, label, item.label, style.text,
				DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

			const float span = (std::max)(item.maxValue - item.minValue, 1e-6f);
			const float ratio = (std::min)(1.0f, (std::max)(0.0f,
				(state.values[item.setting] - item.minValue) / span));
			// 槽
			RECT track{ item.control.left, cy - 2, item.control.right, cy + 2 };
			FillRectColor(dc, track, style.track);
			// 已填充部分
			const int fillTo = item.control.left +
				(int)((item.control.right - item.control.left) * ratio);
			RECT fill{ item.control.left, cy - 2, fillTo, cy + 2 };
			FillRectColor(dc, fill, style.accent);
			// 圆钮
			RECT knob{ fillTo - 6, cy - 8, fillTo + 6, cy + 8 };
			FillRectColor(dc, knob, style.accent);

			// 数值。**列宽固定**，见 LayoutOverlay 里的说明。
			wchar_t value[32]{};
			if (item.setting == int(OverlaySetting::RenderScale)) {
				_snwprintf_s(value, _TRUNCATE, L"%d%%",
					(int)(state.values[item.setting] * 100.0f + 0.5f));
			} else {
				_snwprintf_s(value, _TRUNCATE, L"%.*f", item.decimals,
					state.values[item.setting]);
			}
			SelectObject(dc, fontSmall);
			RECT valueRect{ item.control.right + 6, item.row.top,
				item.row.right - PAD, item.row.bottom };
			DrawTextIn(dc, valueRect, value, style.textDim,
				DT_SINGLELINE | DT_VCENTER | DT_RIGHT);
			SelectObject(dc, fontBody);
			++rowParity;
			break;
		}
		case OverlayItemKind::Status: {
			// 状态区：和 Info 同一套小字渲染，只是指向 state.status[]
			//（布局在耗时下面，永远显示 —— 见 OverlayState::status 的说明）。
			if (item.infoIndex < 0 ||
				item.infoIndex >= (int)state.status.size()) break;
			const OverlayInfoLine& line = state.status[item.infoIndex];
			SelectObject(dc, fontSmall);
			RECT text{ PAD, item.row.top + 1, item.row.right - PAD,
				item.row.bottom };
			DrawTextIn(dc, text, line.text.c_str(),
				line.warn ? style.warn : style.textMute,
				DT_WORDBREAK);
			SelectObject(dc, fontBody);
			break;
		}
		case OverlayItemKind::Info: {
			if (item.infoIndex < 0 ||
				item.infoIndex >= (int)state.info.size()) break;
			const OverlayInfoLine& line = state.info[item.infoIndex];
			SelectObject(dc, fontSmall);
			RECT text{ PAD, item.row.top + 2, item.row.right - PAD,
				item.row.bottom };
			DrawTextIn(dc, text, line.text.c_str(),
				line.warn ? style.warn : style.textMute,
				DT_WORDBREAK);
			SelectObject(dc, fontBody);
			break;
		}
		}
	}

	// **悬浮提示：这个参数是干什么用的。**
	//
	// 鼠标停在一行上（hotIndex）就画一条说明（文案见 OverlayHintFor）。
	// 提示框画在光标**下方、该行下面**，不会盖住正在看的行；
	// 出面板底就翻到该行上方。
	// （#82 滚动：内容裁剪到这里为止 —— 提示框可以画进标题带。）
	SelectClipRgn(dc, nullptr);
	if (state.hotIndex >= 0 && state.hotIndex < (int)items.size()) {
		const OverlayItem& hot = items[state.hotIndex];
		if (const wchar_t* hint = OverlayHintFor(hot.setting, state.lang)) {
			// **用正文字体（14px），不是 12px 的小字** —— 用户实测第一版
			// "提示很小，根本看不全字"。
			SelectObject(dc, fontBody);
			// 先量高度（按面板内宽折行），再定位。
			// **必须直接 DrawTextW**：DrawTextIn 把 rect 拷进局部变量再画，
			// DT_CALCRECT 量出来的高度就丢在局部变量里 —— 上一版正是这么坏的：
			// measure.bottom 恒为 0，提示框高度永远取下限（实测只有 23px）。
			SetTextColor(dc, style.text);
			RECT measure{ 0, 0, width - 2 * PAD - 24, 0 };
			DrawTextW(dc, hint, -1, &measure,
				DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
			const int boxH = (std::max)(24, int(measure.bottom - measure.top) + 20);
			const int boxW = width - 2 * PAD;
			// 默认放在该行下方；出底就放该行上方
			int boxTop = hot.row.bottom + 2;
			if (boxTop + boxH > (LONG)height) boxTop = hot.row.top - boxH - 2;
			if (boxTop < 0) boxTop = 0;
			RECT box{ PAD, boxTop, PAD + boxW, boxTop + boxH };
			FillRectColor(dc, box, style.tooltipBg);
			// 描边：标准 FrameRect（要一支画刷）
			HBRUSH borderBrush = CreateSolidBrush(style.line);
			FrameRect(dc, &box, borderBrush);
			DeleteObject(borderBrush);
			RECT text{ box.left + 10, box.top + 10, box.right - 10, box.bottom - 10 };
			DrawTextIn(dc, text, hint, style.text, DT_WORDBREAK);
			SelectObject(dc, fontBody);
		}
	}

	// #82 滚动条：内容超高才画。右缘 8px 通道，标题带底下到面板底；
	// 画在内容上面。拇指高 = 可视/内容比例，位置随 scroll。
	// 滚轮是主交互（ProcessInput）；这里是可见的反馈，不做拇指拖拽。
	if (state.maxScroll > 0) {
		const int trackTop = TITLE_H + 2;
		const int trackBottom = (LONG)height - 6;
		if (trackBottom - trackTop > 24) {
			RECT trough{ (LONG)width - 11, trackTop, (LONG)width - 3, trackBottom };
			FillRectColor(dc, trough, style.line);
			const int trackH = trough.bottom - trough.top;
			const int contentH = trackH + state.maxScroll;
			const int thumbH = (std::max)(24, trackH * trackH / contentH);
			const int scroll = (std::min)((std::max)(0, state.scroll), state.maxScroll);
			const int travel = trackH - thumbH;
			const int thumbY = trough.top +
				travel * scroll / (state.maxScroll > 0 ? state.maxScroll : 1);
			RECT thumb{ trough.left, thumbY, trough.right, thumbY + thumbH };
			FillRectColor(dc, thumb, style.textMute);
		}
	}

	// **自己画光标。**
	//
	// 游戏通常把系统光标藏了（SetCursor(nullptr) / ClipCursor / raw input），
	// 而我们吞掉了鼠标输入，游戏也不会再画它自己的准星。不自己画的话用户在
	// 浮层上是瞎点。画一个描边箭头，深浅背景上都看得见。
	if (state.cursor.x >= 0 && state.cursor.y >= 0) {
		const POINT p = state.cursor;
		const POINT arrow[] = {
			{ p.x, p.y }, { p.x, p.y + 16 }, { p.x + 4, p.y + 12 },
			{ p.x + 7, p.y + 18 }, { p.x + 10, p.y + 16 }, { p.x + 7, p.y + 11 },
			{ p.x + 12, p.y + 11 }
		};
		HPEN pen = CreatePen(PS_SOLID, 1, RGB(0x10, 0x10, 0x10));
		HBRUSH brush = CreateSolidBrush(RGB(0xff, 0xff, 0xff));
		HGDIOBJ oldPen = SelectObject(dc, pen);
		HGDIOBJ oldBrush = SelectObject(dc, brush);
		Polygon(dc, arrow, (int)(sizeof(arrow) / sizeof(arrow[0])));
		SelectObject(dc, oldBrush);
		SelectObject(dc, oldPen);
		DeleteObject(brush);
		DeleteObject(pen);
	}

	// DIB 的 32bpp BI_RGB 是 BGRX；alpha GDI 不填，我们补 255（面板不透明）
	const uint8_t* source = static_cast<const uint8_t*>(bits);
	for (uint32_t y = 0; y < height; ++y) {
		const uint8_t* srcRow = source + size_t(y) * OVERLAY_WIDTH * 4;
		uint8_t* dstRow = bgra + size_t(y) * OVERLAY_WIDTH * 4;
		for (uint32_t x = 0; x < width; ++x) {
			dstRow[x * 4 + 0] = srcRow[x * 4 + 0];
			dstRow[x * 4 + 1] = srcRow[x * 4 + 1];
			dstRow[x * 4 + 2] = srcRow[x * 4 + 2];
			dstRow[x * 4 + 3] = 0xFF;
		}
	}

	SelectObject(dc, oldFont);
	DeleteObject(fontBody);
	DeleteObject(fontBold);
	DeleteObject(fontBig);
	DeleteObject(fontSmall);
	SelectObject(dc, oldBitmap);
	DeleteObject(bitmap);
	DeleteDC(dc);
	return true;
}

/* ---------------- 命中测试 ---------------- */

// 面板本地坐标 -> item 下标。-1 = 没命中任何可交互的东西。
// **和绘制读同一份 items**，所以不可能出现"看到的和点到的不一致"。
// 标题栏（右上角那个 X 除外）是拖动手柄。
// 单独一个函数而不是走 OverlayHitTest：标题栏不是"控件"，它不该出现在 hover
// 高亮和点击派发里，但又要能被拖 —— 两件事分开判最省心。
inline bool OverlayInTitleBar(POINT local, uint32_t width) noexcept {
	using namespace overlay_detail;
	return local.y >= 0 && local.y < TITLE_H &&
		local.x >= 0 && local.x < (LONG)width - TITLE_H;
}

// 面板本地坐标 -> item 下标。-1 = 没命中任何可交互的东西。
// viewHeight = 面板高：#82 滚动后 items 里存着滚出可视带的行，
// **只点看得见的那截** —— 命中矩形裁进 [TITLE_H, viewHeight)：
// 滚到标题带底下的行不可点（那里画的是标题），跨带的行只认下方那截，
// 面板底以下的行不可点（光标在面板外时 local 会越界）。
inline int OverlayHitTest(const std::vector<OverlayItem>& items,
	POINT local, uint32_t viewHeight) noexcept {
	for (size_t index = 0; index < items.size(); ++index) {
		const OverlayItem& item = items[index];
		const bool interactive =
			item.kind == OverlayItemKind::Toggle ||
			item.kind == OverlayItemKind::Check ||
			item.kind == OverlayItemKind::Slider ||
			item.kind == OverlayItemKind::Select ||
			item.kind == OverlayItemKind::Close;
		if (!interactive) continue;
		// Close 只认它自己那个小方块（钉顶，不裁）；其余认整行再裁进可视带。
		RECT area =
			item.kind == OverlayItemKind::Close ? item.control : item.row;
		if (item.kind != OverlayItemKind::Close) {
			if (area.bottom <= overlay_detail::TITLE_H) continue;
			if (area.top < overlay_detail::TITLE_H) area.top = overlay_detail::TITLE_H;
			if (viewHeight && area.top >= (LONG)viewHeight) continue;
			if (viewHeight && area.bottom > (LONG)viewHeight) {
				area.bottom = (LONG)viewHeight;
			}
		}
		if (local.x >= area.left && local.x < area.right &&
			local.y >= area.top && local.y < area.bottom) {
			return (int)index;
		}
	}
	return -1;
}

// 滑条取值的量化 + 夹紧（拖动相对位移的那条路也要走同一份）
inline float OverlaySliderQuantize(const OverlayItem& item, float value) noexcept {
	if (item.step > 0.0f) {
		value = item.minValue +
			item.step * float((int)((value - item.minValue) / item.step + 0.5f));
	}
	return (std::min)(item.maxValue, (std::max)(item.minValue, value));
}

// 滑条：本地 x -> 取值（已按 step 量化并夹紧）。
// **只该在光标确实按在槽上时用** —— x 在槽左边（标签那半行）会被钳到最小值。
inline float OverlaySliderValueAt(const OverlayItem& item, int localX) noexcept {
	const int left = item.control.left;
	const int right = item.control.right;
	if (right <= left) return item.minValue;
	const float ratio = (std::min)(1.0f, (std::max)(0.0f,
		float(localX - left) / float(right - left)));
	return OverlaySliderQuantize(item,
		item.minValue + ratio * (item.maxValue - item.minValue));
}

}  // namespace DXL
