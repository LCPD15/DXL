// 状态显示的回归测试：喂一份状态 JSON 给 app.js，检查主页那些格子里**真的**出现了
// 数字，而不是"—"。
//
// 为什么要有这个测试：同一类 bug 已经咬过两次。
//   第一次：core 把新字段发布进共享内存了，但宿主拼 JSON 时忘了带上 —— UI 把它们
//           当 undefined，界面上永远显示错的结论（"晚于 swapchain 创建"、"深度候选 0"）。
//   第二次：宿主把十七组字段塞进 char[512]，实际需要 833 字节，_snprintf_s 静默截断
//           成非法 JSON，PostWebMessageAsJson 拒收并丢弃 —— 整块"运行状态"和"诊断"
//           全程空白，而两边一个字的报错都没有。
// 两次都是"后端好的、界面在骗人"，只能靠人肉盯界面才发现。这个测试把它变成一条命令。
//
// 跑法：node scripts\test-status-render.js
// 它不需要 WebView2，也不需要游戏 —— 只用一个最小的 DOM 假件把 app.js 跑起来。

const fs = require('fs');
const path = require('path');
const vm = require('vm');

/* ---------------- 最小 DOM 假件 ---------------- */

function makeElement(id) {
	const element = {
		id,
		textContent: '',
		value: '',
		innerHTML: '',
		checked: false,
		hidden: false,
		disabled: false,
		type: '',
		// select 的选项集合。**假件缺这一项曾让测试报出一条假故障**：
		// app.js 里 `for (const o of select.options)` 在真浏览器里没问题
		// （HTMLOptionsCollection 是可迭代的），在这个假件上却是 undefined。
		// 假件不全会制造"代码坏了"的假象，而那正是这个测试要防的东西。
		options: [],
		min: 0, max: 0, step: 0,
		scrollTop: 0, scrollHeight: 0,
		dataset: {},
		classList: {
			_set: new Set(),
			add(c) { this._set.add(c); },
			remove(c) { this._set.delete(c); },
			contains(c) { return this._set.has(c); },
			toggle(c, on) { if (on === undefined) { this._set.has(c) ? this._set.delete(c) : this._set.add(c); } else if (on) { this._set.add(c); } else { this._set.delete(c); } }
		},
		appendChild() { },
		append() { },
		addEventListener() { },
		setAttribute() { },
		querySelectorAll() { return []; }
	};
	return element;
}

const elements = new Map();
function byId(id) {
	if (!elements.has(id)) elements.set(id, makeElement(id));
	return elements.get(id);
}

const document = {
	documentElement: { setAttribute() { } },
	getElementById: byId,
	createElement: () => makeElement('created'),
	querySelectorAll: () => [],
	querySelector: () => null,
	addEventListener() { }
};

const logLines = [];
const sandbox = {
	document,
	window: { addEventListener() { }, chrome: undefined },
	console: { log: (...a) => logLines.push(a.join(' ')) },
	structuredClone,
	JSON, Object, Array, Number, String, Boolean, Math, Date, RegExp, Set, Map
};
sandbox.globalThis = sandbox;

const appPath = path.join(__dirname, '..', 'src', 'ui', 'web', 'app.js');
vm.runInNewContext(fs.readFileSync(appPath, 'utf8'), sandbox, { filename: appPath });

/* ---------------- 喂数据 ---------------- */

// 先给一份配置：旁听那几行是"只有开了旁听才显示"的，不设置的话它们本来就该是"—"
sandbox.onHostMessage(JSON.stringify({
	type: 'settings',
	payload: {
		theme: 'dark',
		hotkeys: { toggleInject: 'Alt + I', toggleAll: 'Alt + D' },
		profiles: [{
			id: 'onimushawots_demo.exe', name: '鬼武者', exe: 'OnimushaWotS_Demo.exe',
			exePath: 'X:\\game.exe', args: '',
			settings: {
				dlss5Enable: true, eavesdrop: true, srEnable: false,
				dlss5AtEvaluate: true
			}
		}]
	}
}));

// 数值取自一次真实运行（core-77272.log）
sandbox.onHostMessage(JSON.stringify({
	type: 'status',
	payload: {
		dlssDll: true, nrDll: true, fgDll: true,
		attached: true, pid: 77272, api: 'D3D12', hooked: true,
		srState: 'unavailable', nrState: 'active', fgState: 'unavailable',
		nativeDepth: true, nativeMotion: false,
		injectedEarly: false, proxyActive: false, depthCandidates: 30,
		renderWidth: 1920, renderHeight: 1080,
		outputWidth: 1920, outputHeight: 1080,
		presentCount: 11400, evaluateCount: 0, evaluateFailures: 0,
		nrEvaluateCount: 11400, nrEvaluateFailures: 0,
		frameMs: 16.67,
		stallCount: 0, lastStallStage: 0,
		srSkippedFrames: 0, nrSkippedFrames: 0,
		eavesdropModules: 15, eavesdropLookups: 40,
		eavesdropFrames: 11400, eavesdropCapturedFrames: 11399,
		eavesdropCapturedWidth: 1286, eavesdropCapturedHeight: 724,
		eavesdropMvScaleX: 1286.0, eavesdropMvScaleY: 724.0,
		eavesdropJitterX: 0.1875, eavesdropJitterY: -0.0185,
		nrUsingRealMotion: true,
		nrAtEvaluateFrames: 13120, nrAtEvaluateWanted: true,
		// 运行时 DLL 卡：只有 DLSSNR 的控件在主页（dlss/fg 的已删），
		// 版本号来自宿主 GetFileVersionInfo 读的资源段。
		dlssDll: true, fgDll: true,
		nrDllVersion: '5.10.0.01829R',
		// #32：配置页的只读参数格。值来自状态块的 nrParam*（core 每拍都报）。
		nrParamVersion: 7,
		nrParamStyle: 2, nrParamIntensity: 0.9,
		nrParamLocalTone: 1.2, nrParamLocalStructure: 1.0,
		nrParamSkinStructure: 0.6, nrParamAutoMask: 1, nrParamUiCorrection: 1,
		nrParamRenderScale: 0.9,
		target: 'OnimushaWotS_Demo.exe', message: '已接管 present'
	}
}));

/* ---------------- 检查 ---------------- */

// 主页上每一格。**都不允许是"—"** —— 那正是这次 bug 的症状。
const REQUIRED = [
	'stHookState', 'stApi', 'stEarly', 'stSr', 'stNr', 'stProxy', 'stRes',
	'stFrameMs', 'stEvaluate',
	'stStall', 'stSkipped', 'stDepthCount',
	'stEavesChain', 'stEavesFrames', 'stEavesSize', 'stEavesMvScale',
	'stEavesJitter', 'stNrWhere', 'stRealMotion',
	'stNrDll', 'stNrDllVersion',
	// #32：只读参数格（配置页）。同一条规则：不允许是"—"。
	'nrRoStyle', 'nrRoIntensity', 'nrRoTone', 'nrRoStructure', 'nrRoSkin',
	'nrRoAutoMask', 'nrRoUiCorrection', 'nrRoRenderScale'
];

let failed = 0;
const width = Math.max(...REQUIRED.map(id => id.length));
for (const id of REQUIRED) {
	const text = byId(id).textContent;
	const bad = !text || text === '—';
	if (bad) ++failed;
	console.log(`${bad ? '✗' : '·'} ${id.padEnd(width)}  ${text || '(空)'}`);
}

// 左下角那句和 UI 上的"核心"格子
const coreState = byId('coreState').textContent;
if (coreState !== 'OnimushaWotS_Demo.exe') {
	console.log(`✗ coreState 应该是目标进程名，实际是 "${coreState}"`);
	++failed;
}

// 第二个场景：evaluate 点那条路**没跑起来**。
//
// 这里必须断言界面**说出了原因**，而不只是"还没处理到任何一帧"。真实的坑就是这个：
// 原因是"游戏颜色格式 R11G11B10_FLOAT 被白名单拒了"，而界面把它显示成一个谜，
// 只能去翻 core 的日志才知道。
sandbox.onHostMessage(JSON.stringify({
	type: 'status',
	payload: {
		attached: true, api: 'D3D12', hooked: true,
		srState: 'disabled', nrState: 'active', fgState: 'unavailable',
		eavesdropModules: 15, eavesdropLookups: 128,
		eavesdropFrames: 11662, eavesdropCapturedFrames: 11662,
		eavesdropCapturedWidth: 1286, eavesdropCapturedHeight: 724,
		nrAtEvaluateWanted: true, nrAtEvaluateFrames: 0,
		nrAtEvaluateBlocked: 3,   // NrAtEvaluateBlock::ColorFormat
		nrUsingRealMotion: false,
		target: 'OnimushaWotS_Demo.exe', message: ''
	}
}));
const whereBlocked = byId('stNrWhere').textContent;
console.log('');
console.log(`· 没跑起来时显示：${whereBlocked}`);
if (!whereBlocked.includes('颜色格式')) {
	console.log('✗ 没跑起来时必须说出原因（应提到"颜色格式"），' +
		'否则界面把一个有明确原因的失败说成了一个谜');
	++failed;
}

console.log('');
if (failed) {
	console.log(`失败：${failed} 个格子没有内容。`);
	process.exit(1);
}
console.log(`通过：${REQUIRED.length} 个格子都有内容。`);
