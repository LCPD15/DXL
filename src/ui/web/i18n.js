/* Chinese/English UI strings, including dynamic state, instructions and accessibility labels. */
(function () {
	'use strict';

	const EN = {
        "工具内调参": "Edit in launcher",
        "仅当游戏内浮层无法呼出时使用": "Use only when the in-game overlay cannot be opened",
        "修改实时保存；游戏运行时立即生效，处理分辨率与真叠层在松手后应用。": "Changes save automatically and apply live. Render scale and true layers apply when you release the slider.",
        "NR 参数未保存，请使用更新后的核心重启游戏后重试。": "NR parameters were not saved. Restart the game with the updated core and try again.",
        "语义蒙板（实验性功能）": "Semantic Mask (Experimental)",
        "试验功能，效果可能不好。需要完整包中的语义组件。": "Experimental; results may be poor. Requires the semantic components from the complete package.",
        "自动遮罩（Auto Mask）": "Auto Mask",
        "背景强度": "Background strength",
        "SR 输入上下翻转": "Flip SR semantic input",
        "边缘羽化": "Mask feather",
        "游戏内蒙板预览": "In-game mask preview",
        "人物": "People",
        "人物强度": "People strength",
        "车辆": "Vehicles",
        "车辆强度": "Vehicles strength",
        "动物": "Animals",
        "动物强度": "Animals strength",
        "街道设施": "Street objects",
        "街道设施强度": "Street objects strength",
        "运动用品": "Sports",
        "运动用品强度": "Sports strength",
        "食物": "Food",
        "食物强度": "Food strength",
        "餐具": "Tableware",
        "餐具强度": "Tableware strength",
        "家具": "Furniture",
        "家具强度": "Furniture strength",
        "电子设备": "Electronics",
        "电子设备强度": "Electronics strength",
        "家电": "Appliances",
        "家电强度": "Appliances strength",
        "配饰": "Accessories",
        "配饰强度": "Accessories strength",
        "其他物品": "Other objects",
        "其他物品强度": "Other objects strength",

        '，NR 和游戏内面板通过桥接运行；无原生矢量时可使用光流。此路径不提供原生 DLSS 超分或帧生成集成。': '. NR and the in-game panel run through a bridge; optical flow is available without native motion vectors. This path does not provide native DLSS upscaling or frame-generation integration.',
        'Vulkan 的 NR 取决于可用的呈现桥接；当前未提供原生 Vulkan Evaluate 集成。': 'Vulkan NR depends on an available presentation bridge. Native Vulkan Evaluate integration is not currently provided.',

        '兼容模式': 'Compatibility mode',
        '若用自动模式启动游戏崩溃，请尝试兼容模式。': 'If the game crashes when launched in Automatic mode, try Compatibility mode.',
        '从 Steam 等外部启动时，自动监控等待窗口稳定后加载；需要优先捕获原生 DLSS 时，请从工具启动游戏。': 'For games started outside DXL, such as from Steam, Automatic monitoring waits for a stable window. Launch from DXL to prioritize capturing native DLSS.',

        '自动（按游戏兼容性选择）': 'Automatic (game compatibility)',
        '从工具启动时，优先在图形接口初始化前加载；自动模式对 GTA SA 重制版采用兼容加载。从外部启动时，自动模式等待窗口稳定。手动选择尽早会覆盖兼容策略，但可能与游戏初始化冲突。': 'Launching from DXL prioritizes loading before graphics initialization; Automatic uses compatibility loading for GTA SA Definitive Edition. For external launches, Automatic waits for a stable window. Selecting Early overrides this policy but may conflict with game initialization.',

        '确认删除配置': 'Confirm profile deletion',
        '将从游戏库和自动监控列表中移除此配置，不会删除游戏文件。': 'This removes the profile from the library and automatic monitoring. Game files will not be deleted.',
        '若已开启 NVIDIA AI 补帧，同时关闭该游戏的驱动补帧设置。': 'If NVIDIA AI frame generation is enabled, its driver setting for this game will also be disabled.',
        '此操作针对 NVIDIA Smooth Motion，不影响游戏原生 DLSS 帧生成。': 'This applies to NVIDIA Smooth Motion and does not change native DLSS frame generation in the game.',
        '配置已变更，已取消删除。请重新确认。': 'The profile changed, so deletion was cancelled. Confirm again to continue.',
        '配置保存失败，已取消删除。请重试。': 'The profile could not be saved, so deletion was cancelled. Try again.',
        '正在处理另一个配置，请稍后重试。': 'Another profile is being processed. Try again shortly.',
        '未能关闭该游戏的 NVIDIA AI 补帧，配置已保留。请查看日志。': 'NVIDIA AI frame generation could not be disabled for this game. The profile was kept. See the log for details.',
        'NVIDIA 驱动拒绝访问，配置已保留。请以管理员身份启动工具后重试。': 'The NVIDIA driver denied access. The profile was kept. Run DXL as administrator and try again.',
        '游戏 exe 路径无效，配置已保留。请检查路径后重试。': 'The game EXE path is invalid. The profile was kept. Check the path and try again.',
        '错误码：': 'Error code: ',
        '取消': 'Cancel', '确认删除': 'Delete profile',
        "额外功能": "Extras",
        "在游戏内按需启用，设置按游戏保存。": "Enable effects in game as needed. Settings are saved per game.",
        "后处理": "Post-processing",
        "LUT、滤镜与自定义效果在最终画面上处理，也会影响游戏 UI。": "LUTs, filters and custom effects process the final image, including game UI.",
        "支持 ReShade 和 DXL 格式的 .fx。将效果及其 .fxh、纹理一起放入后处理文件夹，在游戏内“调色”页启用和调整，按游戏保存。": "Supports ReShade and DXL .fx files. Copy effects together with their .fxh includes and textures, then enable and adjust them in the in-game Color Grading page. Settings save per game.",
        "打开后处理文件夹": "Open post-processing folder",
        "文件夹内附有使用和制作说明。自定义 FX 默认关闭；需要深度、插件或特殊输入的效果暂不支持。": "The folder includes usage and authoring instructions. Custom FX start disabled. Effects requiring depth, add-ons, or special input are not supported yet.",
        "无法创建后处理文件夹。": "Unable to create the post-processing folder.",
        "完整包已集成以下实验性功能，可在游戏内按需启用。": "The complete package includes these experimental features. Enable them in game as needed.",
        "语义蒙板（试验）": "Semantic mask (Experimental)",
        "通过语义识别给NR输入蒙板控制不同物体的NR强度。": "Uses semantic recognition to provide NR with a mask controlling the strength applied to different objects.",
        "单模型 YOLO：识别人物、车辆等物体，不包含第二个场景分类模型。": "Single YOLO model: recognizes people, vehicles and other objects, without a second scene classification model.",
        "语义组件缺失，请重新解压完整包。": "Semantic components are missing; extract the complete package again.",
        "语义模型已安装并随完整包集成，可在游戏内启用实验性蒙版。": "The complete package includes the semantic model. Enable the experimental mask in game.",
        "完整包已包含模型，无需额外下载": "The complete package includes the model; no additional download is needed",
        "获取组件": "Get components",
        "打开文件夹": "Open folder",
        "刷新状态": "Refresh status",
        "组件缺失时请重新解压完整包，并在替换文件后重启游戏。": "If components are missing, extract the complete package again and restart the game.",

		'游戏库': 'Library', '游戏详情': 'Game details', 'exe 路径和注入时机在下方设置。也可以直接按': 'Set the executable path and injection timing below. You can also press', '当前配置': 'Current profile',
		'默认配置': 'Default profile', '全部游戏': 'All games', '我的收藏': 'Favorites',
		'手动添加': 'Manually added', '款游戏': 'games', '查看游戏详情': 'View game details',
		'更换封面': 'Change cover', '← 游戏库': '← Library', '收藏': 'Favorite', '取消收藏': 'Unfavorite',
		'运行中': 'Running', '未设置 exe': 'No executable', '启动游戏并注入': 'Launch game with DXL',
		'还没有收藏，点击游戏卡片上的星标即可添加。': 'No favorites yet. Select the star on a game card to add one.',
		'没有匹配的游戏': 'No matching games',
		'添加游戏或重新扫描，开始建立你的游戏库。': 'Add a game or scan your installed games to start your library.',
		'尚未捕获原生 SR Evaluate；可能是 SR 未启用、加载器入口未接入或注入较晚。请从工具启动后查看日志中的 NGX Evaluate attached。': 'No native SR Evaluate calls captured. SR may be disabled, a loader entry point may not be hooked, or injection may be late. Launch from DXL and check the log for NGX Evaluate attached.',

		'主页': 'Home', '配置': 'Profile', 'DLSS5 参数': 'DLSS5 Params', '日志': 'Log',
		'配置文件': 'Profiles',
		'管理员启动': 'Run as admin',
		'默认管理员启动': 'Run as administrator by default',
		'自动保存，下次启动生效。取消权限请求后仍可普通启动。': 'Saved automatically for next launch. Cancelling elevation continues without admin rights.',
		'自动切换 Evaluate / Present': 'Switch Evaluate / Present automatically',
		'启用原生 DLSS Evaluate 路径': 'Enable native DLSS Evaluate route',
		'自叠层（Self Layers）': 'Self Layers',
		'真叠层（True NR Layers）': 'True NR Layers',
		'色彩强度': 'Colour Strength',
		'自动光流': 'Automatic optical flow',
		'光流质量': 'Optical flow quality',
		'自定义语义 Mask': 'Custom semantic mask',
		'Mask 背景强度': 'Mask background strength',
		'低': 'Low', '中': 'Medium', '高': 'High',
		'只读显示 —— 请在游戏内 ImGui 面板调参（默认 End 打开）。': 'Read-only — tune in the in-game ImGui panel (End by default).',
		'自动 —— 尽早加载以捕获原生 DLSS': 'Auto — load early to capture native DLSS',
		'尽早（优先捕获原生 DLSS 和矢量）': 'Early (prefer native DLSS and motion)',
		'延迟到游戏出画面后（可能无法捕获原生 DLSS）': 'After first frame (may miss native DLSS)',
		'编码输入（Evaluate）': 'Encoded input (Evaluate)',
		'差值（Evaluate）': 'Difference (Evaluate)',
		'部分游戏需要管理员身份启动才能运行。':
			'Some games only run when launched as administrator.',
		'＋ 添加游戏（选 exe）': '+ Add game (pick exe)',
		'全局监控': 'Global watch',
		'列表里的游戏一启动就自动注入': 'Auto-inject games in the list on launch',
		'⟳ 重新扫描已安装的游戏': '⟳ Rescan installed games',
		'点击按钮后按单键或组合键。效果与面板键为一组全局设置，修改后应用到所有游戏。': 'Click a button and press a key or shortcut. Editing the effect/panel pair applies it to all games.',
		'清除已失效游戏': 'Remove unavailable games',
		'按单键或组合键…': 'Press a key or shortcut…',
		'启动时会自动扫一次': 'Scans once automatically at startup',
		"手动注入": "Manual injection",
        "未启动游戏，请启动游戏，启动后按快捷键": "No game started. Start a game, then use",
        "开关效果、": "to toggle effects and",
        "调节效果，或在已启动游戏窗口使用快捷键": "to adjust them, or switch to an already-running game and press",
        "加载工具。": "to load DXL.",
        "FG 检测阈值（缓冲区数）": "FG detection threshold (buffers)",
        "高级：FG 检测阈值": "Advanced: FG detection threshold",
        "高级：FG 检测阈值（燕云特调默认 8）": "Advanced: FG detection threshold (Where Winds Meet tuned default: 8)",
        "尽量不要修改；如果工具能加载，但 NR 无效果，再尝试提高。": "Leave this unchanged if possible. If DXL loads but NR has no effect, try increasing it.",
        "默认 4，仅本游戏配置，重启游戏生效。": "Default: 4. Applies only to this game profile and takes effect after restarting the game.",
        "燕云十六声特调默认 8，仅本游戏配置，重启游戏生效。": "Where Winds Meet tuned default: 8. Applies only to this game profile and takes effect after restarting the game.",
        "RE引擎游戏需要先安装RE框架和ReShade才能正常使用。": "RE Engine games require REFramework and ReShade to be installed first for normal operation.",
        "绝区零DX12启动参数：": "Zenless Zone Zero DX12 launch argument: ",
        "RE框架下载": "Download REFramework",
        "ReShade下载": "Download ReShade",
        '未加载': 'Not loaded',
		'未关联任何游戏': 'No game linked',

		'从工具启动游戏': 'Launch from tool', '断开': 'Detach',
		'exe 路径和注入时机在左侧「配置」页设置。也可以直接按':
			'Set the exe path and injection timing on the Profile page. You can also press',
		'注入到已经在跑的游戏。': 'to inject into an already-running game.',
		'运行状态': 'Runtime status', '诊断': 'Diagnostics',
		'旁听游戏的 DLSS': 'Eavesdrop on game DLSS',
		'核心': 'Core', '渲染 API': 'Rendering API', '注入时机': 'Injection timing',
		'注入目标': 'Inject target', '工具启动': '(launched by tool)',
		'真超分代理': 'Upscale proxy', '渲染 → 输出': 'Render → Output',
		'帧时间': 'Frame time',
		'卡顿': 'Stalls', '跳帧（SR / NR）': 'Skipped (SR / NR)',
		'原生深度候选': 'Depth buffer candidates',
		'链条': 'Chain', '看到 / 拷到': 'Seen / Copied',
		'矢量分辨率': 'Vector resolution', 'MV 缩放': 'MV scale',
		'DLSS5 处理点': 'DLSS5 processing point', 'DLSS5 用上真矢量': 'DLSS5 uses real vectors',

		'全局设置': 'Global settings',
		'整个工具的设置，不属于某一个游戏配置。':
			'Tool-wide settings, not tied to any game profile.',
		'快捷键': 'Hotkeys',
		'注入 / 断开当前窗口的游戏': 'Inject / detach game in current window',
		'总开关（所有效果）': 'Master switch (all effects)',
		'Debug 视图循环（关 → 深度 → 运动矢量）':
			'Debug view cycle (off → depth → motion)',
		'游戏内浮层（开 / 关）': 'In-game overlay (open / close)',
		'外观': 'Appearance', '主题': 'Theme', '浅色': 'Light',
		'运行时 DLL': 'Runtime DLLs',
		// 主页运行时 DLL 卡（用户拍板：只显示 DLSSNR，别的功能加了再加）
		'core 从工具目录的': 'The core loads the NGX runtime from',
		'里加载 NGX 运行时。现在只有 DLSS5 神经滤镜用到它；以后加了别的功能再列出对应的 DLL。':
			' in the tool directory. Only the DLSS5 neural filter uses it right now; other DLLs will be listed when more features need them.',
		'nvngx_dlssnr.dll（DLSS5 神经滤镜）': 'nvngx_dlssnr.dll (DLSS5 neural filter)',
		'版本': 'Version',
		'打开 DLL 文件夹': 'Open DLL folder',

		'启动与注入': 'Launch & injection',
		'配置名称': 'Profile name', '游戏 exe 路径': 'Game exe path',
		'启动参数': 'Launch args',
		'仅当前配置，不会继承到其他游戏；下次从工具启动时使用。': 'Saved for this profile only, not inherited by other games; used on the next launch from DXL.',
		'例如：鬼武者': 'e.g. Onimusha', '可留空': 'Optional',
		'启用': 'Enable',
		// 配置页总开关（masterEnabled）。core 只在启动时读这个键，
		// 运行中靠 SetEnabled 命令推送（main.cpp 的 setMaster）。
		'总开关（本配置启动时自动照此开启效果）':
			'Master switch (effects auto-start per this setting)',
		'这就是游戏里 Del 那个总开关：管的是「这套工具的全部效果」（超分 + DLSS5 滤镜），不动注入本身。开关会存进配置 —— 上一局开过，这一局启动就自动是开的，不用每次进游戏再按一遍。游戏正在跑时在这里切，会立刻推送给它。':
			'This is the Del master switch in game: it controls ALL effects of this tool (upscaling + DLSS5 filter), not the injection itself. The choice is saved into the profile — if you turned it on last session, the next launch starts with it on, no need to press it again. If the game is running, toggling here pushes to it immediately.',
		'旁听游戏的 DLSS 取真矢量': 'Eavesdrop game DLSS for real vectors',
		'用真运动矢量': 'Use real motion vectors', '用真深度': 'Use real depth',
		'Debug 视图': 'Debug view', 'Debug 强度': 'Debug gain',
		'模式': 'Mode', '质量档（渲染分辨率）': 'Quality (render resolution)',
		'自定义渲染倍率': 'Custom render multiplier', '锐化': 'Sharpening',
		'危险操作': 'Danger zone', '删除此配置': 'Delete this profile',
		'当前配置：': 'Current profile: ',

		'性能：降分辨率跑 DLSSNR': 'Performance: run DLSSNR at lower res',
		'处理分辨率': 'Processing resolution',

		'打开日志文件夹': 'Open log folder', '清空': 'Clear',
		'（等待事件）': '(waiting for events)',

		// ---- 动态状态提示（computeAdvice / setStatus / renderProfileList）----
		// 键是中文原文；生成处用 I18N.t() 包（app.js），切语言即时生效。
		'未加载。切到游戏窗口按': 'Not loaded. Switch to the game window and press',
		'即可加载；想用真超分或原生深度，请用「从工具启动游戏」。':
			'to load. For true upscaling or native depth, use "Launch from tool".',
		'，目前只支持 D3D12 —— 所有功能都不会生效。':
			' — only D3D12 is supported, nothing will take effect.',
		'这个游戏用的是': 'This game uses',
		'，DLSS5 神经滤镜走桥接可用；真超分 / 旁听游戏矢量 / 游戏内浮层仅 D3D12。':
			'. The DLSS5 neural filter works via the bridge; ' +
			'true upscaling / eavesdropped vectors / in-game overlay are D3D12 only.',
		'这个游戏按它自己设置里的分辨率渲染，不理会我们改的 backbuffer 尺寸 —— 「DLSS 超分」的放大档位在它上面必然是裁切放大的画面。已自动改回 DLAA（原生分辨率抗锯齿），重启游戏生效。':
			'This game renders at its own resolution and ignores our backbuffer size — upscaling would only give a cropped picture. Automatically reverted to DLAA (takes effect after restart).',
		'注入偏晚，': 'Injected a bit late, ',
		'还没生效。在游戏里改一次分辨率通常能补上；要稳定生效请关掉游戏，用「从工具启动游戏」重开。':
			'is not in effect yet. Changing the resolution once in game usually fixes it; for it to stick, quit and relaunch with "Launch from tool".',
		'真超分': 'true upscaling', '原生深度': 'native depth', ' 和 ': ' and ',
		'真超分未生效（已降级为 DLAA）。在游戏里改一次分辨率，或用「从工具启动游戏」。':
			'True upscaling not in effect (downgraded to DLAA). Change the resolution once in game, or use "Launch from tool".',
		'运行正常。': 'All good.',
		'不可用': 'Unavailable', '已关闭': 'Off', '待生效': 'Pending',
		'生效中': 'Active', '失败': 'Failed',
		'已接管 present': 'present hooked', '已加载，等待 present': 'loaded, waiting for present',
		'总开关 OFF（Del）': 'master OFF (Del)',
		'正在测…': 'measuring…', '未运行': 'not running',
		'最差 ': 'worst ', '占 60fps 预算 ': 'of the 60fps budget: ',
		'已就位': 'present', '缺失': 'missing',

		'模型（Render Preset）': 'Model (Render Preset)', '风格（Style）': 'Style',
		'总强度（Intensity）': 'Overall intensity', '色调强度（Tone）': 'Tone strength',
		'结构强度（Structure）': 'Structure strength',
		'皮肤结构强度（Skin）': 'Skin structure',
		'语义自动遮罩（Auto Mask）': 'Semantic auto mask',
		'UI 修正（UI Correction）': 'UI correction',

		// ---- #32：DLSSNR 参数只读显示（配置页）----
		// 没在跑时保持上次的值（比一排"—"有用）；开/关这两个是动态生成的。
		'开': 'on', '关': 'off',
		'DLSSNR 参数（只读）': 'DLSSNR parameters (read-only)',
		'局部色调': 'Local tone', '局部结构': 'Local structure',
		'皮肤结构': 'Skin structure', '语义遮罩': 'Semantic mask',
		'UI 修正': 'UI correction', '处理分辨率': 'Processing resolution',
		'强度（Intensity）': 'Intensity',
		'这一栏只做显示，改不了 —— 请在游戏内叠加界面里调参（默认 End 打开）。游戏没在跑时显示的是上次运行结束前的值。':
			'This panel is display-only — adjust parameters in the in-game overlay (End by default). When no game is running, it shows the values from the last session.',

		// ---- #36：配置列表的 DX 版本徽标（title 提示，动态生成）----
		'从游戏 exe 里识别到的图形 API（启发式：按文件里有没有 d3d11/d3d12 的引用判定）':
			'Graphics API detected from the game exe (heuristic: based on d3d11/d3d12 references in the file)',
		// ---- #39：主页选中游戏信息区的位数徽标 ----
		'64 位': '64-bit', '32 位': '32-bit',
		'游戏 exe 的位数（PE 头 Machine 字段）':
			'Bitness of the game exe (PE header Machine field)',

		'语言 / Language': 'Language', '界面语言': 'Interface language',
		'中文': '中文', '日本語': '日本語', 'English': 'English',
		'立即生效。首次启动时按系统语言自动选择。':
			'Applies immediately. First launch picks the system language automatically.',
	};


    Object.assign(EN, {
    "注入快捷键未注册成功，请在全局设置中重新设置，或使用「从工具启动游戏」。": "The injection shortcut could not be registered. Change it in Global settings or use Launch from tool.",
    "等待快捷键注册…": "Waiting for shortcut registration…",
    "快捷键不可用": "Shortcut unavailable",
    "总开关 OFF（{key}）": "Effects OFF ({key})",
    "{target} · 效果 OFF（{key} 打开）": "{target} · Effects OFF ({key} to enable)",
    "只读显示 —— 请在游戏内 ImGui 面板调参（{key} 打开）。": "Read-only — adjust settings in the in-game ImGui panel ({key} to open).",
    "游戏内总开关快捷键：{key}。控制本工具的全部效果，保留注入。开关会保存到本配置；游戏运行时修改会立即生效。": "In-game effects shortcut: {key}. Controls all DXL effects while keeping injection active. The choice is saved for this profile and applied immediately to a running game.",

    "里加载 NGX 运行时。 现在只有 DLSS5 神经滤镜用到它；以后加了别的功能再列出对应的 DLL。": "loads the NGX runtime for neural rendering. Additional runtime files will be listed here if future features need them.",
    "默认": "Default",
    "深色": "Dark",
    "中文": "Chinese",
    "工具只能识别Steam等游戏平台已安装游戏，其他游戏请先添加.exe文件。": "DXL detects installed games from Steam and other supported platforms. For other games, add the .exe file first.",
    "已添加游戏在开启工具后从工具或steam直接启动都行，建议从工具启动": "Once a game is added, keep DXL open and launch it from DXL or Steam. Launching from DXL is recommended.",
    "带有反作弊的网游有风险，请斟酌使用。": "Using DXL in online games with anti-cheat carries a risk. Please use discretion.",
    "效果和面板快捷键只在游戏窗口中响应，不占用系统快捷键。": "Effect and panel hotkeys respond only in the game window and do not reserve system hotkeys.",
    "支持单键，修改后立即生效。面板打开时接管游戏键鼠； 再按面板键、ESC 或右上角 ✕ 关闭，Alt+Tab 和 Win 键始终放行。": "Single keys are supported and changes take effect immediately. The open panel captures game input; close it with the panel key, Esc or the top-right X. Alt+Tab and the Windows key remain available.",
    "界面语言。立即生效；首次启动时按系统语言自动选择。": "Language changes apply immediately. The first launch uses your Windows display language.",
    "从游戏自己的 DLSS 调用里读到真深度 / 真矢量 / jitter。": "Reads native depth, motion vectors and jitter from the game's DLSS calls.",
    "默认已开": "Enabled by default",
    "—— 它是 DLSSNR 画质的上限所在。": "— native inputs help preserve temporal stability.",
    "启动参数 / Launch arguments": "Launch arguments",
    "注入时机怎么选": "Choosing injection timing",
    "自动 / 尽早": "Auto / Early",
    "延迟": "Late",
    "优先在游戏初始化图形接口前加载，便于捕获原生 DLSS Evaluate、深度和运动矢量。部署到游戏目录的启动代理也会尽早加载。": "Loads before graphics initialization when possible to capture native DLSS Evaluate, depth and motion vectors. The startup proxy deployed to the game directory also loads early.",
    "在游戏出画面后加载，作为特殊游戏的手动兼容选项。部分游戏会缓存接口地址，延迟加载可能只能回退到 Present。": "Loads after the first frame as a manual compatibility option. Games that cache interface addresses may then support only the Present fallback.",
    "开启原生 DLSS 时优先使用 Evaluate；主菜单或关闭 DLSS 后按可用资源自动回退。游戏原生 DLSS 无需关闭。": "Prefers Evaluate with native DLSS enabled and switches to a fallback as resources allow in menus or with DLSS disabled. Native DLSS can remain enabled.",
    "神经渲染滤镜。开启原生 DLSS 时，在超分 Evaluate 之后处理 NR，供后续帧生成使用；没有原生 DLSS 时自动回退 Present。优先使用原生运动矢量，缺少时可自动计算光流。": "Neural rendering filter. With native DLSS, NR runs after upscaling Evaluate and before subsequent frame generation. Without native DLSS it falls back to Present. Native motion vectors take priority; optical flow can supply motion when needed.",
    "这就是游戏里 Del 那个总开关：管的是「这套工具的全部效果」 （超分 + DLSS5 滤镜），不动注入本身。开关会存进配置 —— 上一局开过，这一局启动就自动是开的，不用每次进游戏再按一遍。 游戏正在跑时在这里切，会立刻推送给它。": "This is the in-game Del master switch. It controls the tool's effects, including upscaling and the neural filter, while keeping injection active. The choice is saved for the next launch and sent immediately to a running game.",
    "关（正常画面）": "Off (normal image)",
    "深度（近亮远暗，看轮廓层次）": "Depth (near bright, far dark)",
    "运动矢量（中灰不动 / 红=右 / 绿=下）": "Motion (gray = still, red = right, green = down)",
    "怎么用这四项判断真深度/真矢量到底有没有用": "Checking native depth and motion inputs",
    "会把喂给 DLSSNR 的那张深度/矢量**直接画在画面上**， 而且这时候": "displays the depth or motion texture supplied to DLSSNR directly. In this view,",
    "不跑 DLSSNR": "NR is bypassed",
    "—— 所以看到的一定是原始输入本身， 不会掺进滤镜的效果。": "so you see the input without the filter's effect.",
    "编码与差值视图用于检查 Evaluate 路径的输入和处理结果。": "Encoded and Difference views inspect the Evaluate input and processing result.",
    "压缩后的输入": "Encoded input",
    "让你看清我们到底喂了什么进去；": "shows the data supplied to NR;",
    "差值": "Difference",
    "把「滤镜改了多少」直接画出来 ——": "shows the filter's changes —",
    "全黑就是没改": "black means no change",
    "，这比盯着最终画面猜\"好像变了一点\"可靠得多。": ", making changes easier to identify than comparing the final image by eye.",
    "矢量": "Motion vectors",
    "：中灰 = 不动，偏红 = 向右，偏绿 = 向下。 镜头左右平移时整屏应该往同一个方向偏色；偏反了说明矢量约定反了。": ": gray means still, red means right and green means down. Camera panning should produce a consistent direction; an opposite direction suggests an inverted motion convention.",
    "深度": "Depth",
    "：近处亮远处暗（或反之），能看出物体轮廓和层次就说明深度是对的。 看不出层次就调「Debug 强度」（深度是幂曲线指数，矢量是偏移增益，0 = 用默认）。": ": near and far surfaces should show different levels. Adjust Debug gain if needed: it controls the depth curve or motion gain. Zero uses the default.",
    "确认输入没问题之后，关掉 Debug 视图，再用": "After checking the inputs, turn Debug view off and use",
    "两个开关做 A/B —— 深度开关可单独退回零深度；关闭原生运动矢量后按光流开关选择光流或零矢量。改动自动保存，关闭光流可单独检查原生矢量的效果。": "for comparison. Depth can fall back to zero independently. Disabling native motion selects optical flow or zero motion according to the optical-flow switch. Changes save automatically.",
    "运动矢量用于保持跨帧细节稳定。已取得原生矢量时不计算光流；缺少矢量时可在游戏内 ImGui 面板开启自动光流并选择质量。": "Motion vectors help stabilize detail across frames. Optical flow is idle when native motion is available. Otherwise enable automatic optical flow and choose its quality in the in-game panel.",
    "DLSSNR 参数": "DLSSNR parameters",
    "语义 Mask：识别人物、车辆等物体，按区域控制 NR 强度。": "Semantic Mask recognizes objects such as people and vehicles to control NR strength by region.",
    "G/B/A 全图由背景强度控制；R 通道在识别区域内按各组强度控制，区域外取背景强度。0 对应 0，1 对应 255，软边缘平滑混合。": "Background strength controls G/B/A across the image. R uses each group's strength inside detected regions and background strength elsewhere. Values 0 and 1 map to 0 and 255, with soft edge blending.",
    "这里只是工具侧的事件。core 在游戏进程里的详细日志（含 NGX 自己的输出、 卡顿转储）在日志文件夹里。": "This page shows launcher events. Detailed core and NGX logs and stall dumps are in the log folder.",
    "搜索游戏名或 exe…": "Search game name or .exe…",
    "搜索游戏": "Search games",
    "筛选游戏": "Filter games",
    "选择要查看的注入目标": "Select an injected game",
    "DLSSNR 自己的 GPU 耗时（不含游戏本身）": "DLSSNR GPU time (excluding the game)",
    "自定义…": "Custom…",
    "DLAA — 原生分辨率，只提升抗锯齿/稳定性": "DLAA — native resolution, improved anti-aliasing and stability",
    "真超分 — 压低游戏渲染分辨率再放大": "Upscaling — render the game at a lower resolution and upscale",
    "已生效，正在自动保存…": "Applied; saving automatically…",
    "（已清空）": "(cleared)",
    "未知（宿主没报告）": "Unknown (not reported)",
    "早于 swapchain 创建（最佳）": "Before swapchain creation",
    "晚于 swapchain 创建（已通过重建补上）": "After swapchain creation (recovered on recreation)",
    "晚于 swapchain 创建 —— 真超分/原生深度受影响": "After swapchain creation — upscaling/native inputs may be unavailable",
    "已建立": "Created",
    "未建立": "Not created",
    "没有卡过": "No stalls",
    "未启用": "Disabled",
    "没装上（注入太晚？）": "Hooks unavailable (late injection?)",
    "原因未知（看日志）": "Unknown reason (see logs)",
    "DLSS5 还没初始化好（再等几帧）": "NR is initializing",
    "还没从游戏的 barrier 里观察到颜色/矢量的状态": "Waiting for observed color/motion resource states",
    "游戏的颜色格式我们做不了（日志里有具体哪一项不行）": "Unsupported color format (see logs)",
    "游戏颜色的尺寸和我们准备的不一致": "Color resource dimensions do not match",
    "NGX 报错了（详情看日志）": "NGX error (see logs)",
    "正在切换处理路径，等待 GPU 完成上一条路径": "Switching routes; waiting for the previous GPU work",
    "HDR 压缩着色器没就绪": "HDR encoding shader is not ready",
    "命令列表状态记账没装上（少了它会闪退，所以主动拒绝）": "Command-list state tracking is unavailable; processing paused",
    "Present（显示图像）": "Present (display image)",
    "NR 暂停：等待游戏 SR 恢复；FG 状态未知，自动回退尚未验证": "NR paused: waiting for native SR; FG state is unknown and automatic fallback is unverified",
    "等待 NR 初始化": "Waiting for NR initialization",
    "原生运动矢量": "Native motion vectors",
    "光流运动矢量": "Optical-flow motion vectors",
    "零矢量": "Zero motion",
    "是": "Yes",
    "否（使用光流或零矢量，见游戏内面板）": "No (optical flow or zero motion; see in-game panel)",
    "正在使用游戏原生 DLSS；DXL 的额外 SR 已停用，NR 可继续处理。": "Using native game DLSS. DXL's additional SR is disabled; NR can continue.",
    "已加载": "Loaded",
    "工具启动": "Launched by DXL",
    "当前配置：{name}": "Current profile: {name}",
    "卡过 {count} 次（转储在日志里）": "{count} stalls (dumps in log folder)",
    "已补 {count} 个模块，等游戏解析 NGX": "{count} modules hooked; waiting for NGX lookup",
    "已接上（{modules} 个模块 / {lookups} 次解析）": "Connected ({modules} modules / {lookups} lookups)",
    "Evaluate（SR → NR，已处理 {count} 帧）": "Evaluate (SR → NR, {count} frames processed)",
    "等待可用路径：{reason}": "Waiting for an available route: {reason}",
    "原生 DLSS 之后（Evaluate 已处理 {count} 帧）": "After native DLSS ({count} Evaluate frames)",
    "Evaluate 未运行：{reason}": "Evaluate is idle: {reason}",
    "光流运动矢量（{time} ms）": "Optical-flow motion vectors ({time} ms)",
    "{target} · 效果 OFF（Del 打开）": "{target} · Effects OFF (Del to enable)",
    "SR {sr} / NR {nr}（失败 {a}/{b}）": "SR {sr} / NR {nr} (failed {a}/{b})",
    "未知错误": "Unknown error",
    "未处理的 Promise 拒绝": "Unhandled Promise rejection",
    "已请求打开日志文件夹": "Requested the log folder",
    "已请求打开 DLL 文件夹": "Requested the DLL folder",
    "不支持此按键，请使用字母、数字、功能键或导航键。": "Unsupported key. Use a letter, number, function key or navigation key.",
    "该快捷键已用于另一个动作，请先修改原绑定或选择其他按键。": "This shortcut is already assigned. Change the existing binding or choose another key.",
    "正在扫描…装了很多游戏的话要几秒。": "Scanning… this may take a few seconds.",
    "正在检查游戏路径…": "Checking game paths…",
    "语义 Mask（试验）": "Semantic Mask (Experimental)",
    "语义模型已安装并随完整包集成，可在游戏内启用实验性语义 Mask。": "The complete package includes Semantic Mask. Enable it in game when needed.",
    "语义模型已安装并随完整包集成，可在游戏内启用实验性蒙版。": "The complete package includes Semantic Mask. Enable it in game when needed."
});

    const LOG_FRAGMENTS = {
    "⚠ 拿不到游戏进程的路径（pid ": "Cannot read the game process path (PID ",
    "）—— 配置同步会找不到目标，参数会写进当前选中的配置。": "). Settings will be saved to the currently selected profile.",
    "常见原因：游戏以管理员身份运行而本工具不是。": "The game may be elevated while DXL is not.",
    "该进程已经注入过，直接接上（总开关：开）。": "Connected to the already injected process (effects ON).",
    "该进程已经注入过，直接接上（总开关：关，按已设置的效果快捷键打开效果）。": "Connected to the already injected process (effects OFF; press your configured effects shortcut to enable).",
    "注入失败：": "Injection failed: ",
    "注入成功，已连接到 pid ": "Injection succeeded; connected to PID ",
    "。**效果默认是关的 —— 切到游戏窗口按已设置的效果快捷键打开**，按过之后这个选择会存进配置，下次自动照做。": ". Effects are initially off. Press your configured effects shortcut in the game to enable them; your choice is saved.",
    "DLL 已加载，但 2 秒内没等到 core 的状态块。看日志：": "The DLL loaded, but core status was not available within 2 seconds. See: ",
    "快捷键无法识别（支持单键或组合键）：": "Unrecognized shortcut (single keys and combinations are supported): ",
    "快捷键注册失败（可能被别的程序占用）：": "Could not register shortcut (possibly used by another app): ",
    "拿不到前台窗口": "No foreground window found",
    "前台窗口是本程序自己 —— 请切到游戏窗口再按快捷键。": "Switch to a game window before pressing the shortcut.",
    "检测到游戏自带 DLSS / Streamline。DXL 会优先捕获原生 Evaluate；若延迟加载未捕获，建议下次通过部署代理尽早加载。": "Native DLSS / Streamline detected. DXL prefers native Evaluate. If late injection misses it, launch using the startup proxy next time.",
    "已停用并断开 pid ": "Disabled and disconnected PID ",
    "（hook 会留在进程里直到它退出；重新接上后按已设置的效果快捷键再打开效果）": "(hooks remain until the process exits; reconnect and press Del to enable effects)",
    "无法启动封面读取线程，请稍后重试。": "Unable to start cover loading. Please try again.",
    "全局监控已开：列表里的游戏一启动就自动注入。**这是赛跑，不保证赶在游戏建 swapchain 之前** —— 要百分百拿到原生矢量，还是用列表里的 ▶ 从工具启动。": "Global watch enabled: registered games are injected when they launch. Use the library's launch button for earlier injection and the best chance of capturing native inputs.",
    "全局监控已关。": "Global watch disabled.",
    "无法创建语义扩展文件夹。": "Unable to create the Semantic Mask folder.",
    "语义蒙版下载地址尚未设置：config\\extensions.json": "The Semantic Mask download link has not been configured.",
    "默认管理员启动选项保存失败，原设置保留。": "Could not save the administrator preference. The previous setting was kept.",
    "已保存：下次启动 DXL 时请求管理员权限。": "Saved: DXL will request administrator privileges on the next launch.",
    "已保存：下次启动 DXL 时使用普通权限。": "Saved: DXL will use standard privileges on the next launch.",
    "设置已保存，但 core 没有响应": "Settings saved, but the core did not respond",
    "设置保存失败": "Could not save settings",
    "设置已保存": "Settings saved",
    "设置已应用": "Settings applied",
    "配置文件名不合法，已拒绝写入": "Invalid profile filename; write rejected",
    "配置写入失败：": "Could not save profile: ",
    "配置已写入：": "Profile saved: ",
    "（色调=": "(tone=",
    "总开关：ON（已推送给正在运行的游戏）": "Effects ON (sent to the running game)",
    "总开关：OFF（已推送给正在运行的游戏）": "Effects OFF (sent to the running game)",
    "总开关：游戏没在跑，只存进配置（下一局启动生效）": "No game is running. The effects setting was saved for the next launch.",
    "总开关：ON —— 已存进配置，游戏启动时自动照此开启": "Effects ON — saved for the next game launch",
    "总开关：OFF —— 已存进配置，游戏启动时自动照此保持关闭": "Effects OFF — saved for the next game launch",
    "已打开日志文件夹：": "Opened log folder: ",
    "已打开 DLL 文件夹：": "Opened DLL folder: ",
    "这个配置还没设置游戏 exe 路径，无法从工具启动。": "Set the game executable path before launching from DXL.",
    "正在启动 ": "Launching ",
    "，等它起窗口后再注入（可能无法捕获原生 DLSS）…": ": waiting for its window before injection (native DLSS may be missed)…",
    "，进程一出现就注入…": ": injecting as soon as the process appears…",
    "创建启动线程失败": "Unable to start the launch worker",
    "检测到游戏自带 DLSS / Streamline，已保留原生功能。实际 NR 路径以游戏内面板为准；若持续停在 Present，请从工具重新启动游戏以便更早捕获 NGX。": "Native DLSS / Streamline is preserved. Check the NR route in the in-game panel. If it stays on Present, relaunch from DXL for earlier NGX capture.",
    "无法启动失效游戏检查，请稍后重试。": "Unable to check unavailable games. Please try again.",
    "正在扫描 Steam / Epic / GOG 的已安装游戏…": "Scanning installed Steam / Epic / GOG games…",
    "这一局是从工具启动的，**不能取消注入** —— 断开之后再注入就是迟到注入，拿不到游戏的原生深度和矢量，DLSSNR 只能吃零矢量，效果会静默变差。要临时关掉效果请按已设置的效果快捷键，它不动 hook。": "This game was launched by DXL. Keep the startup hooks connected so native inputs remain available. To disable effects temporarily, use the effect hotkey (Del by default).",
    "Debug 视图：切到下一个（关 → 深度 → 运动矢量）": "Next debug view (off → depth → motion)",
    "总开关：ON": "Effects ON",
    "总开关：OFF": "Effects OFF",
    "全局监控：看到列表里的游戏启动了（pid ": "Global watch: a registered game started (PID ",
    "），正在注入…": "); injecting…",
    "目标进程已退出，断开连接：": "Process exited; disconnected: ",
    "找不到 core DLL：": "Core DLL not found: ",
    "打开目标进程被拒绝。游戏以管理员身份运行时，本程序也需要管理员权限。": "Access to the game process was denied. Run DXL as administrator if the game is elevated.",
    "OpenProcess 失败，错误码 ": "OpenProcess failed, error ",
    "目标是 32 位进程。NGX 只有 x64 版本，32 位游戏需要另外的方案。": "The target is 32-bit. This NGX integration requires a 64-bit game.",
    "VirtualAllocEx 失败，错误码 ": "VirtualAllocEx failed, error ",
    "远程 LoadLibrary 线程 10 秒没返回 —— 目标进程极可能卡在 loader lock 上（注入太早，它自己还在加载依赖）。": "Remote LoadLibrary did not return within 10 seconds; the game may still be loading dependencies.",
    "等远程线程失败，错误码 ": "Waiting for the remote thread failed, error ",
    "LoadLibraryW 在目标进程里返回 NULL —— DLL 加载失败。常见原因：缺少依赖，或杀软拦截。": "LoadLibraryW returned NULL in the target process. Check for missing dependencies or security software blocking the DLL.",
    "CreateRemoteThread 失败，错误码 ": "CreateRemoteThread failed, error ",
    "WriteProcessMemory 失败，错误码 ": "WriteProcessMemory failed, error ",
    "这个游戏已经在跑，而且已经注入好了（pid ": "This game is already running and injected (PID ",
    "）——**没有再开一个实例**。要重新来一次请先关掉游戏。": "). No second instance was launched. Exit the game before relaunching.",
    "启动失败，错误码 ": "Launch failed, error ",
    "core 没有在 10 秒内报告 hook 装好，仍然放游戏继续跑（旁听功能可能没接上）": "Core hooks were not reported within 10 seconds. The game will continue; native capture may be unavailable.",
    "挂起注入失败：": "Suspended-process injection failed: ",
    "但它 20 秒内还没开始渲染": "but rendering has not started within 20 seconds",
    "注入成功（pid ": "Injection succeeded (PID ",
    "）。这个游戏加载慢，20 秒了还没出画面，属于正常 —— **请耐心等它出图，不要重复点启动**：再点会开出第二个实例，那个实例注入得晚，旁听接不上，功能反而用不了。": "). The game is still loading. Wait for its first frame instead of launching another instance.",
    "已从工具启动并注入 pid ": "Launched and injected PID ",
    "（挂起启动注入，早于游戏任何代码）": "(injected before game code ran)",
    "（已确认在渲染）": "(rendering confirmed)",
    "，跳过了 ": ", skipped ",
    " 个启动壳进程": " launcher processes",
    "。注意：真身不是我们创建的那个进程（启动壳转手），所以没能用上挂起注入。": ". The game was started by a launcher, so suspended-process injection was not available.",
    "游戏已启动，但超时内没找到进程 ": "The game launched, but this process was not found before timeout: ",
    "。如果是 Steam 游戏，确认配的是真正的游戏 exe 名。": ". For Steam games, select the actual game executable.",
    "找到了 ": "Found ",
    " 个同名进程，但没有一个满足注入条件": " matching processes, but none met the injection conditions",
    "（晚注入要等游戏出现主窗口，超时内没等到）。可以改用「尽早」注入，或等游戏进去了用快捷键手动注入。": "(no game window appeared before the late-injection timeout). Try Early injection or use the shortcut after the game loads.",
    "最后一次注入的错误：": "Last injection error: ",
    " 个同名进程都注入过，但没有一个开始渲染。": " matching processes were injected, but none started rendering.",
    "已自动为 ": "Created a profile for ",
    " 新建配置": "",
    "还没设置 exe 路径。": "has no executable path.",
    "正在扫描…装了很多游戏的话要几秒。": "Scanning… this may take a few seconds.",
    "正在检查游戏路径…": "Checking game paths…",
    "没扫到已安装的游戏。可能是 Steam/Epic/GOG 都没装，": "No installed games found. Steam/Epic/GOG may not be installed, ",
    "也可能库不在默认位置（先打开一次客户端再扫）。": "or a library may be unavailable. Open the game client and scan again.",
    "已清除 ": "Removed ",
    " 个失效游戏。": " unavailable games.",
    "没有失效游戏，列表保持不变。": "No unavailable games were found.",
    " 个路径暂时无法访问，已保留。": " paths are temporarily inaccessible and were kept.",
    "扫到 ": "Found ",
    " 个已安装的游戏，新增了 ": " installed games; added ",
    " 个，都已经在列表里了。": " games; all are already registered.",
    " 个；同路径已有的不会重复添加。": "; existing paths are not added again.",
    "已扫描 ": "Scanned ",
    " 款游戏": " games",
    "全局监控：": "Global watch: ",
    " 在黑名单里（专业/壁纸软件），不会自动注入": " is excluded from automatic injection (productivity/wallpaper app)",
    "已有同一个 exe 的配置「": "An existing profile uses the same executable: ",
    "」，直接切过去，不重复添加。": "; switched to it without adding a duplicate.",
    "界面脚本出错：": "UI script error: ",
    "未知错误": "Unknown error",
    "未处理的 Promise 拒绝": "Unhandled Promise rejection",
    "已请求打开日志文件夹": "Requested the log folder",
    "已请求打开 DLL 文件夹": "Requested the DLL folder",
    "（pid ": "(PID ",
    "（总开关：": "(effects: ",
    "创建启动进程失败": "Could not create the launch process"
};
    // Windows passes its UI language explicitly; browser locale is a standalone fallback.
    const query = new URLSearchParams(window.location ? window.location.search : '');
    function systemLang() {
        const native = query.get('systemLang');
        if (native === 'zh' || native === 'en') return native;
        const locale = String(navigator.language || navigator.userLanguage || 'en').toLowerCase();
        return locale.startsWith('zh') ? 'zh' : 'en';
    }
    let current = ['zh','en'].includes(query.get('lang')) ? query.get('lang') : systemLang();
    const normalize = text => String(text).trim().replace(/\s+/g, ' ');
    const escapeRE = text => text.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
    const tables = {};
    function tableFor(lang) {
        if (tables[lang]) return tables[lang];
        const exact = new Map(), formats = [];
        for (const [zh,en] of Object.entries(EN)) {
            const target = lang === 'en' ? en : zh;
            for (const from of [zh,en]) {
                const normalized = normalize(from);
                if (!/\{\w+\}/.test(from)) { exact.set(normalized,target); continue; }
                const names = [];
                const pattern = normalized.split(/(\{\w+\})/).map(part => {
                    if (/^\{\w+\}$/.test(part)) { names.push(part.slice(1,-1)); return '(.+?)'; }
                    return escapeRE(part);
                }).join('');
                formats.push({pattern:new RegExp('^'+pattern+'$'),names,target});
            }
        }
        return tables[lang] = {exact,formats};
    }
    function lookup(text, depth = 0) {
        const normalized = normalize(text);
        const {exact,formats} = tableFor(current);
        if (exact.has(normalized)) return exact.get(normalized);
        if (depth > 3) return null;
        for (const f of formats) {
            const match = f.pattern.exec(normalized);
            if (!match) continue;
            return f.target.replace(/\{(\w+)\}/g, (_,name) => {
                const value=match[f.names.indexOf(name)+1];
                return lookup(value,depth+1) ?? value;
            });
        }
        return null;
    }
    function apply() {
        const walker = document.createTreeWalker(document.body,NodeFilter.SHOW_TEXT,{
            acceptNode(node) {
                const parent=node.parentElement;
                if (!parent || parent.closest('script,style,[data-i18n-skip],#logView')) return NodeFilter.FILTER_REJECT;
                return node.nodeValue.trim() ? NodeFilter.FILTER_ACCEPT : NodeFilter.FILTER_REJECT;
            }
        });
        const nodes=[]; for(let n=walker.nextNode();n;n=walker.nextNode()) nodes.push(n);
        for(const node of nodes) {
            const hit=lookup(node.nodeValue);
            if(hit===null) continue;
            const leading=node.nodeValue.match(/^\s*/)[0], trailing=node.nodeValue.match(/\s*$/)[0];
            const next=(leading || hit.match(/^\s*/)[0])+hit.trim()+(trailing || hit.match(/\s*$/)[0]);
            if(node.nodeValue!==next) node.nodeValue=next;
        }
        for(const el of document.querySelectorAll('[placeholder],[title],[aria-label]')) {
            if(el.closest('[data-i18n-skip]')) continue;
            for(const attr of ['placeholder','title','aria-label']) {
                const value=el.getAttribute(attr); if(!value) continue;
                const hit=lookup(value);
                if(hit!==null && hit!==value) el.setAttribute(attr,hit);
            }
        }
    }
    function log(text) {
        if (current !== 'en') return text;
        const hit = lookup(text);
        if (hit !== null) return hit;
        for (const from of Object.keys(LOG_FRAGMENTS).sort((a,b)=>b.length-a.length))
            text = text.split(from).join(LOG_FRAGMENTS[from]);
        return text;
    }
    window.I18N={t(text){return lookup(text) ?? text;},log,systemLang,lang(){return current;},
        set(lang){current=lang==='en'?'en':'zh';document.documentElement.lang=current==='en'?'en':'zh-CN';apply();
            window.dispatchEvent(new Event('dxl-language-changed'));},apply};
    document.documentElement.lang=current==='en'?'en':'zh-CN';
    apply();
    // Catch text produced by individual controls between full render passes.
    if(typeof MutationObserver!=='undefined') {
        let queued=false;
        new MutationObserver(()=>{if(queued)return;queued=true;queueMicrotask(()=>{queued=false;apply();});})
            .observe(document.body,{subtree:true,childList:true,characterData:true,attributes:true,
                attributeFilter:['title','placeholder','aria-label']});
    }
})();
