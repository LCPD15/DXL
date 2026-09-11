'use strict';

// UI 侧维护整个配置模型（主题、快捷键、每游戏配置文件），宿主不解析它 —— 见
// main.cpp 顶部的说明。
//
// 但 core 需要一份**扁平**的设置：它被注入到游戏进程里，越少依赖越安全，所以它
// 只读 `profiles\<游戏exe名>.json` 这种平铺的键值文件。因此 Apply 时我们做两件事：
//   1. settings.json  <- 整个模型，原样交给宿主写
//   2. profiles/*.json <- 每个配置展平后的设置，core 读这个
// 展平这一步只在这里做，core 和宿主都不需要知道 profile 的存在。

const OPTIONS = {
	srMode: [
		['dlaa', 'DLAA — 原生分辨率，只提升抗锯齿/稳定性'],
		['upscale', '真超分 — 压低游戏渲染分辨率再放大']
	],
	// DLSS 的质量档本质就是渲染分辨率百分比
	srQuality: [
		['quality', 'Quality（67%）'],
		['balanced', 'Balanced（58%）'],
		['performance', 'Performance（50%）'],
		['ultra_perf', 'Ultra Performance（33%）'],
		['custom', '自定义…']
	],
	// Debug views inspect the active NR route.
	nrDebugView: [
		[0, '关（正常画面）'],
		[1, '深度（近亮远暗，看轮廓层次）'],
		[2, '运动矢量（中灰不动 / 红=右 / 绿=下）'],
		[3, '编码输入（Evaluate）'],
		[4, '差值（Evaluate）']
	],

};

// DLSS5 / DLSSNR 参数。每一项都直接对应 nvngx_dlssnr.dll 的一个参数键
// （见 README 的对照表），core 侧一一透传，中间没有翻译层。
//
// **含义来自 NVIDIA 自己的新闻稿**（nvidia.cn/geforce/news/dlss-5-3d-guided-
// neural-rendering/），那篇里放出的开发者面板和这些键是一一对应的：
//   面板 GLOBAL CONTROLS 的 Structure Intensity -> LocalStructureStrength
//   面板 GLOBAL CONTROLS 的 Tone Intensity      -> LocalToneStrength
//   面板的 MODELS（Model A/B/C）                 -> Hint.Render.Preset
//   面板的 MODEL AUTOMASK                        -> UseAutoMask
// 原来这些标签只是英文键名照抄，玩家没法知道该动哪个。
const NR_PARAMS = [
	{ key: 'nrPreset', label: '模型（Render Preset）', type: 'select', options: [[0, '0 Default'], [1, '1 Preset #1'], [2, '2 Preset #2'], [3, '3 Preset #3']],
	  hint: '不同权重的模型，输出风格不一样。对应官方面板里的 Model A / B / C。' },
	{ key: 'nrStyle', label: '风格（Style）', type: 'select', options: [[0, '0 Default'], [1, '1 Natural'], [2, '2 Cinematic']] },
	{ key: 'nrIntensity', label: '总强度（Intensity）', type: 'range', min: 0, max: 1, step: 0.05,
	  hint: '归零时画面和不开一样，但**开销照付**（神经图是固定的，开销只跟像素数走）。' },
	{ key: 'nrLocalTone', label: '色调强度（Tone）', type: 'range', min: 0, max: 2, step: 0.05,
	  hint: '管**低频**：大范围的光照和色彩响应。' },
	{ key: 'nrLocalStructure', label: '结构强度（Structure）', type: 'range', min: 0, max: 2, step: 0.05,
	  hint: '管**高频**：环境光遮蔽、接触阴影、反射、次表面散射这类细节。' },
	{ key: 'nrSkinStructure', label: '皮肤结构强度（Skin）', type: 'range', min: 0, max: 2, step: 0.05,
	  hint: '单独控制皮肤上的结构强度。0 = 不单独干预皮肤。' },
	{ key: 'nrAutoMask', label: '语义自动遮罩（Auto Mask）', type: 'switch',
	  hint: '让模型**自己识别**画面里的角色和环境，只增强环境、不动角色。' +
	        '不需要我们提供任何东西 —— 识别是模型内部做的。' },
	{ key: 'nrUiCorrection', label: 'UI 修正（UI Correction）', type: 'switch',
	  hint: '⚠ **现在开了没用。** 它要的是单独的 UI 图层（DLSSNR.UI + UIAlpha 两张图），' +
	        '而我们是后处理注入，拿不到游戏把 UI 合成之前的那一层。' }
];

// 一份配置里的功能设置。core 读的就是这些键。
const SETTING_DEFAULTS = {
	srEnable: false,
	srMode: 'dlaa',
	// **纯 UI 侧的标记，core 不读它。** core 报过一次"这个游戏无视我们改的
	// backbuffer 尺寸"就置上，之后配置页永久锁掉放大档位（见 updateVisibility）。
	// 存进配置是刻意的：这是引擎属性，不是这一局的偶然。
	srProxyUnsupported: false,
	srQuality: 'quality',
	srInputMultiplier: 0.67,
	sharpness: 0.0,

	injectTiming: 'auto',
    fgBufferCountThreshold: 4,

	// DLSSNR（这个工具的主功能）。**默认开** —— 玩家装这个工具就是为了它，
	// 让他每个游戏都手动勾一次没有意义。
	dlss5Enable: true,
	// 旁听游戏自己的 DLSS 取真矢量。core 侧的键名是 diagEavesdrop —— 展平时会改名，
	// 见 writeProfileFile。
	//
	// **默认开**（用户拍的板）。它是 DLSSNR 画质的上限所在 —— 真矢量决定时域累积，
	// 而旁听是注入式工具唯一拿到游戏原生矢量的途径。早期它把画面弄坏过一次，
	// 后来在鬼武者/街霸 6 上实测能稳定拿到矢量，所以默认打开；真出问题在配置页
	// 关掉这一项即可。
	eavesdrop: true,
	nrAutoRoute: true,
	dlss5AtEvaluate: true,
	// 总开关（游戏里 Del 那个）。**默认关**，由玩家进游戏自己按一下打开 ——
	// 按过之后这里会被写成他选的值，下一局就自动照做。
	// core 侧读的键名就是 masterEnabled，只在**启动时**取一次（之后以 Del 为准）。
	masterEnabled: false,
	// A/B 开关 + debug 视图。都是即时生效的（core 收到 ReloadSettings 就换），
	// 因为它们不重建纹理也不重建 NGX feature。
	nrUseRealMotion: true,
	nrUseRealDepth: true,
	nrDebugView: 0,
	nrDebugGain: 0,
	// 降分辨率跑 DLSSNR。1 = 全分辨率。开销只跟像素数走，这是唯一能省性能的旋钮。
	nrRenderScale: 1.0,
	nrColourStrength: 1.0,
	nrSelfLayers: 1.0,
	nrTrueLayers: 1,
	nrOpticalFlow: true,
	nrOpticalFlowQuality: 1,
	nrSemanticMask: false,
	nrSemOn: 1,
	nrSemBgInt: 1,
	nrSemanticDebugView: false,
	nrSemanticFlipY: false,
	nrSemanticFeather: 8,
	...Object.fromEntries(Array.from({length: 18}, (_, i) => ['nrSemInt' + i, 0])),

	nrPreset: 0,
	// 新游戏默认（用户拍板）：电影风格（Style 2 = Cinematic）+ skin 0.6。
	// 这里的默认对**没存过这个键的配置**都生效 —— 已在浮层里调过参数的配置
	// 在 profile.settings / core 的 params.json 里有自己的值，不受影响。
	nrStyle: 2,
	// 范围（用户拍的板）：intensity 0~1；tone/structure/skin 0~2 默认 1。
	// skin 旧默认 -1（"不干预"）已弃 —— 0 就是不单独干预，负值没有额外含义。
	nrIntensity: 1.0,
	nrLocalTone: 1.0,
	nrLocalStructure: 1.0,
	nrSkinStructure: 0.6,
	nrAutoMask: true,
	nrUiCorrection: true
};

const MODEL_DEFAULTS = {
	theme: 'dark',
	// 'auto' = 首启按系统语言解析（见 initLanguage）；选过一次就存具体值。
	lang: 'auto',
	// **只列真的实现了的。** 为不存在的功能长期占住一个全系统独占的组合键没道理。
	// （这条注释以前写着"游戏内 overlay 还没做"，现在做出来了 —— 见 toggleOverlay。）
	hotkeyRevision: 0,
    injectHotkeyVersion: 0,
	hotkeys: {
		// NR 和面板键由游戏内 core 消费，支持单键且不占用其他程序的编辑键。
		toggleInject: 'Alt + F8',
		toggleAll: 'Del',
		// Debug 视图循环。当初撤掉是因为功能不存在，现在深度/矢量可视化真做出来了 ——
		// 而它的用处全在"边动边切"上，走界面点应用画面早就过去了。
		toggleDebugView: 'Alt + V',
		// 游戏内浮层。**这一个不走 RegisterHotKey** —— 它由注入进游戏的 core
		// 用窗口过程自己响应。两边都注册的话按一下会触发两次（开了又关）。
		// 传给 core 的方式见 writeProfileFile 里的 overlayVk / overlayMods。
		toggleOverlay: 'End'
	},
	// 全局监控：列表里的游戏一启动就自动注入。**默认开**——
	// 玩家忘了从工具启动是最常见的失败方式（拿不到原生矢量，效果静默变差），
	// 而这条兜底的代价只是一个每 200ms 扫一次进程表的线程。
	// 注意它仍然是**赛跑**，赢不赢不保证；要百分百拿到原生矢量还是用列表里的 ▶。
	watchGames: true,
	// 已扫过一次已安装的游戏（第一次启动扫完由 onScannedGames 落 true，
	// 之后启动跳过自动扫描——用户删掉的 .exe 不再被加回来）。
	scanned: false,
	profiles: [
		{
			id: 'default', name: '默认', exe: '', exePath: '', args: '',
			// pinned / lastLaunch 决定侧栏列表的顺序（置顶优先，其余按最近启动）
			pinned: false, lastLaunch: 0, settings: {}
		}
	]
};

let saved = structuredClone(MODEL_DEFAULTS);   // 磁盘上的
let draft = structuredClone(MODEL_DEFAULTS);   // 编辑中的
let activeId = 'default';
let lastStatus = {};
let registeredInjectKey = null; // null: awaiting native registration; empty: registration failed.
let currentPage = 'library';
let libraryFilter = 'all';
const libraryMetadataAsked = new Map();

/* ---------------- 模型辅助 ---------------- */

function activeProfile() {
	return draft.profiles.find(p => p.id === activeId) || draft.profiles[0];
}

// 严格匹配：只有真的对上才返回配置，对不上返回 null。
// "正在运行"的高亮必须用这个 —— runningProfile() 会退回当前选中的配置，
// 拿它做高亮的话，游戏不在列表里时会给一个假的"正在运行"。
function matchProfileByTarget() {
	const target = String(lastStatus.target || '').toLowerCase();
	if (!target) return null;
	return findProfileForExe(target) || null;
}

// 按 exe 名找配置，四种写法都认：exe 字段 / exePath 的尾段 / id 带或不带 .exe。
//
// **默认配置(id==='default')只做最后的兜底。** 它排在数组最前面，而且玩家常把
// 它指到某个游戏上当"全局默认"（demo 就是这么被指到鬼武者上的）—— 不降级的
// 话 find() 永远先命中它：启动游戏后选中跳回 demo、参数写进 demo，全是这一条
// 根因（实测：启动 onimushawots_demo.exe，onAttached 选中的是 default）。
function findProfileForExe(target) {
	const bare = target.replace(/\.exe$/, '');
	const isMatch = p => {
		const exe = String(p.exe || '').toLowerCase();
		const fromPath = String(p.exePath || '').toLowerCase()
			.split(/[\\/]/).pop() || '';
		const id = String(p.id || '').toLowerCase();
		return exe === target || fromPath === target ||
			id === target || id === bare;
	};
	return draft.profiles.find(p => p.id !== 'default' && isMatch(p)) ||
		draft.profiles.find(isMatch) || null;
}

// 当前正在跑的那份配置。宿主报的 `target` 是 **exe 文件名**（"Game.exe"），
// 不是 profile.id —— 早先直接拿 id 去比，于是"正在运行"的高亮几乎从来没亮过。
//
// 匹配放宽到四种写法（见 matchProfileByTarget）：只比 `exe` 是不够的，手工建的
// 配置可能没填这个字段，那时会静静地退回"当前选中的配置"，于是 Del 的状态
// 被存到了另一份配置上（实测踩过）。
// 找不到就退回当前选中的那份 —— 调用方要能接受这个退让。
function runningProfile() {
	return matchProfileByTarget() || activeProfile();
}

// core 报"这个游戏无视了我们改的 backbuffer 尺寸"时的善后。
//
// core 自己会撤掉代理把画面救回来（那是当场的事），这里做的是**下一局的事**：
// 把这份配置的超分模式改回 DLAA。不改的话玩家下次启动又是同一张裁切画面，
// 而这个游戏无论重启多少次都不会变 —— 它是引擎行为，不是时机问题。
let proxyIgnoredHandled = false;

function handleProxyIgnored() {
	if (proxyIgnoredHandled) return;
	proxyIgnoredHandled = true;
	const profile = runningProfile();
	const settings = resolvedSettings(profile);
	profile.settings = profile.settings || {};
	// **把"这个引擎不支持"记成一个持久标记**，不只是改回 DLAA。
	// 只改 srMode 的话玩家下次自己又选了放大档位 —— 而这在这个引擎上永远
	// 不可能对。有了这个标记，配置页会把放大档位直接锁掉并说明原因。
	const already = Boolean(profile.settings.srProxyUnsupported);
	profile.settings.srProxyUnsupported = true;
	if (settings.srMode === 'upscale') profile.settings.srMode = 'dlaa';
	if (!already) {
		appendLog('「' + (profile.name || profile.id) + '」不支持工具侧的真超分：' +
			'这个游戏按自己设置里的分辨率渲染，不理会我们改的 backbuffer 尺寸，' +
			'放大档位在它上面只会给出裁切放大的画面。已改回 DLAA 并锁掉放大档位' +
			'（重启游戏后生效）。想省性能只能用游戏自己的分辨率/画质设置。');
	}
	saved = structuredClone(draft);
	host.post('applySettings', draft);
	writeProfileFile(profile);
	markClean();
	renderAll();
}

// 玩家在游戏里按了 Del：把这次选择存进**正在跑的那份配置**。
// core 下次启动时从 masterEnabled 取初值，于是"按过一次就记住"。
function onMasterToggled(on) {
	const profile = runningProfile();
	profile.settings = profile.settings || {};
	if (Boolean(profile.settings.masterEnabled) === on) return;
	profile.settings.masterEnabled = on;
	appendLog('总开关：' + (on ? 'ON' : 'OFF') + ' —— 已记进配置「' +
		(profile.name || profile.id) + '」，下次启动自动照做。');
	// 直接落盘，不走"有未应用的修改"那条路：玩家现在人在游戏里，
	// 他不可能切回来点一下"应用"，而这个值的全部意义就是下一局还在。
	//
	// **只写这一份 profile，不走 persistAll。** persistAll 会给 35 份配置各写一次
	// 文件、每次都让 core 重读一遍设置 —— 玩家正在游戏里按键，那是一串没必要的卡顿。
	// core 启动时读的就是 profiles\<exe>.json 里的 masterEnabled，写它就够了。
	saved = structuredClone(draft);
	host.post('applySettings', draft);
	writeProfileFile(profile);
	markClean();
	renderAll();
}

// 一份配置的完整设置 = 默认值 + 它自己存的差异
function profileExeBasename(profile) {
    if (!profile || profile.id === 'default') return '';
    return String(profile.exePath || profile.exe || '').split(/[\\/]/).pop().toLowerCase();
}
function defaultFgThreshold(profile) {
    return profileExeBasename(profile) === 'yysls.exe' ? 8 : 4;
}
// Official REFramework README + exact GameIdentity table, checked 2026-09-10:
// https://github.com/praydog/REFramework/blob/b6baf6b406efc65e077b99cb4d9ad25b0a0a9095/shared/sdk/GameIdentity.cpp
// Provisional entries and upstream's substring fallback are deliberately absent.
// This identifies where to show setup instructions, not a DXL compatibility claim.
const REFRAMEWORK_EXECUTABLES = new Set([
    're2.exe', 'bhd2.exe', 're3.exe', 'bhd3.exe', 're4.exe', 'bhd4.exe',
    're7.exe', 'bhd7.exe', 're8.exe', 'village.exe', 're9.exe',
    'devilmaycry5.exe', 'dmc5.exe', 'monsterhunterrise.exe', 'mhrise.exe',
    'mhrisesunbreakdemo.exe', 'streetfighter6.exe', 'sf6.exe', 'dd2.exe',
    'drdr.exe', 'makaimura_gg_re.exe', 'makaimura.exe', 'gs456.exe',
    'kunitsugami.exe', 'onimusha2.exe', 'monsterhunterwilds.exe', 'mhwilds.exe',
    'starforce.exe', 'megamanstarforce.exe', 'mmstarforce.exe',
    'onimushawots.exe', 'onimushawots_demo.exe'
]);
function needsReFrameworkSetup(profile) {
    return REFRAMEWORK_EXECUTABLES.has(profileExeBasename(profile));
}
function renderProfileCompatibility(profile) {
    document.getElementById('reFrameworkSetup').hidden = !needsReFrameworkSetup(profile);
    document.getElementById('zenlessDx12Setup').hidden = profileExeBasename(profile) !== 'zenlesszonezero.exe';
    const tuned = defaultFgThreshold(profile) === 8;
    document.getElementById('fgThresholdSummary').textContent = tuned
        ? '高级：FG 检测阈值（燕云特调默认 8）' : '高级：FG 检测阈值';
    document.getElementById('fgThresholdScope').textContent = tuned
        ? '燕云十六声特调默认 8，仅本游戏配置，重启游戏生效。'
        : '默认 4，仅本游戏配置，重启游戏生效。';
}
function resolvedSettings(profile) {
	const settings = Object.assign(structuredClone(SETTING_DEFAULTS), profile.settings || {});
    settings.fgBufferCountThreshold = normalizeFgThreshold(
        profile.settings?.fgBufferCountThreshold, defaultFgThreshold(profile));
    return settings;
}

// core 只按 exe 名找文件，所以 id 就是小写的 exe 名（默认配置用 default）
// core 找配置文件的规则（`SettingsReader::FindPath`）是**小写的完整 exe 名 + .json**：
//     profiles\streetfighter6.exe.json
// 找不到就退回 profiles\default.json。
//
// **这里必须用同一条规则，不能用 profile.id。** id 是"去掉 .exe 的名字"
// （`onExePicked` / `onScannedGames` 都 `replace(/\.exe$/, '')`），于是扫描或
// 手动添加出来的配置会被写成 `streetfighter6.json` —— 那个文件 core 永远不读，
// 玩家在界面上改的所有设置都静默不生效，而两边都不报错。
// 只有早期那些 id 里带 `.exe` 的老配置碰巧对上了。
function profileFileName(profile) {
	if (profile.id === 'default') return 'default.json';
	const exe = profileExeBasename(profile);
	if (exe) return exe + '.json';
	// 没填 exe 的配置（新建还没选文件）—— core 反正找不到它，用 id 保持稳定
	return profile.id + '.json';
}

/* ---------------- 与宿主通信 ---------------- */

const host = {
	post(type, payload, extra) {
		const message = Object.assign({ type, payload }, extra || {});
		if (window.chrome && window.chrome.webview) {
			window.chrome.webview.postMessage(JSON.stringify(message));
		} else {
			console.log('[no host]', message);
		}
	}
};

function onHostMessage(raw) {
	let msg;
	try {
		msg = typeof raw === 'string' ? JSON.parse(raw) : raw;
	} catch {
		return;
	}
	switch (msg.type) {
        case 'update':
            window.DxlUpdate?.receive(msg.payload);
            break;
		case 'settings':
			loadModel(msg.payload || {});
			break;
		case 'status':
			setStatus(msg.payload);
			break;
		case 'attached':
			onAttached(msg.payload || {});
			break;
		case 'hotkeysRegistered':
            registeredInjectKey = msg.payload?.toggleInject || '';
            renderHotkeyHints();
            renderAdvice(lastStatus);
            break;
		case 'extensions':
            renderExtensions(msg.payload || {});
            break;
		case 'launcherPreferences':
			onLauncherPreferences(msg.payload || {});
			break;
		case 'log':
			appendLog(msg.payload);
			break;
		case 'exePicked':
			onExePicked(msg.payload || {});
			break;
		case 'icons':
			// 宿主把 exe 图标抠成 PNG 放进 web/iconcache/，这里只收 url。
			// **走文件 + 虚拟主机，不走 base64** —— 图标是 64x64 甚至更大，
			// 十几个配置的 base64 会让消息膨胀好几百 KB。
			//
			// **只有真的多了图标才重画列表。** 无条件重画会和
			// renderProfileList() 末尾的 requestMissingIcons() 组成死循环，
			// 后果远不止费电，见那个函数上面的大段注释。
			onIcons(msg.payload || {});
			break;
		case 'dxVersions':
			// #36：宿主后台线程（DxScanWorker）扫完 exe 补发的 DX 版本判定。
			// 单独一条消息的原因：全文件扫描几百毫秒，卡 UI 线程会冻住界面。
			onDxVersions(msg.payload || {});
			break;
		case 'libraryMetadata':
			onLibraryMetadata(msg.payload || {});
			break;
		case 'gameCover':
			onGameCover(msg.payload || {});
			break;
		case 'scannedGames':
			onScannedGames(msg.payload || {});
			break;
		case 'invalidGames':
			onInvalidGames(msg.payload || {});
			break;
		case 'masterToggled':
			// 玩家在游戏里按了 Del。宿主已经把命令发给 core 了，这里只负责**存盘**：
			// 下一局 core 会从配置里的 masterEnabled 取初值，自动照玩家上次的选择来。
			onMasterToggled(Boolean(msg.payload && msg.payload.on));
			break;
	}
}

if (window.chrome && window.chrome.webview) {
	window.chrome.webview.addEventListener('message', e => onHostMessage(e.data));
}

/* ---------------- 未捕获异常要说出来 ---------------- */

// **JS 抛异常在这个宿主里是完全静默的。** WebView2 没有可见的控制台，
// 而一个异常会让当前这次事件处理半途而废 —— 症状是"点了没反应"，
// 界面上一点提示都没有，日志里也没有。查这种问题只能靠猜。
//
// 所以把未捕获异常同时送到两个地方：界面的日志页，和宿主的 ui-*.log 文件
// （后者能在工具外面读，界面自己坏掉时还看得到）。
function reportUiError(what, detail) {
	const line = '界面脚本出错：' + what + ' —— ' + detail;
	try { appendLog(line); } catch (_) { /* 日志区都没了就算了 */ }
	host.post('uiError', null, { text: line });
}

window.addEventListener('error', event => {
	const stack = event.error && event.error.stack ? ' | ' + event.error.stack : '';
	reportUiError(
		String(event.message || '未知错误'),
		(event.filename || '?') + ':' + (event.lineno || 0) + ':' +
			(event.colno || 0) + stack);
});

window.addEventListener('unhandledrejection', event => {
	const reason = event.reason;
	reportUiError('未处理的 Promise 拒绝',
		reason && reason.stack ? reason.stack : String(reason));
});

// 已经删掉的设置项。这批控件不存在了，core 也不读，但磁盘上的旧文件里还躺着 ——
// Object.assign 会把它们原样留下，于是每次 Apply 又写回去，永远清不掉。
// 留着不只是脏：哪天新增的键和它们撞名，就会读到一个几个月前的值。
//
// **只删这张明确的清单，不删"所有不认识的键"。** 上一版是后者，结果把手写进
// profiles\*.json 的诊断键（diagNrDumpPixels 之类）也一并擦掉了 —— 用户在界面上点一次
// 应用，正在排查的那个开关就没了，而且没有任何提示。core 侧的诊断键本来就是刻意
// 不在 UI 上暴露的，UI 没有资格删它们。
const DEAD_SETTING_KEYS = [
	'dlssVersion', 'depthMode', 'motionMode',
	'perfWindow', 'depthDebug', 'motionDebug',

];

function pruneDeadSettings(settings) {
	if (!settings || typeof settings !== 'object') return {};
	const clean = Object.assign({}, settings);
	for (const key of DEAD_SETTING_KEYS) delete clean[key];
	// **范围钳制：旧数据免疫。** skin 的旧默认 -1（"不干预"）在新范围 0~2 之外 ——
	// 存量配置里躺着的话，每次自动存盘都会把它原样写回扁平文件，core 读到的
	// 永远是 -1（用户实测：工具里改 skin，游戏里永远弹回 -1）。这里在**载入**
	// 时就丢掉越界值，让它落回默认 —— 迁移脚本只治存量，这一条治根。
	const range = { nrIntensity: [0, 1], nrLocalTone: [0, 2], nrLocalStructure: [0, 2], nrSkinStructure: [0, 2], nrColourStrength: [0, 1], nrSelfLayers: [1, 3], nrTrueLayers: [1, 5], nrOpticalFlowQuality: [0, 2] };
	for (const [key, [min, max]] of Object.entries(range)) {
		if (!(key in clean)) continue;
		const v = Number(clean[key]);
		if (!Number.isFinite(v) || v < min - 1e-6 || v > max + 1e-6) delete clean[key];
	}
	for (const key of ['nrTrueLayers', 'nrOpticalFlowQuality']) {
		if (key in clean) clean[key] = Math.round(Number(clean[key]));
	}
    if ('fgBufferCountThreshold' in clean) {
        const n = Number(clean.fgBufferCountThreshold);
        if (Number.isInteger(n) && n >= 2 && n <= 16) clean.fgBufferCountThreshold = n;
        else delete clean.fgBufferCountThreshold; // invalid values use this game's default
    }
	return clean;
}

function normalizeFgThreshold(value, fallback = 4) {
    const n = Number(value);
    return Number.isInteger(n) && n >= 2 && n <= 16 ? n : fallback;
}

function loadModel(payload) {
	saved = Object.assign(structuredClone(MODEL_DEFAULTS), payload);
    const migrateFgDefaults = !saved.fgDefaultsVersion;
	if (!Array.isArray(saved.profiles) || !saved.profiles.length) {
		saved.profiles = structuredClone(MODEL_DEFAULTS.profiles);
	}
	for (const profile of saved.profiles) {
		profile.settings = pruneDeadSettings(profile.settings);
		if (profile.pinned === undefined && profile.favorite !== undefined) profile.pinned = Boolean(profile.favorite);
	}
    saved.fgDefaultsVersion = 1;
	saved.hotkeys = Object.assign(
		structuredClone(MODEL_DEFAULTS.hotkeys), saved.hotkeys || {});
	// 磁盘上可能还躺着早期版本的快捷键名（toggleDebug / togglePerf 之类）。
	// 不在当前清单里的一律删掉，别让它们赖在配置里假装是个功能。
	for (const action of Object.keys(saved.hotkeys)) {
		if (!(action in MODEL_DEFAULTS.hotkeys)) delete saved.hotkeys[action];
	}
    // Migrate the previous manual-injection default once, without replacing a
    // custom binding or conflicting with another existing action.
    const bindingKey = value => String(value || '').replace(/\s+/g, '').toLowerCase();
    if (!saved.injectHotkeyVersion && bindingKey(saved.hotkeys.toggleInject) === 'alt+9' &&
        !Object.entries(saved.hotkeys).some(([action, combo]) => action !== 'toggleInject' && bindingKey(combo) === 'alt+f8'))
        saved.hotkeys.toggleInject = 'Alt + F8';
    saved.injectHotkeyVersion = 1;
	// Only migrate the two previous stock defaults; keep users' custom bindings.
    if (!saved.hotkeyRevision && String(saved.hotkeys.toggleAll).replace(/\s+/g, '').toLowerCase() === 'alt+d')
        saved.hotkeys.toggleAll = 'Del';
    if (!saved.hotkeyRevision && String(saved.hotkeys.toggleOverlay).replace(/\s+/g, '').toLowerCase() === 'alt+0')
        saved.hotkeys.toggleOverlay = 'End';
    saved.hotkeyRevision = Number.isSafeInteger(saved.hotkeyRevision) && saved.hotkeyRevision >= 0
        ? saved.hotkeyRevision : 0;
	draft = structuredClone(saved);
	if (!draft.profiles.some(p => p.id === activeId)) activeId = draft.profiles[0].id;
	// 语言要在第一次 renderAll 之前定好（renderAll 末尾会按它套翻译）。
	// 放在 loadModel 里而不是页面加载时：draft.lang 从磁盘配置来。
	initLanguage();
	renderAll();
	markClean();
	pushHotkeys();
	// 监控列表和开关都要在读完配置之后同步给宿主 —— 宿主重启后不记得这两件事。
	pushWatchList();
	if (draft.watchGames) host.post('setWatch', null, { on: 1 });
    // The model stores explicit overrides; old flattened files also contained
    // stock defaults. Only a missing model override proves the old 4 was a
    // generated default. Preserve every explicit value, including 4.
    if (migrateFgDefaults) {
        for (const profile of draft.profiles) {
            if (defaultFgThreshold(profile) === 8 &&
                !Object.hasOwn(profile.settings, 'fgBufferCountThreshold')) writeProfileFile(profile, true);
        }
        host.post('applySettings', draft, {noReload: 1});
    }

	// **只在第一次启动（配置里还没有 scanned 标记）时自动扫一次。**
	//
	// 以前是每次启动都扫：玩家把不想管的 .exe 从列表里删掉，下次启动
	// 扫描又把它加回来（按 exe 路径去重只认"配置里还有没有"，不认"删过"）。
	// 现在扫描完在 settings.json 里落一个 scanned: true，之后启动直接跳过；
	// 想重新扫用手动按钮（⟳），那是玩家自己的动作，加回来怪不了别人。
	// 只在第一次读到配置时判断；loadModel 在"放弃修改"之类的路径上还会被调到。
	if (!autoScanDone && !draft.scanned) {
		autoScanDone = true;
		autoScanQuiet = true;
		host.post('scanGames');
	}
}

// 启动时自动扫描只做一次；quiet 表示这一次的结果不要在日志里啰嗦
let autoScanDone = false;
let autoScanQuiet = false;

// 注入成功后宿主会告诉我们是哪个游戏。**先按 exe 名找已有的配置去选中** ——
// 找不到才新建。早先直接拿 `id = exe.toLowerCase()` 去比：扫描建出来的配置
// id 是去掉 .exe 的（"streetfighter6"），和这里比不上的话就会**复制出第二份**，
// 于是列表顶上出现两个街霸 6。列表排序本来就会把启动过的顶上去，
// 选中已有的那份就够了，不需要复制。
function onAttached(info) {
	const exe = info.exe || '';
	if (!exe) return;
	const target = exe.toLowerCase();
	// 优先命中非 default 的配置 —— 见 findProfileForExe 里的说明
	let profile = findProfileForExe(target);
	if (!profile) {
		profile = {
			id: target,
			name: exe.replace(/\.exe$/i, ''),
			exe,
			exePath: info.path || '',
			args: '',
			settings: {}
		};
		draft.profiles.push(profile);
		appendLog('已自动为 ' + exe + ' 新建配置');
		// 立刻落盘，别让玩家因为忘了点 Apply 而丢掉
		saved = structuredClone(draft);
		host.post('applySettings', draft);
		writeProfileFile(profile);
	} else if (!profile.exePath && info.path) {
		profile.exePath = info.path;
	}
	activeId = profile.id;
	renderAll();
	showPage('profile');
	markDirty();
}

// "Alt + 0" -> { vk, mods }。mods 的位：1=Alt 2=Ctrl 4=Shift。
//
// **翻译放在 UI 侧。** core 是注入到游戏进程里的，越少解析人类可读的字符串越好；
// 而这个组合键的写法本来就是 UI 自己定的格式。
function parseHotkeyForCore(text) {
    let mods = 0, vk = 0;
    const names = { del: 0x2e, delete: 0x2e, end: 0x23, ins: 0x2d, insert: 0x2d,
        home: 0x24, pageup: 0x21, pgup: 0x21, pagedown: 0x22, pgdn: 0x22,
        space: 0x20, tab: 9, enter: 13, backspace: 8,
        arrowleft: 0x25, left: 0x25, arrowup: 0x26, up: 0x26,
        arrowright: 0x27, right: 0x27, arrowdown: 0x28, down: 0x28 };
    for (const raw of String(text || '').split('+')) {
        const token = raw.trim().toLowerCase();
        if (token === 'alt') { mods |= 1; continue; }
        if (token === 'ctrl' || token === 'control') { mods |= 2; continue; }
        if (token === 'shift') { mods |= 4; continue; }
        if (token === 'win' || token === 'meta') { mods |= 8; continue; }
        let key = names[token] || 0;
        if (/^f([1-9]|1[0-9]|2[0-4])$/.test(token)) key = 0x70 + Number(token.slice(1)) - 1;
        else if (/^[0-9a-z]$/.test(token)) key = token.toUpperCase().charCodeAt(0);
        if (!key || vk) return { vk: 0, mods: 0 };
        vk = key;
    }
    return { vk, mods };
}

// noReload = **这份文件的内容来自 core**（浮层改的参数随状态块报回来）。
// 这时只落盘、**不要**让 core 重读 —— 它刚报的就是这个值，重读不但多余，
// 还会在玩家继续拖的半路上把（落盘那一下的）旧值压回 core，把他正往 1.5 拖的
// 参数打回 1.2。方向只有一个：core -> 文件 落盘即可；UI -> 文件 才带 ReloadSettings。
function writeProfileFile(profile, noReload) {
	// core 只认扁平文件里的固定键名。UI 上叫 eavesdrop，core 侧的诊断键叫
	// diagEavesdrop —— 在这里改名，别让 core 去理解 UI 的命名。
	const flat = resolvedSettings(profile);
    // Missing means "use the host executable's current default" to core. Do
    // not flatten a generated default into an apparent user override again.
    if (!Object.hasOwn(profile.settings || {}, 'fgBufferCountThreshold')) delete flat.fgBufferCountThreshold;
	flat.diagEavesdrop = Boolean(flat.eavesdrop);
	delete flat.eavesdrop;
	// 浮层的快捷键和语言是**全局设置**，但 core 只读 per-exe 的扁平文件 ——
	// 所以每份都带一遍。不带的话 core 会用它自己的默认值（End / 中文），
	// 而界面上显示的是用户改过的那个，两边就对不上了。
    const effect = parseHotkeyForCore(draft.hotkeys && draft.hotkeys.toggleAll);
    const overlay = parseHotkeyForCore(draft.hotkeys && draft.hotkeys.toggleOverlay);
    if (effect.vk) {
        flat.hkEnable = effect.vk;
        flat.hkEnableMods = effect.mods;
    }
    if (overlay.vk) {
        flat.hkUi = flat.overlayVk = overlay.vk;
        flat.hkUiMods = flat.overlayMods = overlay.mods;
    }
    flat.hotkeyRevision = Number(draft.hotkeyRevision) || 0;

	// 浮层语言跟着语言下拉走（0 中 / 2 英；日语已删，'ja' 落到 0=中文）。
	// 切语言 -> markDirty -> 自动存盘 -> 每份配置都写一遍 -> core 重读 -> 灌给浮层。
	flat.overlayLang = draft.lang === 'en' ? 2 : 0;
	host.post('applyProfile', flat,
		{ file: profileFileName(profile), noReload: noReload ? 1 : 0 });
}

function pushHotkeys() {
    host.post('registerHotkeys', null, draft.hotkeys);
}
function renderHotkeyHints() {
    const tr = window.I18N ? I18N.t : x => x;
    const hint = document.getElementById('hintInjectKey');
    if (hint) hint.textContent = registeredInjectKey === null ? tr('等待快捷键注册…') : registeredInjectKey || tr('快捷键不可用');
    const panel = document.getElementById('panelHotkeyHint');
    if (panel) panel.textContent = tr(`只读显示 —— 请在游戏内 ImGui 面板调参（${draft.hotkeys.toggleOverlay} 打开）。`);
    const master = document.getElementById('masterHotkeyHint');
    if (master) master.textContent = tr(`游戏内总开关快捷键：${draft.hotkeys.toggleAll}。控制本工具的全部效果，保留注入。开关会保存到本配置；游戏运行时修改会立即生效。`);
}


/* ---------------- 构建控件 ---------------- */

function fillSelect(el, pairs) {
	el.innerHTML = '';
	for (const [value, text] of pairs) {
		const opt = document.createElement('option');
		opt.value = String(value);
		opt.textContent = text;
		el.appendChild(opt);
	}
}

for (const [id, pairs] of Object.entries(OPTIONS)) {
	const el = document.getElementById(id);
	if (el) fillSelect(el, pairs);
}

// 参数面板已删（用户拍板，见 index.html 里的注释）：游戏内浮层是唯一调参入口。
// 这个函数留着空转 —— #nrParams 不存在时直接返回，别让加载时 null 引用。
function buildNrParams() {
	const root = document.getElementById('nrParams');
	if (!root) return;
	root.innerHTML = '';
	for (const p of NR_PARAMS) {
		const row = document.createElement('div');
		row.className = 'row';

		const label = document.createElement('label');
		label.className = 'row-label';
		label.textContent = p.label;
		label.setAttribute('for', p.key);
		row.appendChild(label);

		if (p.type === 'switch') {
			const wrap = document.createElement('label');
			wrap.className = 'switch';
			wrap.innerHTML =
				`<input type="checkbox" id="${p.key}" data-key="${p.key}"><span class="slider-track"></span>`;
			row.appendChild(wrap);
		} else {
			const field = document.createElement('div');
			field.className = 'row-field';
			if (p.type === 'select') {
				const sel = document.createElement('select');
				sel.id = p.key;
				sel.dataset.key = p.key;
				fillSelect(sel, p.options);
				field.appendChild(sel);
			} else {
				const rng = document.createElement('input');
				rng.type = 'range';
				rng.id = p.key;
				rng.dataset.key = p.key;
				rng.min = p.min;
				rng.max = p.max;
				rng.step = p.step;
				const out = document.createElement('output');
				out.className = 'row-value';
				out.id = p.key + 'Out';
				field.append(rng, out);
			}
			row.appendChild(field);
		}
		root.appendChild(row);

		// 说明单独一行，不塞进 title 属性 —— 悬停提示在这种"我该动哪个"的场景里
		// 等于没有：玩家得先猜到该把鼠标放上去。
		if (p.hint) {
			const hint = document.createElement('p');
			hint.className = 'param-hint';
			hint.innerHTML = p.hint;
			root.appendChild(hint);
		}
	}
}
buildNrParams();

/* ---------------- 渲染 ---------------- */

function formatValue(key, value) {
	if (key === 'srInputMultiplier') return Number(value).toFixed(2) + 'x';
	if (key === 'nrDebugGain') {
		return Number(value) > 0 ? Number(value).toFixed(1) : '默认';
	}
	if (key === 'nrRenderScale') {
		// **只回一个短字符串。**
		//
		// 这里原来回的是"90%（像素数 81%）"，长度随取值变 —— 而读数就排在滑块
		// 右边、滑块是 flex:1 1 auto。于是"文字变长 -> 滑块变短 -> 滑块下的鼠标
		// 落在了另一个刻度上 -> 取值又变 -> 文字再变长"，从 100% 往下拉的时候
		// 会在 95%/90% 之间反复横跳，根本停不下来。
		// **读数的宽度绝不能依赖它自己的内容。** 像素数那半挪到 #nrScaleNote 里，
		// 那是一整行的说明文字，长度变化不影响任何控件的几何。
		return Math.round(Number(value) * 100) + '%';
	}
	if (key === 'fgBufferCountThreshold' || key === 'nrPreset' || key === 'nrStyle') return String(Math.round(value));
	return Number(value).toFixed(2);
}

// 按开关/模式显示或禁用相关行，避免出现"设了但不生效"的控件
function updateVisibility() {
	const settings = resolvedSettings(activeProfile());
	for (const row of document.querySelectorAll('[data-requires]')) {
		const on = Boolean(settings[row.dataset.requires]);
		row.classList.toggle('is-muted', !on);
		for (const ctl of row.querySelectorAll('input, select')) {
			// 被"游戏自己开着 DLSS"锁死的开关不许在这里被解开 ——
			// 这两个判据是独立的，谁都不能覆盖谁（见 setStatus 里的 conflict）。
			if (ctl.dataset.blockedByGameDlss) continue;
			ctl.disabled = !on;
		}
	}
	for (const row of document.querySelectorAll('[data-when-sr-mode]')) {
		row.hidden = settings.srMode !== row.dataset.whenSrMode;
	}
	for (const row of document.querySelectorAll('[data-when-sr-quality]')) {
		row.hidden = settings.srMode !== 'upscale' ||
			settings.srQuality !== row.dataset.whenSrQuality;
	}
	// **这个引擎不吃我们的真超分时，把放大档位锁掉。**
	//
	// 判据是 core 报过 proxySizeIgnored（见 handleProxyIgnored）。这类引擎
	// （RE Engine / 街霸 6 实测）按自己设置里的分辨率渲染，压根不读 swapchain
	// 的尺寸，所以"放大"这个选项在它上面**永远**是一张裁切放大的画面。
	// 留着让玩家反复踩没有意义 —— 直接不给选，并说清楚为什么。
	const srModeSelect = document.getElementById('srMode');
	const srUnsupported = Boolean(settings.srProxyUnsupported);
	// 按下标遍历，不用 for...of —— `options` 在真浏览器里可迭代，但这段代码也要
	// 能在无头假件上跑（scripts/test-status-render.js），那边它只是个数组。
	if (srModeSelect && srModeSelect.options) {
		for (let i = 0; i < srModeSelect.options.length; i++) {
			const option = srModeSelect.options[i];
			option.disabled = srUnsupported && option.value === 'upscale';
		}
	}
	const srNote = document.getElementById('srProxyNote');
	if (srNote) {
		srNote.hidden = !srUnsupported;
	}
	// 把滑块换算成**预估耗时**，而不是只显示一个百分比。
	// 那个百分比对"我该往下拉多少"毫无指导意义；这两行数字才是决策依据。
	// 拟合来自实测：842x454=2.68ms、1920x1080=6.92ms -> 固定 1.7ms + 2.5ms/兆像素。
	const note = document.getElementById('nrScaleNote');
	if (note) {
		const scale = Number(settings.nrRenderScale) || 1;
		const guess = (w, h) => {
			const mpx = (w * scale) * (h * scale) / 1e6;
			return (1.7 + 2.5 * mpx).toFixed(1);
		};
		const area = Math.round(scale * scale * 100);
		// 开销跟**像素数**走，不跟边长走：只看边长的话 70% 看起来像"省 30%"，
		// 实际省的是 51%。这句以前在滑块读数里，因为会改变宽度挪到了这里。
		const areaText = scale >= 0.999 ? ''
			: '边长 ' + Math.round(scale * 100) + '% = <b>像素数 ' + area +
				'%</b>（开销跟像素数走）。';
		note.innerHTML = areaText + (scale >= 0.999
			? '现在是全分辨率。按实测拟合：1080p 约 <b>' + guess(1920, 1080) +
				'ms</b>，1440p 约 <b>' + guess(2560, 1440) + 'ms</b>，4K 约 <b>' +
				guess(3840, 2160) + 'ms</b>（60fps 的预算是 16.7ms）。'
			: '预估耗时：1080p 约 <b>' + guess(1920, 1080) + 'ms</b>，1440p 约 <b>' +
				guess(2560, 1440) + 'ms</b>，4K 约 <b>' + guess(3840, 2160) +
				'ms</b>。这是估算 —— <b>底部那条常驻读数是这台机器的真实耗时</b>，'
				+ '以它为准。');
	}
}

/* ---------------- 配置列表 ---------------- */

// exe 图标的 url，宿主抠好 PNG 之后回填。key = profile.id
let iconUrls = {};
// **已经问过宿主的 id，问过就不再问**，哪怕没要到图标。
//
// 这一条是防死循环，不是省流量：宿主收到 requestIcons 一定会回一条 icons，
// 但抠不出来的 id **不在回包里**。早先"收到 icons 就重画列表"，而重画列表末尾
// 又会去问缺的图标，于是只要有一个 exe 抠不出图标（路径失效、exe 里根本没有
// 图标资源……），它就永远留在"还缺"里：
//     渲染列表 → 问图标 → 收到回包 → 渲染列表 → …
// 按 IPC 的速度无限转。实测 33 个配置里只差 1 个，空转就吃掉宿主 65% 的一个核、
// WebView 一整个核。而且**列表 DOM 每轮都被重建**，mousedown 和 mouseup 落在
// 不同元素上，浏览器根本不派发 click —— 表现就是"点配置行毫无反应，一直停在
// 启动时选中的那个"。查这种问题别盯着 click 处理函数，它是好的。
let iconAsked = new Set();
// 正在改名 / 正在等删除确认的那一行。同时只允许一个，省掉一堆状态互斥的麻烦。
let renamingId = null;
let confirmDeleteId = null;
let profileFilter = '';

const ROW_ICONS = {
	play: 'M4.5 3.2 12.5 8l-8 4.8V3.2Z',
	rename: 'M2.5 13.5h11M4 11.2 11.2 4l1.6 1.6L5.6 12.8l-1.6.2v-1.8Z',
	// 置顶：箭头**朝上**。第一版画反了（三角在下），看着像"下移"。
	pin: 'M8 13.5V8M5 8h6L8 2.5 5 8Z',
	del: 'M3.5 5h9M6 5V3.5h4V5m-5 0 .6 8h4.8L11 5'
};

function svgIcon(path) {
	return '<svg viewBox="0 0 16 16" aria-hidden="true"><path d="' + path +
		'" fill="none" stroke="currentColor" stroke-width="1.5" ' +
		'stroke-linecap="round" stroke-linejoin="round"/></svg>';
}

// 置顶的在前；其余按**最后启动时间**倒序；都没启动过的按名字。
// 玩家找的几乎总是"上次玩的那个"，按名字排等于每次都要重读一遍列表。
// **不改 draft.profiles 的实际顺序** —— 那个数组的顺序对应磁盘，排序只是显示。
function sortedProfiles() {
	const keyword = profileFilter.trim().toLowerCase();
	return draft.profiles
		.filter(p => {
			if (p.id === 'default') return false;
			if (libraryFilter === 'favorites' && !p.pinned) return false;
			if (!['all', 'favorites'].includes(libraryFilter) && gameSource(p) !== libraryFilter) return false;
			if (!keyword) return true;
			return (p.name || '').toLowerCase().includes(keyword) ||
				(p.exe || '').toLowerCase().includes(keyword) ||
				(p.exePath || '').toLowerCase().includes(keyword);
		})
		.slice()
		.sort((a, b) => {
			if (Boolean(a.pinned) !== Boolean(b.pinned)) return a.pinned ? -1 : 1;
			const la = Number(a.lastLaunch) || 0;
			const lb = Number(b.lastLaunch) || 0;
			if (la !== lb) return lb - la;
			return String(a.name || a.id).localeCompare(String(b.name || b.id));
		});
}

function requestMissingIcons() {
	// 宿主按 id 把 PNG 缓存在 web/iconcache/ 下，所以只有还没拿到 url 的才要问。
	//
	// **分隔符必须是可打印字符。** 第一版用了 \x1f / \x1e（ASCII 的单元/记录
	// 分隔符），看起来最"正确"，实际上完全不工作：JSON.stringify 会把控制字符
	// 转义成 ``，而宿主那个**刻意不解析 JSON** 的取值器只是取引号之间的
	// 原文，拿到的是字面量的六个字符 —— split 一个都分不出来，于是一个图标都没有。
	// `|` 和 `*` 在 Windows 文件名里非法，路径里不可能出现，JSON 也不会动它们。
	const pending = draft.profiles.filter(
		p => p.exePath && !iconUrls[p.id] && !iconAsked.has(p.id));
	if (!pending.length) return;
	for (const p of pending) iconAsked.add(p.id);
	host.post('requestIcons', null, {
		items: pending.map(p => p.id + '|' + p.exePath).join('*')
	});
}

// 收到宿主回来的图标。**只在真的多了东西时才重画列表** —— 见上面那段。
// DX 版本不再走这条消息：全文件扫描要几百毫秒，宿主把它挪去了后台线程
// （DxScanWorker），走单独的 dxVersions 消息补发。
function onIcons(payload) {
	let added = false;
	for (const [id, url] of Object.entries(payload || {})) {
		if (!url || iconUrls[id] === url) continue;
		iconUrls[id] = url;
		added = true;
	}
	if (added) renderProfileList();
}

// #36：后台 DX 版本判定的补发（宿主 DxScanWorker）。
// 每项是 { dx, arch }——两个都识别不出时宿主不会发这一项。
function onDxVersions(payload) {
	let added = false;
	for (const [id, info] of Object.entries(payload || {})) {
		if (!info || typeof info !== 'object') continue;
		if (dxVersions[id] && dxVersions[id].dx === info.dx &&
			dxVersions[id].arch === info.arch) continue;
		dxVersions[id] = { dx: info.dx || '', arch: info.arch || '' };
		added = true;
	}
	// 足迹：宿主后台扫描补发的结果落进 ui-*.log —— 这条链路（后台线程 →
	// WM_APP_DX_DONE → PostToUi → 渲染）哪一环断了，靠这个说话。
	host.post('uiError', null, {
		text: `[DX] 后台判定回来：${Object.keys(payload || {}).length} 项`
			+ `（新增 ${added ? Object.keys(payload || {}).length : 0}）`
	});
	if (added) { renderProfileList(); renderHomeBadges(); }
}

// #39：主页顶部选中游戏信息区的 DX / 位数徽标（用户指定放这里 ——
// 列表行里名字长时徽标仍然容易看不见，主页这块是专门给它留的位置）。
function renderHomeBadges() {
	const root = document.getElementById('homeBadges');
	if (!root) return;
	root.innerHTML = '';
	const info = dxVersions[activeProfile().id];
	if (!info) return;
	if (info.dx) {
		const b = document.createElement('span');
		b.className = 'dx-badge';
		b.textContent = info.dx;
		b.title = '从游戏 exe 里识别到的图形 API（启发式：'
			+ '按文件里有没有 d3d11/d3d12 的引用判定）';
		root.appendChild(b);
	}
	if (info.arch) {
		const b = document.createElement('span');
		b.className = 'dx-badge';
		b.textContent = info.arch;
		b.title = '游戏 exe 的位数（PE 头 Machine 字段）';
		root.appendChild(b);
	}
}

// 配置的 exe 路径变了就得重新问一次，否则改完路径图标永远停在首字母块。
// DX 版本（#36）跟着同一条路：路径变了旧判定就作废了。
function forgetIcon(id) {
	iconAsked.delete(id);
	delete iconUrls[id];
	delete dxVersions[id];
	libraryMetadataAsked.delete(id);
}

// id -> "DX12" / "DX11" / "DX11/12"。随 icons 一起回来（onIcons），
// 显示在列表图标旁边。识别不出就没有这一项 —— 不硬写"未知"。
const dxVersions = {};

function iconNode(profile) {
	const url = iconUrls[profile.id];
	if (url) {
		const img = document.createElement('img');
		img.className = 'profile-icon';
		img.src = url;
		img.alt = '';
		return img;
	}
	// 还没有图标（没填 exe，或者抠失败）就用首字母块，别留个空洞
	const box = document.createElement('span');
	box.className = 'profile-icon is-letter';
	box.textContent = (profile.name || profile.id).trim().slice(0, 1).toUpperCase();
	return box;
}

// GTA SA Definitive Edition fails during startup hooks on the tested installation.
// Its window-settled path reaches Present and successfully executes NR.
// Explicit Early/Late settings remain authoritative.
function resolvedInjectionTiming(profile, source = 'launch') {
    const setting = resolvedSettings(profile).injectTiming;
    if (setting === 'late' || setting === 'early') return setting;
    // External processes are already initializing; only a tool-owned launch can
    // hold the primary thread while installing hooks. Automatic watch waits.
    if (source === 'watch') return 'late';
    const exe = String(profile.exe || profile.exePath || '').split(/[\\/]/).pop().toLowerCase();
    return exe === 'sanandreas.exe' ? 'late' : 'early';
}

function launchProfile(profile) {
	if (!profile.exePath) {
		appendLog('「' + (profile.name || profile.id) + '」还没设置 exe 路径。');
		activeId = profile.id;
		renderAll();
		return;
	}
	// 记下启动时间：列表就是靠这个把最近玩过的顶到前面
	profile.lastLaunch = Date.now();
	activeId = profile.id;
	// Flush the model before starting: an immediate launch must use and save the
	// latest arguments even when the ordinary autosave timer has not fired yet.
	persistAll();
	const timing = resolvedInjectionTiming(profile);
	appendLog(timing === 'late'
		? '注入时机：延迟（可能无法捕获原生 DLSS）'
		: '注入时机：尽早（优先捕获原生 DLSS 和矢量）');
	host.post('launch', null, {
		exePath: profile.exePath,
		args: profile.args || '',
		timing
	});
	renderAll();
}

function gameSource(profile) {
	const source = String(profile.source || '').toLowerCase();
	if (source.includes('steam')) return 'Steam';
	if (source.includes('epic')) return 'Epic';
	if (source.includes('gog')) return 'GOG';
	if (!source && /[\\/]steamapps[\\/]common[\\/]/i.test(profile.exePath || '')) return 'Steam';
	return 'Manual';
}

function mergeLibraryMetadata(profile, info) {
	let changed = false;
	for (const key of ['source', 'steamAppId', 'coverUrl']) {
		if (!Object.prototype.hasOwnProperty.call(info, key)) continue;
		if (key === 'coverUrl' && profile.coverCustom) continue;
		const value = String(info[key] || '');
		if (key === 'source' && !['Steam', 'Epic', 'GOG', 'Manual'].includes(value)) continue;
		if (key === 'steamAppId' && value && !/^\d+$/.test(value)) continue;
		if (key === 'coverUrl' && value && !validCoverUrl(value)) continue;
		if (profile[key] !== value) { profile[key] = value; changed = true; }
	}
	return changed;
}

function requestLibraryMetadata() {
	const pending = draft.profiles.filter(p => p.id !== 'default' && p.exePath &&
		libraryMetadataAsked.get(p.id) !== p.exePath);
	if (!pending.length) return;
	for (const p of pending) libraryMetadataAsked.set(p.id, p.exePath);
	host.post('requestLibraryMetadata', null, {items: pending.map(p => p.id + '|' + p.exePath).join('*')});
}

function onLibraryMetadata(payload) {
	let changed = false;
	for (const [id, info] of Object.entries(payload)) {
		const profile = draft.profiles.find(p => p.id === id);
		if (!profile || !info || typeof info !== 'object') continue;
		if (info.exePath && String(info.exePath).toLowerCase() !== String(profile.exePath).toLowerCase()) continue;
		changed = mergeLibraryMetadata(profile, info) || changed;
	}
	if (changed) { renderProfileList(); markDirty(); }
}

function validCoverUrl(value) {
	return /^https:\/\/[^\s]+$/i.test(value) || /^covercache\/[a-z0-9._/-]+$/i.test(value);
}

function onGameCover(info) {
	const profile = draft.profiles.find(p => p.id === info.id);
	if (!profile || !info.coverUrl || !validCoverUrl(info.coverUrl)) return;
	if (Object.prototype.hasOwnProperty.call(info, 'exePath') &&
		String(info.exePath || '').toLowerCase() !== String(profile.exePath || '').toLowerCase()) return;
	profile.coverUrl = info.coverUrl;
	profile.coverCustom = true;
	renderProfileList();
	markDirty();
}

function coverCandidates(profile) {
	const candidates = [];
	if (profile.coverUrl && validCoverUrl(profile.coverUrl)) candidates.push(profile.coverUrl);
	if (/^\d+$/.test(String(profile.steamAppId || ''))) {
		const base = 'https://shared.fastly.steamstatic.com/store_item_assets/steam/apps/' + profile.steamAppId + '/';
		candidates.push(base + 'library_600x900.jpg', base + 'header.jpg');
	}
	return [...new Set(candidates)];
}

function openGameDetails(profile) {
	activeId = profile.id;
	renderAll();
	showPage('profile');
}

function buildProfileRow(profile) {
	const tr = window.I18N ? (I18N.log || I18N.t) : x => x;
	const row = document.createElement('article');
	row.className = 'profile-row' + (profile.id === activeId ? ' is-active' : '');
	const art = document.createElement('button');
	art.type = 'button';
	art.className = 'game-art';
	art.setAttribute('aria-label', tr('查看游戏详情') + ' ' + (profile.name || profile.id));
	const fallback = document.createElement('div');
	fallback.className = 'game-art-fallback';
	fallback.appendChild(iconNode(profile));
	const fallbackName = document.createElement('span');
	fallbackName.className = 'game-art-name';
	fallbackName.textContent = profile.name || profile.id;
	fallback.appendChild(fallbackName);
	art.appendChild(fallback);
	const sources = coverCandidates(profile);
	if (sources.length) {
		const cover = document.createElement('img');
		cover.className = 'game-cover';
		cover.alt = '';
		cover.loading = 'lazy';
		cover.decoding = 'async';
		let next = 0;
		const advance = () => {
			if (next >= sources.length) { cover.hidden = true; return; }
			const url = sources[next++];
			cover.classList.toggle('is-wide', /\/header\.jpg(?:\?|$)/.test(url));
			cover.src = url;
		};
		cover.addEventListener('error', advance);
		advance();
		art.appendChild(cover);
	}
	art.addEventListener('click', () => openGameDetails(profile));
	row.appendChild(art);
	const main = document.createElement('button');
	main.type = 'button';
	main.className = 'profile-main';
	const name = document.createElement('span');
	name.className = 'profile-name';
	name.textContent = profile.name || profile.id;
	main.appendChild(name);
	main.title = profile.name || profile.id;
	main.addEventListener('click', () => openGameDetails(profile));
	const footer = document.createElement('div');
	footer.className = 'game-footer';
	footer.appendChild(main);
	const actions = document.createElement('div');
	actions.className = 'profile-actions';
	// Platform is in the group heading; the compact card shows title and actions only.
	const favorite = document.createElement('button');
	favorite.type = 'button';
	favorite.className = 'icon-btn favorite-btn' + (profile.pinned ? ' is-on' : '');
	favorite.textContent = profile.pinned ? '★' : '☆';
	favorite.title = tr(profile.pinned ? '取消收藏' : '收藏');
	favorite.setAttribute('aria-label', favorite.title + ' ' + (profile.name || profile.id));
	favorite.setAttribute('aria-pressed', String(Boolean(profile.pinned)));
	favorite.addEventListener('click', () => {
		profile.pinned = !profile.pinned;
		renderProfileList();
		markDirty();
	});
	actions.appendChild(favorite);
	const play = document.createElement('button');
	play.type = 'button';
	play.className = 'play-btn';
	play.title = tr('启动游戏并注入');
	play.setAttribute('aria-label', play.title + ' ' + (profile.name || profile.id));
	play.innerHTML = svgIcon(ROW_ICONS.play);
	play.disabled = !profile.exePath;
	play.addEventListener('click', () => { launchProfile(profile); showPage('profile'); });
	actions.appendChild(play);
	footer.appendChild(actions);
	row.appendChild(footer);
	return row;
}

function renderProfileList() {
	const root = document.getElementById('profileList');
	if (!root) return;
	root.innerHTML = '';
	const tr = window.I18N ? (I18N.log || I18N.t) : x => x;
	const list = sortedProfiles();
	const total = draft.profiles.filter(p => p.id !== 'default').length;
	document.getElementById('libraryCount').textContent = list.length + ' / ' + total + ' · ' + tr('款游戏');
	if (!list.length) {
		const empty = document.createElement('div');
		empty.className = 'profile-empty';
		empty.textContent = tr(libraryFilter === 'favorites' ? '还没有收藏，点击游戏卡片上的星标即可添加。' :
			profileFilter ? '没有匹配的游戏' : '添加游戏或重新扫描，开始建立你的游戏库。');
		root.appendChild(empty);
	}
	for (const source of ['Favorites', 'Manual', 'Steam', 'Epic', 'GOG']) {
		const games = list.filter(p => source === 'Favorites' ? p.pinned : !p.pinned && gameSource(p) === source);
		if (!games.length) continue;
		const group = document.createElement('section');
		group.className = 'library-group';
		const heading = document.createElement('h2');
		heading.className = 'library-group-heading';
		heading.textContent = (source === 'Favorites' ? tr('收藏') : source === 'Manual' ? tr('手动添加') : source) + ' · ' + games.length;
		group.appendChild(heading);
		const grid = document.createElement('div');
		grid.className = 'game-grid';
		for (const profile of games) grid.appendChild(buildProfileRow(profile));
		group.appendChild(grid);
		root.appendChild(group);
	}
	requestMissingIcons();
	requestLibraryMetadata();
}

function renderAll() {
	const profile = activeProfile();
	const settings = resolvedSettings(profile);

	for (const el of document.querySelectorAll('[data-key]')) {
		const key = el.dataset.key;
		if (!(key in settings)) continue;
		if (el.type === 'radio') {
            el.checked = el.value === String(settings[key]);
        } else if (el.type === 'checkbox') {
			el.checked = Boolean(settings[key]);
		} else {
			el.value = String(settings[key]);
			if (el.type === 'range') {
				const out = document.getElementById(key + 'Out');
				if (out) out.textContent = formatValue(key, settings[key]);
			}
		}
	}

	document.getElementById('profileName').value = profile.name || '';
	document.getElementById('profileExePath').value = profile.exePath || '';
	for (const id of ['profileArgs', 'homeLaunchArgs']) {
		const input = document.getElementById(id);
		if (input.value !== (profile.args || '')) input.value = profile.args || '';
	}
	document.getElementById('homeTitle').textContent = profile.name || profile.id;
    renderProfileCompatibility(profile);
	document.getElementById('sidebarCurrentGame').textContent = profile.name || profile.id;
	document.getElementById('changeGameCover').disabled = profile.id === 'default';
	document.getElementById('homeSub').textContent =
		profileExeBasename(profile) || '未关联任何游戏';
	renderHomeBadges();
	document.getElementById('settingsFor').textContent =
		'当前配置：' + (profile.name || profile.id);
	document.getElementById('deleteProfile').disabled = profile.id === 'default';

	const watchToggle = document.getElementById('watchGames');
	if (watchToggle) watchToggle.checked = Boolean(draft.watchGames);

	// 主页那句提示里的快捷键要跟着设置变，否则改了快捷键之后它就在骗人
	renderHotkeyHints();
    renderAdvice(lastStatus);

	applyTheme(draft.theme);
	renderProfileList();
	updateVisibility();
	for (const [action, combo] of Object.entries(draft.hotkeys)) {
		const btn = document.querySelector(`[data-hotkey="${action}"]`);
		if (btn) btn.textContent = combo;
	}
	// 动态生成的文本（上面这些 textContent / renderProfileList 的行）
	// 也要走一遍翻译 —— 生成的是普通文本节点，同一条路。
	// **zh 也要走**：三向互译表里英文键也指向中文值，切回中文靠这条路。
	if (window.I18N && draft.lang && draft.lang !== 'auto') I18N.apply();
}

/* ---------------- 编辑 ---------------- */

document.addEventListener('input', e => {
	const el = e.target;
	const key = el.dataset && el.dataset.key;
	if (!key) return;
	// **输入写进哪份配置，必须和 liveApplyNow 写的是同一份。**
	// 两个各找各的（input→activeProfile / liveApply→matchProfileByTarget）时，
	// 拖滑块把值写进 A 配置，推给 core 的却是 B 配置的旧值 —— 38 次 ReloadSettings
	// 全带旧值的根（core-86252 / ui-48600 两局实测）。
	if (el.type === 'radio' && !el.checked) return;
    const profile = (key === 'injectTiming' || key === 'fgBufferCountThreshold')
        ? activeProfile() : matchProfileByTarget() || activeProfile();
	profile.settings = profile.settings || {};

	if (el.type === 'checkbox') {
		profile.settings[key] = el.checked;
	} else if (el.type === 'range') {
		profile.settings[key] = key === 'fgBufferCountThreshold'
            ? normalizeFgThreshold(el.value, defaultFgThreshold(profile)) : parseFloat(el.value);
		const out = document.getElementById(key + 'Out');
		if (out) out.textContent = formatValue(key, profile.settings[key]);
	} else {
		// 下拉菜单的值一律当字符串存 —— 之前把 "310.7" 转成数字，写进
		// settings.json 就变成了 310.7，读回来对不上原来的选项
		const raw = el.value;
		profile.settings[key] = /^-?\d+$/.test(raw) ? parseInt(raw, 10) : raw;
	}
	// 输入环节的足迹。排查"改了没生效"两轮查不出断点，就是因为这条路静默 ——
	// 写盘那条有日志了，这条也要有：一次拖动三行日志（输入/写盘/重读），
	// 断在哪一环一眼看出。走 uiError 通道（宿主会落进 ui-*.log 文件）。
	host.post('uiError', null, {
		text: `[输入] ${key}=${el.value} -> 配置「${profile.name || profile.id}」`
	});
	updateVisibility();
	markDirty();
    if (key === 'injectTiming') pushWatchList();
	scheduleLiveApply(key);
});

/* ---------------- 实时生效（不用每次点应用） ---------------- */

// **哪些键改了要立刻推给 core。**
//
// 分两类是有原因的：
//   便宜的（强度/风格/局部色调/结构/皮肤/遮罩/debug 视图）—— core 收到
//     ReloadSettings 只是换几个参数值，不重建纹理也不重建 NGX feature，随便推。
//   贵的（处理分辨率）—— 要等我们自己的命令列表跑完、释放纹理、重建 feature，
//     一次几百毫秒。所以它**只在 change（滑块松手）时推**，不跟着 input 推。
// 其余的（srEnable / injectTiming / exe 路径）根本不适合热改：前者要重建
// swapchain 代理，后者只在启动时读 —— 那些仍然走「应用」。
const LIVE_CHEAP_KEYS = new Set([
	'nrIntensity', 'nrLocalTone', 'nrLocalStructure', 'nrSkinStructure',
	'nrPreset', 'nrStyle', 'nrAutoMask', 'nrUiCorrection',
	'nrDebugView', 'nrDebugGain', 'nrUseRealMotion', 'nrUseRealDepth',
	'sharpness', 'nrColourStrength', 'nrSelfLayers', 'nrOpticalFlow',
	'nrAutoRoute', 'dlss5AtEvaluate'
]);
const LIVE_EXPENSIVE_KEYS = new Set(['nrRenderScale', 'nrTrueLayers', 'nrOpticalFlowQuality']);

let liveApplyTimer = 0;

// 只写 profiles\*.json + 让 core 重读，**不动 saved**。
//
// 不动 saved 是刻意的：底部那条"有未应用的修改"仍然会亮，玩家知道这些值还没落进
// settings.json（也就是重启工具之后会丢）。实时生效解决的是"看效果要点五次应用"，
// 不是替代保存。
function liveApplyNow() {
	liveApplyTimer = 0;
	// **目标 = 正在运行的那份配置**（matchProfileByTarget），不是当前选中的。
	// activeProfile() 和它对不上时（点了别的配置 / 附加时自动选择没生效），
	// 拖滑块写的是另一份配置的文件，core 读的游戏那份永远只有旧值 ——
	// 38 次 ReloadSettings 全带旧值的根（core-86252 实测）。
	// 退回 activeProfile 只在"没有游戏在跑"时发生（改了也没人读）。
	writeProfileFile(matchProfileByTarget() || activeProfile());
}

function scheduleLiveApply(key) {
	if (!LIVE_CHEAP_KEYS.has(key)) return;
	// 防抖 120ms：拖滑块时 input 每帧都来，每次都写文件 + 走一次 IPC 太浪费。
	// 120ms 短到手感上还是"实时"，又能把一次拖动收敛成几次。
	if (liveApplyTimer) clearTimeout(liveApplyTimer);
	liveApplyTimer = setTimeout(liveApplyNow, 120);
}

// 贵的那类只在松手时推一次。range 的 change 事件正好是松手才触发。
document.addEventListener('change', e => {
	const key = e.target && e.target.dataset && e.target.dataset.key;
	if (key && LIVE_EXPENSIVE_KEYS.has(key)) liveApplyNow();
	// 总开关是特例：core 只在**第一次读设置**时取 masterEnabled（core.cpp 的
	// diagnosticsRead 那段），之后以 Del / 命令为准 —— 所以写文件 +
	// ReloadSettings 对正在跑的游戏**不生效**，运行中只能走 SetEnabled 命令。
	// 文件那份是给下一局启动用的。
	if (key === 'masterEnabled') {
		host.post('setMaster', null, { on: e.target.checked ? 1 : 0 });
	}
});

for (const [id, field] of Object.entries(
	{ profileName: 'name', profileExePath: 'exePath', profileArgs: 'args', homeLaunchArgs: 'args' })) {
	document.getElementById(id).addEventListener('input', e => {
		const profile = activeProfile();
		profile[field] = e.target.value;
		if (field === 'args') {
			const other = document.getElementById(id === 'profileArgs' ? 'homeLaunchArgs' : 'profileArgs');
			if (other.value !== profile.args) other.value = profile.args;
		}
		if (field === 'name') renderAll();
		// 换了 exe 就得重新抠图标，否则图标永远停在旧游戏的（或首字母块）。
		// 这里不立刻重画：路径是一个字符一个字符敲出来的，每敲一下都去问一次
		// 图标既没意义又吵。renderAll / 下一次列表重画时自然会问。
		if (field === 'exePath') {
            forgetIcon(profile.id);
            // Refresh only compatibility advice while typing; do not disturb
            // focus/caret or start metadata requests for each partial path.
            renderProfileCompatibility(profile);
            const fg = resolvedSettings(profile).fgBufferCountThreshold;
            document.getElementById('fgBufferCountThreshold').value = String(fg);
            document.getElementById('fgBufferCountThresholdOut').textContent = String(fg);
            if (window.I18N) I18N.apply();
        }
		markDirty();
	});
}

// 8. 新建配置 = **选文件**，不是选进程。
//
// 选进程等于鼓励迟到注入，而迟到注入拿不到游戏的原生运动矢量 ——
// 那正是这个工具画质的上限所在。所以入口直接是"挑 exe，然后从工具启动"。
document.getElementById('newProfile').addEventListener('click', () => {
	host.post('pickExe');
});

// 宿主选完文件回来
function onExePicked(info) {
	const exePath = String(info.exePath || '');
	if (!exePath) return;
	const exe = String(info.exe || '').toLowerCase();
	// id 用 exe 名（去掉 .exe）—— core 就是按 exe 名找 profiles\<name>.json 的。
	// 已经有同 exe 的配置就直接选中它，不要造出两份同名配置互相覆盖。
	const baseId = exe.replace(/\.exe$/, '').replace(/[^a-z0-9._-]/g, '_') || 'game';
	const existing = draft.profiles.find(p => p.id === baseId);
	if (existing) {
		existing.exePath = exePath;
		existing.exe = exe;
		activeId = existing.id;
		forgetIcon(existing.id);
		appendLog('已有同一个 exe 的配置「' + (existing.name || existing.id) +
			'」，已更新它的路径并选中。');
	} else {
		draft.profiles.push({
			id: baseId,
			name: String(info.suggestedName || '').trim() || baseId,
			exe,
			exePath,
			args: '',
			pinned: false,
			lastLaunch: 0,
			settings: {}
		});
		activeId = baseId;
	}
	renderAll();
	markDirty();
}

// 7. 扫描已安装的游戏（Steam / Epic / GOG）
document.getElementById('scanGames').addEventListener('click', () => {
	autoScanQuiet = false;
	host.post('scanGames');
	appendLog('正在扫描…装了很多游戏的话要几秒。');
});


// Check in the native host, then remove only the exact profile/path pair checked.
// Keeping separate saved/draft copies avoids applying unrelated pending edits.
const cleanupGamesButton = document.getElementById('cleanupInvalidGames');
cleanupGamesButton.addEventListener('click', () => {
    if (cleanupGamesButton.disabled) return;
    const items = draft.profiles.filter(p => p.id !== 'default' && p.exePath)
        .map(p => p.id + '|' + p.exePath).join('*');
    cleanupGamesButton.disabled = true;
    host.post('cleanupInvalidGames', null, { items });
    appendLog('正在检查游戏路径…');
});
function onInvalidGames(payload) {
    cleanupGamesButton.disabled = false;
    if (payload.failed) return;
    const missing = Array.isArray(payload.missing) ? payload.missing : [];
    const matches = p => p.id !== 'default' && p.exePath && missing.some(item =>
        item.id === p.id && String(item.exePath).toLowerCase() === String(p.exePath).toLowerCase());
    const removed = draft.profiles.filter(matches);
    draft.profiles = draft.profiles.filter(p => !matches(p));
    saved.profiles = saved.profiles.filter(p => !matches(p));
    if (!draft.profiles.length) draft.profiles = structuredClone(MODEL_DEFAULTS.profiles);
    if (!saved.profiles.length) saved.profiles = structuredClone(MODEL_DEFAULTS.profiles);
    if (!draft.profiles.some(p => p.id === activeId))
        activeId = (draft.profiles.find(p => p.id === 'default') || draft.profiles[0]).id;
    if (removed.length) {
        for (const profile of removed) forgetIcon(profile.id);
        host.post('applySettings', saved, { noReload: 1 });
        pushWatchList();
        renderAll();
    }
    appendLog('已清除 ' + removed.length + ' 个失效游戏。' +
        (payload.unknown ? '另有 ' + payload.unknown + ' 个路径暂时无法确认，已保留。' : ''));
}

function onScannedGames(payload) {
	const games = Array.isArray(payload.games) ? payload.games : [];
	if (!games.length) {
		appendLog('没扫到已安装的游戏。可能是 Steam/Epic/GOG 都没装，' +
			'或者游戏装在这三个平台之外 —— 用「添加游戏」手动选 exe。');
		return;
	}
	// 按 exe 路径去重：已经有同一个 exe 的配置就跳过，别造重复
	const existingPaths = new Set(
		draft.profiles.map(p => String(p.exePath || '').toLowerCase())
			.filter(Boolean));
	let added = 0;
	const usedIds = new Set(draft.profiles.map(p => p.id));
	for (const game of games) {
		const exePath = String(game.exePath || '');
		if (!exePath) continue;
		const existing = draft.profiles.find(p => String(p.exePath || '').toLowerCase() === exePath.toLowerCase());
		if (existing) {
			mergeLibraryMetadata(existing, game);
			continue;
		}
		const exe = String(game.exe || '').toLowerCase();
		let id = exe.replace(/\.exe$/, '').replace(/[^a-z0-9._-]/g, '_') || 'game';
		// **id 撞车必须处理。** core 是按 id 找 profiles\<id>.json 的，
		// 两个游戏都叫 game.exe（很常见）会互相覆盖设置。
		if (usedIds.has(id)) {
			let n = 2;
			while (usedIds.has(id + '-' + n)) n++;
			id = id + '-' + n;
		}
		usedIds.add(id);
		existingPaths.add(exePath.toLowerCase());
		draft.profiles.push({
			id,
			name: String(game.name || id),
			exe,
			exePath,
			args: '',
			source: gameSource(game),
			steamAppId: String(game.steamAppId || ''),
			coverUrl: validCoverUrl(String(game.coverUrl || '')) ? game.coverUrl : '',
			pinned: false,
			lastLaunch: 0,
			settings: {}
		});
		added++;
	}
	const quiet = autoScanQuiet;
	autoScanQuiet = false;
	if (added) {
		appendLog('扫到 ' + games.length + ' 个已安装的游戏，新增了 ' + added +
			' 个配置。挑出来的 exe 不一定对（一个游戏目录里常有好几个 exe）—— ' +
			'启动失败的话点进配置页改一下路径。');
	} else if (!quiet) {
		// 启动时的自动扫描没有新增就别说话 —— 每次开工具都刷一行"都已经在列表里了"
		// 只是噪音，而日志是用来看异常的。手动点「重新扫描」时才回一句。
		appendLog('扫到 ' + games.length + ' 个，都已经在列表里了。');
	}
	const hint = document.getElementById('scanHint');
	if (hint) {
		hint.textContent = added
			? '刚新增 ' + added + ' 个'
			: '已收录 ' + games.length + ' 个';
	}
	if (added) {
		renderAll();
		if (quiet) {
			// **自动扫描的结果直接落盘，不要留成"有未应用的修改"。**
			// 用户什么都没改，凭空看到一个待保存状态很奇怪；而且不点应用的话
			// 每次开工具都要重扫重加一遍，那个提示会一直挂着。
			persistAll();
		} else {
			markDirty();
		}
	}
	// **扫描过一次就落标记，下次启动不再自动扫。**
	// 无论这次有没有新增都落——用户删掉的 .exe 被下次启动的自动扫描加回来，
	// 根因就是"每次启动都扫 + 标记只认配置里还有没有"。手动 ⟳ 按钮不受影响：
	// 它是玩家自己的动作，加回来怪不了别人。
	draft.scanned = true;
	renderProfileList();
	if (quiet) {
		persistAll();
	} else {
		markDirty();
	}
}

// 9. 全局监控：列表里的游戏一启动就自动注入
const watchBox = document.getElementById('watchGames');
if (watchBox) {
	watchBox.addEventListener('change', () => {
		draft.watchGames = watchBox.checked;
		host.post('setWatch', null, { on: watchBox.checked ? 1 : 0 });
		if (watchBox.checked) pushWatchList();
		markDirty();
	});
}

// 状态条的目标切换下拉框（见 renderTargetSelect 顶部的说明）。
// 宿主拿着多目标表，这里只报选中的 pid。
const targetSelectBox = document.getElementById('stTargetSelect');
if (targetSelectBox) {
	targetSelectBox.addEventListener('change', () => {
		const pid = Number(targetSelectBox.value);
		if (pid > 0) host.post('switchTarget', null, { pid });
	});
}

// 全局监控的黑名单：专业软件和壁纸软件不该被自动注入。
//
// 起因（用户实测）：开着的 Blender 被"看到列表里的游戏启动了"自动注入，
// 主页从此一直挂着注入状态 —— 再开游戏测试时根本没法从主页判断这次
// 注入没注入成功。这些进程一开就是几小时、还常驻，最先被监控接上、
// 之后一直占着状态显示的，恰恰就是它们。
//
// 按 exe 名（小写）精确匹配。名字来源：这些软件的 exe 名多年不变。
const WATCH_BLOCKLIST = new Set([
	// DCC / 专业软件
	'blender.exe', 'houdinifx.exe', 'houdini.exe', 'houdinicore.exe',
	'maya.exe', '3dsmax.exe', '3dsmaxio.exe', 'cinema4d.exe', 'c4d.exe',
	'substance_painter.exe', 'substance_designer.exe', 'designer.exe',
	'zbrush.exe', 'katana.exe', 'nuke.exe', 'nukestudio.exe',
	'davinciresolve.exe', 'resolve.exe', 'afterfx.exe', 'premiere.exe',
	'photoshop.exe', 'illustrator.exe', 'indesign.exe',
	'houdiniengine.exe', 'maried.exe', 'marmosettool.exe',
	'toolbag.exe', 'marmoset_toolbag.exe', 'marmosettoolbag3.exe',
	'unrealvert.exe', 'unrealed.exe', 'ue4editor.exe', 'ue5editor.exe',
	'unity.exe', 'unityhub.exe', 'unity_editor.exe',
	'godot.exe', 'godot4.exe',
	// 壁纸软件（常驻，最容易先被接上）
	'wallpaper32.exe', 'wallpaper64.exe', 'wallpaperengine32.exe',
	'wallpaperengine64.exe', 'engine32.exe', 'engine64.exe',
	'lively.exe', 'lively wallpaper.exe', 'wallpaperlive.exe',
	'rainmeter.exe', 'deskscapes.exe', 'pushvideowallpaper.exe',
	'video paper.exe', 'videopaper.exe', 'mpv.exe',
]);
function isWatchBlocked(exeName) {
	return WATCH_BLOCKLIST.has(String(exeName || '').toLowerCase());
}

// 把要监控的 exe 名推给宿主。宿主不理解 profile 结构，只认这一串名字。
function pushWatchList() {
	const names = draft.profiles
		.map(p => String(p.exe || '').toLowerCase())
		.filter(Boolean)
		.filter(exe => {
			if (isWatchBlocked(exe)) {
				// 被黑名单挡掉的只在这里说一次 —— 每次应用设置都会推一遍，
				// 不 filter 的话日志会被刷屏。
				blockedWatchOnce = blockedWatchOnce || new Set();
				if (!blockedWatchOnce.has(exe)) {
					blockedWatchOnce.add(exe);
					appendLog('全局监控：' + exe + ' 在黑名单里（专业/壁纸软件），不会自动注入');
				}
				return false;
			}
			return true;
		});
	// 分隔符同 requestIcons：**不能用控制字符**（JSON.stringify 会转义，宿主的
	// 朴素取值器拿到的是字面量）。这条以前用控制字符，后果是监控列表永远只有一个
	// 分不开的条目 —— **全局监控其实一直没匹配到任何游戏**，而且它不报错，
	// 只是什么都不发生。
	const lateNames = draft.profiles
        .filter(p => names.includes(String(p.exe || '').toLowerCase()) && resolvedInjectionTiming(p, 'watch') === 'late')
        .map(p => String(p.exe).toLowerCase());
    host.post('watchList', null, { names: names.join('*'), lateNames: lateNames.join('*') });
}
let blockedWatchOnce = null;

// 搜索框常驻（以前是一个放大镜图标点开，太小、注意不到）
const profileSearchBox = document.getElementById('profileSearch');
if (profileSearchBox) {
	profileSearchBox.addEventListener('input', () => {
		profileFilter = profileSearchBox.value;
		renderProfileList();
	});
}

const deleteProfileDialog = document.getElementById('deleteProfileDialog');
document.getElementById('deleteProfile').addEventListener('click', () => {
    const profile = draft.profiles.find(p => p.id === activeId);
    if (!profile || profile.id === 'default') return;
    confirmDeleteId = profile.id;
    document.getElementById('deleteProfileName').textContent = profile.name || profile.id;
    deleteProfileDialog.showModal();
});
deleteProfileDialog.addEventListener('close', () => { confirmDeleteId = null; });
deleteProfileDialog.addEventListener('cancel', () => { confirmDeleteId = null; });
document.getElementById('cancelDeleteProfile').addEventListener('click', () => {
    confirmDeleteId = null;
    deleteProfileDialog.close();
});
document.getElementById('confirmDeleteProfile').addEventListener('click', () => {
    const id = confirmDeleteId;
    confirmDeleteId = null;
    deleteProfileDialog.close();
    if (!id || id === 'default' || !draft.profiles.some(p => p.id === id)) return;
    draft.profiles = draft.profiles.filter(p => p.id !== id);
    forgetIcon(id);
    if (activeId === id) activeId = 'default';
    renderAll();
    markDirty();
});

document.getElementById('launchBtn').addEventListener('click', () =>
	launchProfile(activeProfile()));

document.getElementById('detachBtn')
	.addEventListener('click', () => host.post('detach', null));

/* ---------------- dirty 状态：没有应用按钮了，改动即应用即存盘 ---------------- */

const dirtyHint = document.getElementById('dirtyHint');

const actionsBar = document.getElementById('actionsBar');

// 防抖自动存盘。所有 markDirty 都走这里 —— 没有"点应用"这一步了
// （用户拍板：所有的修改都默认应用）。600ms：拖滑条时输入事件每帧都来，
// 每次都写 35 份配置 + 走一轮 IPC 太浪费；600ms 短到感知不到"没存上"。
let autoPersistTimer = 0;
function scheduleAutoPersist() {
	if (autoPersistTimer) return;
	autoPersistTimer = setTimeout(() => {
		autoPersistTimer = 0;
		if (JSON.stringify(saved) === JSON.stringify(draft)) return;
		persistAll();
	}, 600);
}

function markDirty() {
	// **说清楚"已经在游戏里生效"和"已经存盘"是两件事。**
	// DLSSNR 的参数改完 120ms 内就推给 core 了，这里只是存盘的防抖。
	dirtyHint.textContent = '已生效，正在自动保存…';
	// **操作条常驻**：DLSSNR 的实时耗时读数在这条上，藏起来就看不到了，
	// 而那是判断"这个处理分辨率划不划算"的唯一真实依据。
	actionsBar.hidden = currentPage === 'library';
	scheduleAutoPersist();
}

function markClean() {
	dirtyHint.textContent = '';
	actionsBar.hidden = currentPage === 'library';
}

// 两份配置指向同一个 exe = 它们写的是同一个文件，**后写的赢**，而界面上看不出来。
// core 只按 exe 名找文件（见 profileFileName），所以这不是能靠改名回避的问题 ——
// 只能告诉用户去删掉一个。静默地让"改了没用"发生过太多次了。
function warnDuplicateProfileFiles() {
	const byFile = new Map();
	for (const profile of draft.profiles) {
		const file = profileFileName(profile);
		if (!byFile.has(file)) byFile.set(file, []);
		byFile.get(file).push(profile.name || profile.id);
	}
	for (const [file, names] of byFile) {
		if (names.length < 2) continue;
		appendLog('⚠ 有 ' + names.length + ' 份配置指向同一个 exe（都写 profiles\\' +
			file + '）：' + names.join('、') + '。core 只会读到最后写进去的那一份 —— ' +
			'请删掉多余的，否则"改了设置没生效"会随机出现。');
	}
}

function persistAll() {
	saved = structuredClone(draft);
	host.post('applySettings', draft);
	// 每个配置都写一份展平文件：core 只认这些
	for (const profile of draft.profiles) writeProfileFile(profile);
	warnDuplicateProfileFiles();
	pushHotkeys();
	// 配置增删改之后监控列表就变了，必须重推 —— 否则新加的游戏监控不到，
	// 删掉的游戏还在被监控（那会注入到一个用户已经不想管的进程里）。
	pushWatchList();
	markClean();
}

/* ---------------- 快捷键捕获 ---------------- */

let capturing = null;

for (const btn of document.querySelectorAll('[data-hotkey]')) {
	btn.addEventListener('click', () => {
		if (capturing) capturing.classList.remove('is-capturing');
		capturing = btn;
		btn.classList.add('is-capturing');
		btn.textContent = '按单键或组合键…';
	});
}

window.addEventListener('keydown', e => {
	if (!capturing) return;
	e.preventDefault();
	if (e.key === 'Escape') {
		renderAll();
		capturing.classList.remove('is-capturing');
		capturing = null;
		return;
	}
	if (['Control', 'Alt', 'Shift', 'Meta'].includes(e.key)) return;

	const parts = [];
	if (e.ctrlKey) parts.push('Ctrl');
	if (e.altKey) parts.push('Alt');
	if (e.shiftKey) parts.push('Shift');
	if (e.metaKey) parts.push('Win');
    const keyName = { Delete: 'Del', Insert: 'Ins', ' ': 'Space' }[e.key] ||
        (e.key.length === 1 ? e.key.toUpperCase() : e.key);
    parts.push(keyName);
    if (!parseHotkeyForCore(parts.join(' + ')).vk) {
        appendLog('不支持此按键，请使用字母、数字、功能键或导航键。');
        return;
    }

    const action = capturing.dataset.hotkey;
    const combo = parts.join(' + ');
    const parsed = parseHotkeyForCore(combo);
    const conflict = Object.entries(draft.hotkeys).find(([otherAction, otherCombo]) => {
        if (otherAction === action) return false;
        const other = parseHotkeyForCore(otherCombo);
        return other.vk === parsed.vk && other.mods === parsed.mods;
    });
    if (conflict) {
        appendLog('该快捷键已用于另一个动作，请先修改原绑定或选择其他按键。');
        return;
    }
    draft.hotkeys[action] = combo;
    if (action === 'toggleInject') draft.injectHotkeyVersion = 1;
    // Injection/debug keys are launcher-only and must not override newer in-game bindings.
    if (action === 'toggleAll' || action === 'toggleOverlay') draft.hotkeyRevision = Date.now();
	capturing.classList.remove('is-capturing');
	capturing = null;
	renderAll();
	markDirty();
});

/* ---------------- 主题 ---------------- */

function applyTheme(theme) {
	const dark = theme !== 'light';
	document.documentElement.setAttribute('data-theme', dark ? 'dark' : 'light');
	document.getElementById('themeToggle').textContent = dark ? '浅色' : '深色';
}

document.getElementById('themeToggle').addEventListener('click', () => {
	draft.theme = draft.theme === 'light' ? 'dark' : 'light';
	applyTheme(draft.theme);
	markDirty();
});

/* ---------------- 语言切换 ---------------- */

// lang: 'auto'（首启，按系统语言解析后落成具体值）/ 'zh' / 'en'（日语已删）。
// 语言变化时同步上报宿主（setLang）：宿主自己发的提示（注入成功那几条）
// 按它选中/英文案，不然英文界面下这些提示还是中文，看起来就像"没提示"。
function reportLang(lang) {
	host.post('setLang', null, { lang: lang === 'en' ? 2 : 0 });
}
function initLanguage() {
	const sel = document.getElementById('langSel');
	if (!sel) return;
	let lang = draft.lang;
	if (lang === 'auto' || !lang) {
		lang = (window.I18N && I18N.systemLang()) || 'zh';
		draft.lang = lang;
        saved.lang = lang;
        host.post('applySettings', draft, { noReload: 1 }); // Persist the first resolved choice without reloading a game.
	}
	if (window.I18N) I18N.set(lang);
	sel.value = lang;
	reportLang(lang);       // 宿主启动时不知道语言，这里补报一次
}

document.getElementById('langSel').addEventListener('change', e => {
	const lang = e.target.value;
	draft.lang = lang;
	if (window.I18N) I18N.set(lang);
	reportLang(lang);
	markDirty();
});

/* ---------------- 左侧导航 ---------------- */

function showPage(name) {
    if (name === 'extras') host.post('getExtensions');
	currentPage = name;
	for (const item of document.querySelectorAll('.nav > .nav-item'))
		item.classList.toggle('is-active', item.dataset.page === name);
	for (const page of document.querySelectorAll('.page'))
		page.classList.toggle('is-active', page.dataset.page === name);
	document.getElementById('advice').hidden = name === 'library' || name === 'extras';
	document.getElementById('actionsBar').hidden = name === 'library' || name === 'extras';
}
for (const item of document.querySelectorAll('.nav > .nav-item'))
	item.addEventListener('click', () => showPage(item.dataset.page));
document.getElementById('backToLibrary').addEventListener('click', () => showPage('library'));
document.getElementById('libraryFilter').addEventListener('change', e => {
	libraryFilter = e.target.value;
	renderProfileList();
});
document.getElementById('defaultProfile').addEventListener('click', () => {
	if (!draft.profiles.some(p => p.id === 'default')) {
		draft.profiles.unshift(structuredClone(MODEL_DEFAULTS.profiles[0]));
		markDirty();
	}
	activeId = 'default';
	renderAll();
	showPage('profile');
});
document.getElementById('changeGameCover').addEventListener('click', () => {
	const profile = activeProfile();
	if (profile.id !== 'default') host.post('pickGameCover', null, {id: profile.id, exePath: profile.exePath || ''});
});

// Native host owns this global preference in launcher.json. A game/profile save
// cannot silently change elevation, and cancelling UAC does not close the UI.
const adminLaunch = document.getElementById('adminLaunch');
if (adminLaunch) {
	adminLaunch.addEventListener('change', () => {
		adminLaunch.disabled = true;
		host.post('setAdminLaunch', null, { on: adminLaunch.checked ? 1 : 0 });
	});
}
function onLauncherPreferences(preferences) {
	if (!adminLaunch) return;
	adminLaunch.checked = preferences.adminLaunch !== false;
	adminLaunch.disabled = false;
}

/* ---------------- 状态 ---------------- */

const STATE_TEXT = {
	unavailable: '不可用',
	disabled: '已关闭',
	standby: '待生效',
	active: '生效中',
	failed: '失败'
};
// STATE_TEXT 的翻译版：t() 查不到回退中文（i18n 字典里有这五个词）。
function stateText(key) {
	return window.I18N ? I18N.t(STATE_TEXT[key] || key) : STATE_TEXT[key];
}

// 状态提示条：把一堆标志位翻译成"你现在该做什么"。
// 真超分和原生深度都要求我们赶在游戏创建渲染资源之前在场，玩家没法从画面上看出
// 有没有赶上，所以必须明确告诉他。
// **三语**：所有句子用 t() 包（中文原文为键，见 i18n.js）—— 拼接处按句段拆开
// 翻译，中间夹的动态值（快捷键、API 名）原样保留。
function computeAdvice(s) {
	const t = window.I18N ? I18N.t : (x => x);
	const settings = resolvedSettings(activeProfile());
	if (!s.attached) {
		return {
			level: 'info',
			text: registeredInjectKey === null ? t('等待快捷键注册…')
                : !registeredInjectKey ? t('注入快捷键未注册成功，请在全局设置中重新设置，或使用「从工具启动游戏」。')
                : `${t('未启动游戏，请启动游戏，启动后按快捷键')} ${draft.hotkeys.toggleAll} ${t('开关效果、')} ` +
                    `${draft.hotkeys.toggleOverlay} ${t('调节效果，或在已启动游戏窗口使用快捷键')} ${registeredInjectKey} ${t('加载工具。')}`
		};
	}
    if (['D3D11', 'D3D9', 'OpenGL'].includes(s.api)) {
        return {
            level: 'info',
            text: `${t('这个游戏用的是')} ${s.api}${t('，NR 和游戏内面板通过桥接运行；无原生矢量时可使用光流。此路径不提供原生 DLSS 超分或帧生成集成。')}`
        };
    }
    if (s.api === 'Vulkan') {
        return {
            level: 'info',
            text: t('Vulkan 的 NR 取决于可用的呈现桥接；当前未提供原生 Vulkan Evaluate 集成。')
        };
    }
	// **这个游戏不看 swapchain 的尺寸 —— 真超分对它不可能正确。**
	//
	// 我们的真超分靠"给游戏一组更小的 backbuffer 并谎报尺寸"实现，而有些引擎
	// （RE Engine 实测如此，街霸 6 就是）按自己设置里的分辨率渲染，压根不读
	// swapchain 的 desc。它于是把全分辨率的画面往小 backbuffer 上写，只有左上角
	// 装得进去 —— 玩家看到的是"画面被放大、超出窗口"。
	// core 那边已经会自动撤掉代理恢复画面，这里负责把配置改回 DLAA，
	// 否则下一局还是同一个坑。
	if (s.proxySizeIgnored) {
		return {
			level: 'error',
			text: t('这个游戏按它自己设置里的分辨率渲染，不理会我们改的 backbuffer ' +
				'尺寸 —— 「DLSS 超分」的放大档位在它上面必然是裁切放大的画面。' +
				'已自动改回 DLAA（原生分辨率抗锯齿），重启游戏生效。')
		};
	}
	// 字段缺失时不要下结论 —— 宁可什么都不说，也不要给出错误的诊断
	if (s.injectedEarly === false) {
		const needs = [];
		if (settings.srEnable && settings.srMode === 'upscale' &&
			s.proxyActive === false) {
			needs.push(t('真超分'));
		}
		if (s.depthCandidates === 0) needs.push(t('原生深度'));
		if (needs.length) {
			return {
				level: 'warn',
				text: t('注入偏晚，') +
					needs.join(t(' 和 ')) +
					t('还没生效。在游戏里改一次分辨率通常能补上；要稳定生效请关掉游戏，' +
						'用「从工具启动游戏」重开。'),
				action: 'launch'
			};
		}
	}
	if (settings.srEnable && settings.srMode === 'upscale' &&
		s.proxyActive === false) {
		return {
			level: 'warn',
			text: t('真超分未生效（已降级为 DLAA）。在游戏里改一次分辨率，' +
				'或用「从工具启动游戏」。'),
			action: 'launch'
		};
	}
	return { level: 'ok', text: t('运行正常。') };
}

function renderAdvice(s) {
	const t = window.I18N ? window.I18N.t : (text => text);
	const advice = computeAdvice(s);
	const box = document.getElementById('advice');
	const icon = document.getElementById('adviceIcon');
	const actions = document.getElementById('adviceActions');
	box.hidden = currentPage === 'library';
	box.dataset.level = advice.level;
	icon.textContent =
		advice.level === 'ok' ? '✓' : advice.level === 'error' ? '!' : 'i';
	document.getElementById('adviceText').textContent = advice.text;

	actions.innerHTML = '';
	if (advice.action === 'launch' && activeProfile().exePath) {
		const btn = document.createElement('button');
		btn.className = 'btn btn-ghost btn-sm';
		btn.textContent = t('从工具启动游戏');
		btn.addEventListener('click', () =>
			document.getElementById('launchBtn').click());
		actions.appendChild(btn);
	}
}

// 浮层里改的参数只落在 core 内存里（ApplyOverlaySetting -> nrSettings）。
// core 把当前值随状态块报回来（v11 的 nrParam*），这里回填进输入框并存进配置 ——
// 否则"在游戏里调好的值，一回工具就变回去了"（用户实测踩过）。
//
// **只在值真的变了时写**：状态块每次都带这些字段，无脑写的话每次轮询都
// 触发一次落盘 + renderAll，浮层里拖一下滑条会写成几百次。
// 玩家正在操作本侧输入框（聚焦/拖动中）时跳过：他的手比轮询快，别抢。
const NR_PARAM_SYNC_KEYS = {
	nrParamIntensity: 'nrIntensity',
	nrParamLocalTone: 'nrLocalTone',
	nrParamLocalStructure: 'nrLocalStructure',
	nrParamSkinStructure: 'nrSkinStructure',
	nrParamPreset: 'nrPreset',
	nrParamStyle: 'nrStyle',
	nrParamAutoMask: 'nrAutoMask',
	nrParamUiCorrection: 'nrUiCorrection',
	// 处理分辨率。加浮层同步时漏了它 —— "降分辨率不能双向同步"的根（用户实测）。
	nrParamRenderScale: 'nrRenderScale',
	nrParamColourStrength: 'nrColourStrength',
	nrParamSelfLayers: 'nrSelfLayers',
	nrParamTrueLayers: 'nrTrueLayers',
	nrParamOpticalFlow: 'nrOpticalFlow',
	nrParamOpticalFlowQuality: 'nrOpticalFlowQuality',
	nrParamSemanticMask: 'nrSemanticMask',
	nrParamSemanticEnabled: 'nrSemOn',
	nrParamSemanticBg: 'nrSemBgInt',
	nrParamSemanticDebug: 'nrSemanticDebugView',
	nrParamSemanticFlipY: 'nrSemanticFlipY',
	nrParamSemanticFeather: 'nrSemanticFeather',
	...Object.fromEntries(Array.from({length: 18}, (_, i) => ['nrParamSemanticIntensity' + i, 'nrSemInt' + i]))
};
// 本侧对应的 data-key 集合（玩家正在操作这些控件时不同步）
const NR_PARAM_SYNC_KEYS_LOCAL = new Set(Object.values(NR_PARAM_SYNC_KEYS));
// 上一轮看到的参数版本号。**只在版本号变化（= 游戏内浮层真的改了参数）时
// 才采纳 core 报的值**；版本号没变时 core 报的只是"我自己推送的回声"
// （UI -> ReloadSettings -> core 内存，版本号不 bump）—— 采纳的话，
// 本地正在编辑的值会被"core 还没收到新值"的那一拍抢回旧值
// （用户实测：工具里改 skin 成任何值，游戏里都弹回 -1 —— 旧配置的旧默认）。
let lastNrParamVersion = -1;
let lastNrParamTarget = '';
function syncNrParamsFromCore(s) {
	if (!s) return;
	const version = Number(s.nrParamVersion);
	if (!Number.isFinite(version)) return;
	const target = String(s.currentPid || s.pid || s.target || '');
	if (version === lastNrParamVersion && target === lastNrParamTarget) return;
	lastNrParamVersion = version;
	lastNrParamTarget = target;
	const profile = matchProfileByTarget() || activeProfile();
	profile.settings = profile.settings || {};
	let changed = false;
	for (const [srcKey, dstKey] of Object.entries(NR_PARAM_SYNC_KEYS)) {
		if (s[srcKey] === undefined) continue;
		const coreVal = s[srcKey];
		const local = resolvedSettings(profile)[dstKey];
		const same = typeof coreVal === 'boolean'
			? Boolean(local) === coreVal
			: Math.abs(Number(local) - Number(coreVal)) < 1e-4;
		if (same) continue;
		profile.settings[dstKey] = coreVal;
		changed = true;
	}
	if (!changed) return;
	// **来源是 core：只落盘，不回推。** 见 writeProfileFile 顶部的说明 ——
	// 回推的话，玩家在游戏里 1 -> 1.2 -> 1.5 拖到一半就会被落盘那一下的
	// 1.2 打回去（用户实测：参数被"同步回了 1.5 前面的值"）。
	writeProfileFile(profile, /*noReload*/ true);
	// Persist only the fields sourced from this core into the launcher model.
	// Do not publish a ReloadSettings echo or commit unrelated pending edits.
	const persisted = saved.profiles.find(p => p.id === profile.id);
	if (persisted) {
		persisted.settings = persisted.settings || {};
		for (const key of Object.values(NR_PARAM_SYNC_KEYS)) {
			if (key in profile.settings) persisted.settings[key] = profile.settings[key];
		}
		host.post('applySettings', saved, { noReload: 1 });
	}
	renderAll();
}

/* ---------------- 注入目标切换（宿主的多目标表） ----------------
 *
 * 宿主每个注入过的进程都记一张表（status.targets + currentPid），
 * 这里渲染成下拉框。以前只有一个目标：监控先接上 blender 之类的
 * 常驻软件之后，后面启动的游戏被 `if (g_status.IsOpen()) return 0;`
 * 静默丢掉 —— 界面上永远只有第一个目标，测试游戏时根本没法判断
 * 注入没注入成功（用户实测）。
 */
function renderTargetSelect(s) {
	const row = document.getElementById('stTargetRow');
	const select = document.getElementById('stTargetSelect');
	if (!row || !select) return;
	const targets = Array.isArray(s && s.targets) ? s.targets : [];
	if (targets.length === 0) {
		row.hidden = true;
		select.innerHTML = '';
		return;
	}
	row.hidden = false;
	// currentPid 不在表里（刚退出还没清）就先选第一项
	const currentPid = Number(s.currentPid) || 0;
	const selected = targets.some(t => Number(t.pid) === currentPid)
		? currentPid : Number(targets[0].pid);
	// 重建选项。要避免打字/展开中重建打断用户：选项没变就只改选中项
	const optionsHtml = targets.map(t =>
		`<option value="${Number(t.pid)}">${t.name || ('pid ' + t.pid)}` +
		(t.launchedByTool ? '（工具启动）' : '') + `</option>`).join('');
	if (select.innerHTML !== optionsHtml) select.innerHTML = optionsHtml;
	if (Number(select.value) !== selected) select.value = String(selected);
}

function setStatus(s) {
	s = s || {};
	// target 变化必须留下足迹：它是配置同步找"正在运行的游戏"的唯一依据，
	// 空的话一切参数都写进当前选中的配置 —— 而这条路之前完全静默。
	if ((s && s.target) !== lastStatus.target) {
		host.post('uiError', null, {
			text: `[状态] target=${s && s.target ? s.target : '(空)'}`
		});
	}
	lastStatus = s;
	if (s.proxySizeIgnored) handleProxyIgnored();
	syncNrParamsFromCore(s);
	renderTargetSelect(s);
	// 三语助手：t() 查不到回退中文（键是中文原文，见 i18n.js 的字典）。
	const t = window.I18N ? I18N.t : (x => x);
	const put = (id, v) => {
		const el = document.getElementById(id);
		if (el) el.textContent = v == null ? '—' : t(String(v));
	};
	const state = v => (v ? stateText(v) : null);
	const yesNo = v => (v === undefined ? null : (v ? '是' : '否'));

	const attached = Boolean(s.attached);
	// 用词刻意保守（"加载"而不是"注入"）—— 同样的字也显示在游戏画面上。
	put('stHookState', !attached ? t('未加载')
		: s.hooked ? t('已接管 present') : t('已加载，等待 present'));

	/* ---- 底部常驻：DLSSNR 的真实 GPU 耗时 ---- */
	// core 报的是**指数滑动平均**，不是全程均值 —— 长会话里全程均值会把几分钟前
	// 的数字一直拖进来，那不是"现在多少"。改处理分辨率时 core 会清零重新开始。
	const perfMs = document.getElementById('perfNrMs');
	const perfSub = document.getElementById('perfNrSub');
	if (perfMs) {
		const ms = Number(s.nrGpuMs);
		if (!attached || !(ms > 0)) {
			perfMs.textContent = '—';
			perfMs.className = 'perf-value';
			if (perfSub) {
				perfSub.textContent = !attached ? t('未加载')
					: s.masterEnabled === false ? t(`总开关 OFF（${draft.hotkeys.toggleAll}）`)
					: s.nrState === 'active' ? t('正在测…') : t('未运行');
			}
		} else {
			perfMs.textContent = ms.toFixed(2) + ' ms';
			// 60fps 的预算是 16.7ms。占掉三成以上就该提醒了 ——
			// 这个读数存在的意义就是让人能当场判断"划不划算"。
			perfMs.className = 'perf-value' +
				(ms > 8 ? ' is-bad' : ms > 5 ? ' is-warn' : ' is-good');
			if (perfSub) {
				const worst = Number(s.nrGpuMsWorst);
				const share = (ms / 16.67 * 100).toFixed(0);
				perfSub.textContent = t('最差 ') + (worst > 0 ? worst.toFixed(1) : '—')
					+ ' ms · ' + t('占 60fps 预算 ') + share + '%';
			}
		}
	}

	/* ---- 13. 游戏自己开着 DLSS 时，硬禁掉我们的超分 ---- */
	// 判据用 core 报的 gameDlssSeen（旁听到游戏解析过 NVSDK_NGX_* 或真的 evaluate
	// 过）。**这比"等它崩一次再说"强**：两套 DLSS 在一个进程里会直接崩到桌面。
	const srBox = document.getElementById('srEnable');
	const conflictLive = document.getElementById('srConflictLive');
	const conflict = attached && s.gameDlssSeen === true;
	if (srBox) {
		if (conflict) {
			srBox.disabled = true;
			srBox.dataset.blockedByGameDlss = '1';
		} else if (srBox.dataset.blockedByGameDlss) {
			delete srBox.dataset.blockedByGameDlss;
			// 解禁交回 updateVisibility 决定（它还要看别的 data-requires）
			updateVisibility();
		}
	}
	if (conflictLive) {
		conflictLive.textContent = conflict
			? '　正在使用游戏原生 DLSS；DXL 的额外 SR 已停用，NR 可继续处理。'
			: '';
		conflictLive.className = conflict ? 'warn-live is-on' : 'warn-live';
	}
	put('stApi', attached ? s.api : null);
	// 显示"后果"而不是"事实"：injectedEarly=false 但代理已建立、深度也找到了，
	// 是完全正常的组合 —— 玩家在游戏里改一次分辨率走的是 ResizeBuffers，不会触发
	// CreateSwapChainForHwnd，所以这一位仍然是 false，但功能都补上了。
	// 只报原始事实会让人以为出了问题。
	// 只有"实际需要"的东西缺了才算受影响：SR 关着时没有代理是正常的，
	// 深度模式选了零深度时没有候选也是正常的。
	const st = resolvedSettings(activeProfile());
	const needsProxy = st.srEnable && st.srMode === 'upscale';
	// 旁听要的是"我们赶在游戏解析 NGX 函数之前把加载器补丁装上了"，判据是**补丁
	// 装到了几个模块上**。原来这里拿"深度候选数"当判据是错的：深度候选是我们自己
	// 的 SR 才需要的东西，而旁听即使注入偏晚也可能接上（鬼武者实测：Steam 启动壳
	// 转手、注入时 6 个 NVIDIA 模块已在，旁听照样接上并抄到一万多帧）。
	// 拿错判据 = 界面给出一个和实际情况无关的结论，那比不显示更糟。
	const needsEaves = Boolean(st.eavesdrop);
	const recovered =
		(!needsProxy || s.proxyActive !== false) &&
		(!needsEaves || s.eavesdropModules === undefined ||
			s.eavesdropModules > 0);
	put('stEarly', !attached ? null
		: s.injectedEarly === undefined ? '未知（宿主没报告）'
		: s.injectedEarly ? '早于 swapchain 创建（最佳）'
		: recovered ? '晚于 swapchain 创建（已通过重建补上）'
		: '晚于 swapchain 创建 —— 真超分/原生深度受影响');
	put('stSr', state(s.srState));
	put('stNr', state(s.nrState));
	put('stProxy', !attached ? null
		: s.proxyActive === undefined ? '未知（宿主没报告）'
		: s.proxyActive ? '已建立' : '未建立');
	put('stDepthCount', !attached ? null
		: s.depthCandidates === undefined ? '未知（宿主没报告）' : s.depthCandidates);

	/* ---- 诊断 ---- */
	// 卡顿：0 次是好消息，说清楚它是"有没有卡过"而不是"现在卡不卡"
	put('stStall', !attached ? null
		: s.stallCount === undefined ? '未知（宿主没报告）'
		: s.stallCount === 0 ? '没有卡过'
		: `卡过 ${s.stallCount} 次（转储在日志里）`);
	// 跳帧不是错误：它是"绝不阻塞游戏 present 线程"的代价，偶尔涨几帧正常
	put('stSkipped', !attached ? null
		: s.srSkippedFrames === undefined ? '未知（宿主没报告）'
		: `${s.srSkippedFrames} / ${s.nrSkippedFrames}`);

	/* ---- 旁听 ---- */
	const eavesOn = Boolean(resolvedSettings(activeProfile()).eavesdrop);
	put('stEavesChain', !attached ? null
		: !eavesOn ? '未启用'
		: s.eavesdropModules === undefined ? '未知（宿主没报告）'
		: s.eavesdropModules === 0 ? '没装上（注入太晚？）'
		: s.eavesdropLookups === 0
			? `已补 ${s.eavesdropModules} 个模块，等游戏解析 NGX`
			: `已接上（${s.eavesdropModules} 个模块 / ${s.eavesdropLookups} 次解析）`);
	// "看到"和"拷到"是两件事：拷贝要求先从 barrier 观察到资源状态
	put('stEavesFrames', !attached || !eavesOn ? null
		: s.eavesdropFrames === undefined ? '未知（宿主没报告）'
		: `${s.eavesdropFrames} / ${s.eavesdropCapturedFrames || 0}`);
	put('stEavesSize', !attached || !eavesOn || !s.eavesdropCapturedWidth ? null
		: `${s.eavesdropCapturedWidth}x${s.eavesdropCapturedHeight}`);
	put('stEavesMvScale', !attached || !eavesOn || !s.eavesdropFrames ? null
		: `${(s.eavesdropMvScaleX || 0).toFixed(1)}, ${(s.eavesdropMvScaleY || 0).toFixed(1)}`);
	put('stEavesJitter', !attached || !eavesOn || !s.eavesdropFrames ? null
		: `${(s.eavesdropJitterX || 0).toFixed(4)}, ${(s.eavesdropJitterY || 0).toFixed(4)}`);
	// DLSS5 到底跑在哪儿。这一行决定了下面那行的含义，所以放它上面。
	//
	// 一帧都没处理到时**必须说出原因**。上一版只写"已请求，但还没处理到任何一帧"——
	// 而真实原因是"游戏的颜色格式是 R11G11B10_FLOAT，被我们的白名单拒了"。
	// 那句话把一个有明确原因的失败说成了一个谜，用户没法据此做任何事。
	// 这些码对应 IpcProtocol.h 的 NrAtEvaluateBlock，加了一定要两边都加。
	const BLOCK_TEXT = {
		1: 'DLSS5 还没初始化好（再等几帧）',
		2: '还没从游戏的 barrier 里观察到颜色/矢量的状态',
		3: '游戏的颜色格式我们做不了（日志里有具体哪一项不行）',
		4: '游戏颜色的尺寸和我们准备的不一致',
		5: 'NGX 报错了（详情看日志）',
		6: '正在切换处理路径，等待 GPU 完成上一条路径',
		7: 'HDR 压缩着色器没就绪',
		8: '命令列表状态记账没装上（少了它会闪退，所以主动拒绝）',
		// 这一条的文案必须带**做法**，不能只报现象。它对应的现实是"工具装上了、
		// 日志里没有任何报错、画面毫无变化"—— 用户唯一能看到的线索就是这一行。
		9: '尚未捕获原生 SR Evaluate；可能是 SR 未启用、加载器入口未接入或注入较晚。请从工具启动后查看日志中的 NGX Evaluate attached。'
	};
	const atEval = Boolean(s.nrAtEvaluateWanted);
	const atEvalFrames = s.nrAtEvaluateFrames || 0;
	const blockText = BLOCK_TEXT[s.nrAtEvaluateBlocked] || '原因未知（看日志）';
	put('stNrWhere', !attached ? null
		: s.nrRoute === 3 ? 'NR 暂停：等待游戏 SR 恢复；FG 状态未知，自动回退尚未验证'
		: s.nrRoute === 2 ? `Evaluate（SR → NR，已处理 ${atEvalFrames} 帧）`
		: s.nrRoute === 1 ? 'Present（显示图像）'
		: s.nrRoute === 0 ? (atEval ? `等待可用路径：${blockText}` : '等待 NR 初始化')
		: s.nrAtEvaluateWanted === undefined ? '未知（宿主没报告）'
		: !atEval ? 'Present（显示图像）'
		: atEvalFrames > 0 ? `原生 DLSS 之后（Evaluate 已处理 ${atEvalFrames} 帧）`
		: `Evaluate 未运行：${blockText}`);
	put('stRealMotion', !attached ? null
		: s.nrMotionSource === 1 ? '原生运动矢量'
		: s.nrMotionSource === 2 ? '光流运动矢量' + (Number(s.nrOpticalFlowMs) > 0 ? `（${Number(s.nrOpticalFlowMs).toFixed(2)} ms）` : '')
		: s.nrMotionSource === 0 ? '零矢量'
		: s.nrUsingRealMotion === undefined ? '未知（宿主没报告）'
		: s.nrUsingRealMotion ? '是'
		: '否（使用光流或零矢量，见游戏内面板）');

	// 运行时 DLL。只显示 DLSSNR（用户拍板）：现在只有它在用 NGX 运行时，
	// stDlssDll/stFgDll 的控件已从主页删掉 —— 状态块里那两个字段还在发
	// （core 那侧 probe 三个都做），留着不发的话以后加回来还得两边查。
	put('stNrDll', s.nrDll === undefined ? null : (s.nrDll ? t('已就位') : t('缺失')));
	// DLL 版本（资源段的 FileVersion）。读不到就显示"—"：没有资源段的 DLL 是存在的。
	put('stNrDllVersion', s.nrDllVersion ? String(s.nrDllVersion) : '—');

	/* ---- #32：DLSSNR 参数只读显示（配置页） ---- */
	// 值全部来自状态块的 nrParam*（core 随每一拍报回来）—— 浮层里改了它会跟着变。
	// 没在跑时保持上次的值不动（nrParamVersion 没变、core 也没报新值），
	// 比显示一排"—"有用：玩家刚关游戏回来看的还是他调过的那套。
	const STYLES = ['0 Default', '1 Natural', '2 Cinematic'];
	const roStyle = s.nrParamStyle === undefined ? null :
		STYLES[Math.max(0, Math.min(2, Math.round(Number(s.nrParamStyle))))] || '—';
	put('nrRoStyle', roStyle);
	put('nrRoIntensity', s.nrParamIntensity === undefined ? null
		: Number(s.nrParamIntensity).toFixed(2));
	put('nrRoTone', s.nrParamLocalTone === undefined ? null
		: Number(s.nrParamLocalTone).toFixed(2));
	put('nrRoStructure', s.nrParamLocalStructure === undefined ? null
		: Number(s.nrParamLocalStructure).toFixed(2));
	put('nrRoSkin', s.nrParamSkinStructure === undefined ? null
		: Number(s.nrParamSkinStructure).toFixed(2));
	put('nrRoAutoMask', s.nrParamAutoMask === undefined ? null
		: s.nrParamAutoMask ? t('开') : t('关'));
	put('nrRoUiCorrection', s.nrParamUiCorrection === undefined ? null
		: s.nrParamUiCorrection ? t('开') : t('关'));
	put('nrRoRenderScale', s.nrParamRenderScale === undefined ? null
		: Math.round(Number(s.nrParamRenderScale) * 100) + '%');
	put('nrRoSelfLayers', s.nrParamSelfLayers === undefined ? null : Number(s.nrParamSelfLayers).toFixed(2));
	put('nrRoTrueLayers', s.nrParamTrueLayers === undefined ? null : Math.round(Number(s.nrParamTrueLayers)));
	put('nrRoColourStrength', s.nrParamColourStrength === undefined ? null : Number(s.nrParamColourStrength).toFixed(2));
	put('nrRoOpticalFlow', s.nrParamOpticalFlow === undefined ? null : s.nrParamOpticalFlow ? t('开') : t('关'));
	put('nrRoOpticalFlowQuality', s.nrParamOpticalFlowQuality === undefined ? null : [t('低'), t('中'), t('高')][Math.max(0, Math.min(2, Math.round(Number(s.nrParamOpticalFlowQuality))))]);
	put('nrRoSemantic', s.nrParamSemanticMask === undefined ? null : s.nrParamSemanticMask ? t('开') : t('关'));
	put('nrRoSemanticBg', s.nrParamSemanticBg === undefined ? null : Number(s.nrParamSemanticBg).toFixed(2));
	put('stRes', s.renderWidth && s.outputWidth
		? `${s.renderWidth}x${s.renderHeight} → ${s.outputWidth}x${s.outputHeight}`
		: null);
	put('stFrameMs', s.frameMs ? s.frameMs.toFixed(2) + ' ms' : null);
	put('stEvaluate', s.evaluateCount === undefined ? null
		: `SR ${s.evaluateCount} / NR ${s.nrEvaluateCount || 0}` +
		  (s.evaluateFailures || s.nrEvaluateFailures
			? `（失败 ${s.evaluateFailures || 0}/${s.nrEvaluateFailures || 0}）` : ''));

	document.getElementById('coreDot').classList.toggle('is-on', attached);
	// 加载了但总开关是关的 —— 这一条必须显出来，不然玩家会以为工具没生效。
	document.getElementById('coreState').textContent = !attached ? '未加载'
		: s.masterEnabled === false
			? t(`${s.target || t('已加载')} · 效果 OFF（${draft.hotkeys.toggleAll} 打开）`)
			: (s.target || '已加载');
	document.getElementById('detachBtn').disabled = !attached;
	renderAdvice(s);
}

const logEntries = [];
function renderLog() {
    const tr = window.I18N ? (I18N.log || I18N.t) : x => x;
    const view = document.getElementById('logView');
    view.textContent = logEntries.map(e => `[${e.time}] ${tr(e.line)}`).join('\n');
    view.scrollTop = view.scrollHeight;
}
function appendLog(line) {
    const time = new Date().toLocaleTimeString('en-GB', { hour12: false });
    logEntries.push({time, line: String(line)});
    if (logEntries.length > 1000) logEntries.shift();
    renderLog();
}

/* ---------------- 启动 ---------------- */

/* ---------------- 日志页的两个按钮 ---------------- */

document.getElementById('openLogsBtn').addEventListener('click', () => {
	// core 的日志在游戏进程里写的，UI 自己看不到 —— 直接把文件夹打开给用户，
	// 比在界面上印一行路径让他自己复制有用得多。
	host.post('openLogs', null);
	appendLog('已请求打开日志文件夹');
});

document.getElementById('clearLogBtn').addEventListener('click', () => {
	logEntries.length = 0;
    document.getElementById('logView').textContent = window.I18N ? I18N.t('（已清空）') : '（已清空）';
});

// 打开 ngx\（运行时 DLL）文件夹。和日志那个同一条路：宿主用 explorer 开，
// 比在界面上印一条路径让用户自己复制有用。
document.getElementById('openNgxDirBtn').addEventListener('click', () => {
	host.post('openNgxDir');
	appendLog('已请求打开 DLL 文件夹');
});

/* ---------------- 启动 ---------------- */

// 版本号写在一处，别在 HTML 里硬编码（之前 HTML 里那个 v0.4.0 早就过期了）
const APP_VERSION = 'v0.3';
window.addEventListener('dxl-language-changed', () => { renderLog(); renderHotkeyHints(); renderAdvice(lastStatus); });
document.getElementById('brandVersion').textContent = APP_VERSION;
document.getElementById('projectLink').addEventListener('click', event => {
    event.preventDefault();
    host.post('openProjectPage');
});
for (const [id, action] of [['reFrameworkDownload', 'openReFrameworkPage'], ['reShadeDownload', 'openReShadePage']]) {
    document.getElementById(id).addEventListener('click', event => {
        event.preventDefault();
        host.post(action);
    });
}

renderAll();
markClean();
function renderExtensions(info) {
    document.getElementById('semanticInstallStatus').textContent = info.installed
        ? '语义模型已安装并随完整包集成，可在游戏内启用实验性语义 Mask。' : '语义组件缺失，请重新解压完整包。';
    document.getElementById('semanticDownloadAddress').textContent = info.downloadUrl || '完整包已包含模型，无需额外下载';
    document.getElementById('downloadSemantic').disabled = !info.downloadUrl;
    document.getElementById('semanticFolder').textContent = info.folder || '';
    if (window.I18N) window.I18N.apply();
}
document.getElementById('downloadSemantic').addEventListener('click', () => host.post('downloadSemantic'));
document.getElementById('openSemanticFolder').addEventListener('click', () => host.post('openSemanticFolder'));
document.getElementById('refreshExtensions').addEventListener('click', () => host.post('getExtensions'));
host.post('uiReady', null);
