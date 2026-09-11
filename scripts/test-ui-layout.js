'use strict';

// 这个样式表已经栽过四次同一形状的坑：**不报错、界面还"能用"，只是少了一层布局。**
//
//   1. `.page-scroll { overflow-y: auto }` —— 容器实际叫 `.pages`
//   2. `.app { display: grid; ... }`       —— 容器实际叫 `.shell`
//   3. `.brand-title` / `.brand-sub`       —— HTML 里是 `.brand-name` / `.brand-version`
//   4. `.content` 是 grid 子项却没写 `min-height: 0`
//
// 前三次是**类名对不上**，第四次是**类名全对、但缺一条必需声明**。
// 症状每次都一样：滚轮滚不动、参数一多必须把窗口拉高、"应用"按钮点不到。
// 所以这里查两类东西。
//
// 顺便记一条：这个测试第一版**用 `used.includes(name)` 做子串匹配**，于是 `.app`
// 被 `<script src="app.js">` 里的 "app" 匹配上了 —— 测试通过，而 bug #2 照样在。
// 短类名（app / page / row / nav / dot）全都有这个问题。现在按**整词**匹配。

const fs = require('fs');
const path = require('path');

const web = path.join(__dirname, '..', 'src', 'ui', 'web');
const css = fs.readFileSync(path.join(web, 'style.css'), 'utf8');
const html = fs.readFileSync(path.join(web, 'index.html'), 'utf8');
const js = fs.readFileSync(path.join(web, 'app.js'), 'utf8');

const cssNoComments = css.replace(/\/\*[\s\S]*?\*\//g, '');

/* ---------- CSS：类名 -> 合并后的声明 ---------- */

// @media 的花括号会打乱下面这个"按 } 切块"的粗解析，先把 @xxx{ 的头去掉。
// 里面的规则会被当成无条件规则 —— 对本测试（只看有没有声明过）是安全的方向。
const flat = cssNoComments.replace(/@[^{]+\{/g, '');

const declsOf = new Map();   // class -> Map(prop -> value)
const declaredClasses = new Set();

for (const chunk of flat.split('}')) {
	const brace = chunk.indexOf('{');
	if (brace < 0) continue;
	const selector = chunk.slice(0, brace);
	const body = chunk.slice(brace + 1);

	const props = new Map();
	for (const decl of body.split(';')) {
		const colon = decl.indexOf(':');
		if (colon < 0) continue;
		props.set(decl.slice(0, colon).trim().toLowerCase(),
			decl.slice(colon + 1).trim().toLowerCase());
	}

	for (const part of selector.split(',')) {
		const trimmed = part.trim();
		for (const m of trimmed.matchAll(/\.([A-Za-z][A-Za-z0-9_-]*)/g)) {
			declaredClasses.add(m[1]);
		}
		// 合并声明只认**单一类选择器**（`.x` 或 `.x:hover`）——
		// `.a .b` / `.a.b` 里的声明不一定落在 .b 单独出现的地方，不能当成它有。
		const solo = /^\.([A-Za-z][A-Za-z0-9_-]*)(:[A-Za-z-]+(\([^)]*\))?)?$/.exec(trimmed);
		if (!solo) continue;
		const name = solo[1];
		if (!declsOf.has(name)) declsOf.set(name, new Map());
		const target = declsOf.get(name);
		for (const [k, v] of props) target.set(k, v);
	}
}

const decl = (classes, prop) => {
	for (const c of classes) {
		const m = declsOf.get(c);
		if (m && m.has(prop)) return m.get(prop);
	}
	return undefined;
};

/* ---------- 第一类：布局规则挂在不存在的类名上 ---------- */

const usedTokens = new Set();
for (const m of html.matchAll(/class\s*=\s*"([^"]*)"/g)) {
	for (const t of m[1].split(/\s+/)) if (t) usedTokens.add(t);
}
// JS 里的类名可能出现在字符串、模板串、querySelector('.x') 里。
// 按"非类名字符"切开取整词 —— 比子串匹配严，比只看 class= 宽。
for (const m of js.matchAll(/(['"`])((?:\\.|(?!\1)[^\\])*)\1/g)) {
	for (const t of m[2].split(/[^A-Za-z0-9_-]+/)) if (t) usedTokens.add(t);
}


// Concrete assignments stay authoritative even when quote punctuation inside a
// comment confuses the broad string scan above (e.g. "users' custom bindings").
for (const assignment of js.matchAll(/\.className\s*=[^\r\n]+/g)) {
    for (const literal of assignment[0].matchAll(/(['"`])([^'"`\r\n]*)\1/g))
        for (const token of literal[2].split(/[^A-Za-z0-9_-]+/)) if (token) usedTokens.add(token);
}

const LAYOUT_HINTS = [
	'display', 'overflow', 'position', 'grid-template',
	'flex-direction', 'grid-column', 'flex'
];

const missingLayout = [];
const missingOther = [];
for (const name of [...declaredClasses].sort()) {
	if (usedTokens.has(name)) continue;
	const props = declsOf.get(name);
	const isLayout = props && [...props.keys()].some(
		p => LAYOUT_HINTS.some(h => p.startsWith(h)));
	(isLayout ? missingLayout : missingOther).push(name);
}

/* ---------- 第二类：滚动容器的祖先链缺 min-height: 0 ---------- */

// 简易 HTML 嵌套解析：只要 class 列表和父子关系。
const VOID = new Set(['area', 'base', 'br', 'col', 'embed', 'hr', 'img', 'input',
	'link', 'meta', 'param', 'source', 'track', 'wbr']);

const root = { tag: 'root', classes: [], parent: null };
let cursor = root;
const TAG_RE = /<(\/?)([a-zA-Z][a-zA-Z0-9-]*)((?:"[^"]*"|'[^']*'|[^>"'])*?)(\/?)>/g;
for (const m of html.matchAll(TAG_RE)) {
	const [, closing, tag, attrs, selfClose] = m;
	const name = tag.toLowerCase();
	if (closing) {
		// 找到最近的同名祖先再收，容错漏写的闭合标签
		let node = cursor;
		while (node && node.tag !== name) node = node.parent;
		if (node && node.parent) cursor = node.parent;
		continue;
	}
	if (VOID.has(name) || selfClose) continue;
	const cm = /class\s*=\s*"([^"]*)"/.exec(attrs);
	const node = {
		tag: name,
		classes: cm ? cm[1].split(/\s+/).filter(Boolean) : [],
		parent: cursor
	};
	cursor = node;
}

// 重新走一遍，收集所有节点（上面的 cursor 走法只留了链，没留全表）
const nodes = [];
{
	let cur = root;
	const TAG2 = new RegExp(TAG_RE.source, 'g');
	for (const m of html.matchAll(TAG2)) {
		const [, closing, tag, attrs, selfClose] = m;
		const name = tag.toLowerCase();
		if (closing) {
			let node = cur;
			while (node && node.tag !== name) node = node.parent;
			if (node && node.parent) cur = node.parent;
			continue;
		}
		if (VOID.has(name) || selfClose) continue;
		const cm = /class\s*=\s*"([^"]*)"/.exec(attrs);
		const node = {
			tag: name,
			classes: cm ? cm[1].split(/\s+/).filter(Boolean) : [],
			parent: cur
		};
		nodes.push(node);
		cur = node;
	}
}

const isScroller = n => {
	const oy = decl(n.classes, 'overflow-y') || decl(n.classes, 'overflow');
	return oy === 'auto' || oy === 'scroll';
};
const isFlexOrGrid = n => {
	const d = decl(n.classes, 'display');
	return d === 'flex' || d === 'grid' ||
		d === 'inline-flex' || d === 'inline-grid';
};
const hasMinHeightZero = n => {
	const v = decl(n.classes, 'min-height');
	return v === '0' || v === '0px' || v === '0%';
};

const chainProblems = [];
for (const n of nodes) {
	if (!n.classes.length || !isScroller(n)) continue;
	for (let a = n.parent; a && a.tag !== 'root'; a = a.parent) {
		if (!a.classes.length || !isFlexOrGrid(a)) continue;
		if (hasMinHeightZero(a)) continue;
		chainProblems.push({
			scroller: '.' + n.classes.join('.'),
			ancestor: '.' + a.classes.join('.'),
			display: decl(a.classes, 'display')
		});
	}
}

/* ---------- 第三类：渲染 → 发消息 → 收消息 → 渲染 的死循环 ---------- */

// 又一个同形状的坑，但这次不在 CSS 里：
//
//   renderProfileList() 末尾会 requestMissingIcons()，
//   宿主收到 requestIcons **一定**回一条 icons，抠不出图标的 id 不在回包里，
//   而 icons 的处理函数当时无条件 renderProfileList() ——
//   于是只要有一个 exe 抠不出图标，三者就以 IPC 的速度无限转。
//
// 症状同样是"不报错、界面还能用"：列表 DOM 每轮都被重建，mousedown 和 mouseup
// 落在不同元素上，浏览器根本不派发 click ——**点配置行毫无反应**，一直停在启动时
// 选中的那个。实测空转吃掉宿主 65% 的一个核、WebView 一整个核。
//
// 所以这里钉死这个环上的两个断点。

function bodyOf(source, header) {
	const at = source.indexOf(header);
	if (at < 0) return null;
	const open = source.indexOf('{', at);
	if (open < 0) return null;
	let depth = 0;
	for (let i = open; i < source.length; i++) {
		if (source[i] === '{') depth++;
		else if (source[i] === '}' && --depth === 0) return source.slice(open, i + 1);
	}
	return null;
}

// **注释必须先剥掉。** 第一版没剥，于是 case 'icons' 里那句
// "无条件重画会和 renderProfileList() 末尾的…组成死循环" 的**注释**
// 把检查自己触发了 —— 测试红着，代码却是对的。
// 引号内的 // 不算注释（路径、正则里都有）；这里只做到这一层，够用。
function stripComments(source) {
	let out = '';
	let quote = '';
	for (let i = 0; i < source.length; i++) {
		const c = source[i];
		if (quote) {
			out += c;
			if (c === '\\') { out += source[++i] || ''; continue; }
			if (c === quote) quote = '';
			continue;
		}
		if (c === '"' || c === "'" || c === '`') { quote = c; out += c; continue; }
		if (c === '/' && source[i + 1] === '/') {
			while (i < source.length && source[i] !== '\n') i++;
			out += '\n';
			continue;
		}
		if (c === '/' && source[i + 1] === '*') {
			i += 2;
			while (i < source.length && !(source[i] === '*' && source[i + 1] === '/')) i++;
			i++;
			continue;
		}
		out += c;
	}
	return out;
}

const loopProblems = [];
const jsCode = stripComments(js);
const renderList = bodyOf(jsCode, 'function renderProfileList(');
const askIcons = bodyOf(jsCode, 'function requestMissingIcons(');

if (!renderList || !askIcons) {
	loopProblems.push('找不到 renderProfileList / requestMissingIcons，'
		+ '这个检查已经失效，要么改名了要么删了 —— 请更新本测试');
} else if (renderList.includes('requestMissingIcons(')) {
	// 环的一半还在，那另一半的两个断点就都得在
	if (!askIcons.includes('iconAsked.has') || !askIcons.includes('iconAsked.add')) {
		loopProblems.push('requestMissingIcons 没有用 iconAsked 记住"问过了" —— '
			+ '抠不出图标的 id 会被反复问，环就闭合了');
	}
	const iconsCase = /case\s*'icons'\s*:([\s\S]*?)break;/.exec(jsCode);
	if (!iconsCase) {
		loopProblems.push("找不到 case 'icons' —— 请更新本测试");
	} else if (/\brenderProfileList\s*\(/.test(iconsCase[1])) {
		loopProblems.push("case 'icons' 里直接 renderProfileList() —— "
			+ '收到回包就重画，等于把环接上；要走 onIcons()，只在真的多了图标时重画');
	}
}

/* ---------- 报告 ---------- */

if (missingOther.length) {
	console.log('提示：这些类名只有装饰性规则，HTML/JS 里也没用到（死 CSS，不影响布局）：');
	console.log('  ' + missingOther.join(' '));
}

let failed = false;

if (missingLayout.length) {
	failed = true;
	console.error('');
	console.error('**布局规则挂在不存在的类名上** —— 这一层布局永远不会生效：');
	for (const name of missingLayout) console.error('  .' + name);
}

if (chainProblems.length) {
	failed = true;
	console.error('');
	console.error('**滚动容器的祖先缺 min-height: 0** —— 滚动条不会出现：');
	for (const p of chainProblems) {
		console.error(`  ${p.scroller} 的祖先 ${p.ancestor}`
			+ ` (display: ${p.display}) 没有 min-height: 0`);
	}
	console.error('');
	console.error('grid / flex 子项的 min-height 默认是 auto —— "不许缩到比内容还矮"。');
	console.error('少了它，祖先会被内容顶高，滚动容器永远不溢出，等于不存在。');
}

if (loopProblems.length) {
	failed = true;
	console.error('');
	console.error('**渲染 ↔ IPC 死循环的防线破了** —— 症状是点配置行没反应：');
	for (const p of loopProblems) console.error('  ' + p);
}

if (failed) {
	console.error('');
	console.error('这几类都是同一个形状：不报错、界面还能用，只是少了一层布局 /'
		+ ' 多了一圈空转。');
	process.exit(1);
}

const scrollers = nodes.filter(n => n.classes.length && isScroller(n));
console.log('通过：' + declaredClasses.size + ' 个类名都在 HTML/JS 里用到了（整词匹配）；'
	+ scrollers.length + ' 个滚动容器的祖先链都有 min-height: 0；'
	+ '图标请求没有形成重画环。');
