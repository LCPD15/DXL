# DXL 更新日志 / Changelog

## 0.1

- 修复启动期极小 D3D11 辅助窗口抢占主呈现设备，导致后续 D3D12 游戏画面无法运行 NR 或显示菜单的问题。此修复按表面尺寸判断，不依赖游戏名称。
- 修复晚注入时原生 SR 已运行，却因等待 Present 路由采纳队列而无法初始化 NR 的循环依赖；候选队列限定为同设备的 DIRECT 队列并保留有效引用。游戏内 UI 仍要求已确认的呈现队列。
- D3D12 设备确认后即初始化命令列表状态跟踪，避免 FG 从首帧跳过 Present 滤镜时遗漏 SR→NR 的前置初始化。
- 修复启动提示按钮的翻译函数作用域错误，避免工具状态界面持续抛出 JavaScript 异常。

Tiny D3D11 startup surfaces no longer claim the main presentation device and
block a later D3D12 game window. The rule is independent of game names.
Late-loaded native SR can now initialize NR without first entering the Present
filter route. Submission queue candidates retain a valid reference and must
belong to the selected device and use the DIRECT type. In-game UI still requires
a confirmed presentation queue. Command-list tracking starts when the D3D12
device is identified, including when FG bypasses the Present filter path.
Launch advice buttons now use the correct
translation function, fixing repeated JavaScript errors during status updates.

- 新增加载诊断：记录远程加载线程返回值、完整路径模块确认和核心启动阶段；区分“尝试注入”“DLL 存在”和“核心初始化完成”，不再把非零线程退出码直接当作加载成功。初始化诊断不改变渲染逻辑。
- 修复远程加载线程超时后过早释放其参数内存的问题。

Startup diagnostics now record the loader thread result, full-path module
confirmation and core initialization milestones. A nonzero thread exit code
alone no longer reports successful loading. Rendering behavior is unchanged.
The loader argument remains valid when thread completion cannot be confirmed.

- 修复 D3D10 游戏通过 D3D11 兼容接口加载时，画面拷贝和共享 fence 在错误的接口模式下执行，导致 NR 为 0 帧的问题。桥接执行期间临时使用私有 D3D11 状态，完成后还原游戏的接口模式和绑定；不增加逐帧 CPU 等待。

D3D10 games exposed through D3D11 compatibility interfaces now run bridge copies
and shared-fence operations inside a private D3D11 context state. The game's API
mode and bindings are restored afterwards. This fixes the zero-NR-frame failure
without adding steady-state CPU waits.

- 修复已观察到的合法无描述符堆状态被误判为无法恢复，导致部分成功 SR 帧跳过 NR 的问题；完成 NR 后正确恢复空堆与原生根绑定。此修复对所有 D3D12 游戏生效。
- 燕云十六声（yysls.exe）的 FG 缓冲区检测阈值特调默认值改为 8，其他游戏默认 4；保留明确保存过的自定义值，并在折叠参数处注明燕云特调。
- RE 引擎游戏配置名称旁新增 REFramework / ReShade 安装说明和官方下载链接，支持中英文。
- 日志按原因汇总 NR 跳过情况，区分绑定状态、GPU 同步、初始化等待和菜单暂停。

Observed legal D3D12 states with no descriptor heaps no longer cause successful
SR frames to miss NR. Empty heaps and the game's root bindings are restored
after processing. This is a general D3D12 fix. Where Winds Meet (yysls.exe)
defaults to an FG buffer threshold of 8; other games retain 4 and explicit
custom values are preserved. Its advanced setting identifies the tuned default.
RE Engine profiles now show bilingual REFramework / ReShade installation
instructions and official download links beside the game name. Bounded NR
diagnostics distinguish binding rejection, GPU waits, initialization and menus.

- 修复与 Steam 等覆盖层重复发现 Present/Present1 接口时的转发递归；保留接口地址，并按实现分别转发到原始入口。
- 修复原生 SR 在菜单中停止时，自动回退到仍可能由 FG 管理的 Present 链的问题；提高缓冲区阈值不再被视为这次交接安全的依据。
- 这类菜单期间显示“NR 暂停，等待游戏 SR 恢复”；有效 SR 返回后恢复，独立 Present 用法保留。
- FG 检测阈值改成默认折叠的高级参数，提示仅在工具能加载但 NR 无效果时尝试提高。

Present/Present1 forwarding now preserves interface entry addresses and uses a
separate original trampoline for each implementation, preventing recursive
forwarding when overlays such as Steam rediscover presentation methods.
Automatic routing holds an uncertain SR-to-Present handoff when native SR stops
in menus; an increased buffer threshold does not establish safe FG ownership.
The UI explains the pause and resumes its state when native SR succeeds again.
Standalone Present remains available. The FG threshold is now an advanced,
initially collapsed setting with guidance for diagnosing missing NR output.

- 新增 x64 OpenGL 3.2+ / D3D9 呈现桥接，复用 NR、光流、语义蒙板和游戏内面板。
- 为预先缓存的 D3D12 SR/RR Evaluate 地址增加精确导出捕获，排除 FG 和 Vulkan 接口。
- 修复长时间重建游戏窗口导致看门狗误判退出、NR 永久停止的问题；换链校验设备和队列。
- 链式注入跳过专用崩溃上报子进程，避免对崩溃收集器加载图形核心。
- 支持 D3D9 晚注入与 MSAA 回写，保留游戏设备状态；修正小型 .NET 游戏启动程序的扫描优先级。
- 游戏内语义功能名称统一为“语义蒙板 / Semantic Mask”。

Added x64 OpenGL 3.2+ / D3D9 presentation bridging with the existing NR,
optical-flow, semantic-mask and in-game UI features. Exact D3D12 SR/RR export
capture also handles previously cached Evaluate pointers while excluding FG
and Vulkan entry points. Window replacement no longer permanently disables
hooks through the freeze watchdog; presentation devices and queues are checked.
Dedicated crash-report helpers are excluded from child injection.


- 手动注入默认快捷键改为 Alt + F8，迁移旧默认并保留自定义绑定。
- 未启动游戏时的提示同时显示实际效果开关、面板与手动注入快捷键。
- 配置页新增 FG 检测阈值：默认 4、整数范围 2～16、重启游戏生效。
- 更新游戏库的启动方式说明。

Manual injection now defaults to Alt + F8, with legacy-default migration and
custom binding preservation. The idle hint shows the configured effects/panel
shortcuts and the successfully registered injection shortcut. The per-game FG
buffer threshold accepts 2–16, defaults to 4 and takes effect after restarting
the game. Library launch guidance has been updated.

- 注入时机改为三个互斥按钮，晚注入更名为兼容模式，并增加崩溃时的提示。
- 外部启动游戏时，自动监控统一等待窗口稳定，降低初始化期间注入的冲突风险；
  从工具启动仍优先使用受控早注入，GTA SA 的自动兼容策略保留。
- 显式尽早监控增加加载器稳定检查，无窗口时使用早注入初始化标记。
- 删除配置按钮加大；修改注入时机立即同步监控策略。

Injection timing now uses three exclusive buttons, with late injection labeled
Compatibility mode. Automatic monitoring waits for a stable window for all games
launched outside DXL; tool-owned launches retain controlled early injection and
the existing GTA SA compatibility policy. Explicit Early monitoring checks loader
stability and uses the startup marker before a window exists. The delete button
is larger, and timing changes update monitoring immediately.

- 删除配置按钮移至配置名称右侧，新增二次确认；取消或 Esc 不会移除配置。
- 修复全局监控绕过晚注入、与工具启动流程争抢注入的问题。
- GTA SA 重制版从工具以自动模式启动时使用晚注入兼容路径。
- 并发启动使用独立结果消息，同一游戏启动中不再重复启动。

The profile deletion button is beside the profile name and requires confirmation;
Cancel or Escape preserves the profile. Automatic monitoring now respects late
injection and yields to launches initiated by DXL. GTA SA Definitive Edition uses
late injection when launched from DXL in Automatic mode.
Concurrent launches have independent results, and duplicate pending launches are ignored.

- 窗口大小、位置及最大化状态在下次启动时恢复。
- 首次启动跟随 Windows 显示语言，补齐英文界面及状态提示。
- 默认勾选管理员启动；语义 Mask 默认关闭。
- 游戏库补充自动识别使用说明与反作弊风险提示。
- 版本号旁新增 @LCPD15 项目链接；发布文件夹包含版本号。

Window geometry and maximized state are restored on restart. The initial language
follows Windows, with complete English UI and status text. Administrator launch is
checked by default; Semantic Mask stays off by default. The library includes usage
and anti-cheat guidance. A project link appears beside the version number, and
release folder names include the version.

- 版本编号重新从 0.1 开始。
- 主程序、核心、注入器与通信名称统一为 DXL。
- 配置统一保存到 %LOCALAPPDATA%/DXL；首次运行可导入旧 AppData
  或程序同目录配置，保留原文件并优先使用已有的新配置。
- 完整包集成单个 YOLO11n-seg 模型和 TensorRT Lean；语义 Mask 保持实验性、
  默认关闭，边缘羽化默认值为 8。
- 将 SDK、模型、构建产物、截图、诊断与开发历史移出源码目录。
- 新增源码导出白名单、敏感信息检查、依赖获取说明及许可审核记录。

Version numbering restarts at 0.1. Branding is unified, settings
use the per-user DXL directory with non-destructive legacy import, and the complete
package includes the single semantic model. Development output and restricted
dependencies are external; source export is allowlisted and excludes Git history.
