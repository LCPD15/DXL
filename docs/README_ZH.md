# DXL — DLSS eXtended Loader

[English](README_EN.md) | **简体中文** · **版本 0.6**

DXL（DLSS eXtended Loader）基于我之前的 RE DLSS5 Load Mod 扩充，目的是尽可能方便地在各类游戏中启用 DLSS NR。对于支持的游戏，工具可获取原生运动矢量并与帧生成配合，提供更高质量的效果；具体兼容性取决于游戏。

[前作 GitHub](https://github.com/LCPD15/RE_DLSS5_Load_Mod) · [前作 Nexus Mods](https://www.nexusmods.com/onimushawayofthesword/mods/32) · [DXL 项目](https://github.com/LCPD15/DXL)

[下载完整包](https://github.com/LCPD15/DXL/releases/latest) · [中文版在线说明](https://github.com/LCPD15/DXL/blob/v0.6/docs/DXL-Guide-ZH.md)

## 特征

- **真实运动矢量**——利用支持游戏自身的 DLSS 运动数据，让时间处理跟随场景，减少闪烁。
- **更高的帧速率**——降低神经渲染内部分辨率以节省 GPU 时间，同时保持图像清晰。
- **无需任何配置**——不需要在游戏内安装任何dll、asi、addon文件，使用本工具启动游戏即可，默认快捷键在游戏内按Del开关效果，按End开关参数界面
- **游戏内菜单**——无需离开游戏即可调整每个参数并查看调试信息。

以下为前作的效果与性能示例；图中为旧版界面，DXL 默认快捷键为 Del / End。RE 引擎游戏的前置要求见下方安装说明。

![前作：不同 NR 渲染缩放的画面与帧率对比](https://github.com/user-attachments/assets/b922778a-3d6e-4612-961a-fa8e3cfa08d0)

![前作：原生运动矢量与帧生成配合的效果示例](https://github.com/user-attachments/assets/459a24ee-18bd-4652-a0af-f277ebe79c58)

<img width="1546" height="1043" alt="image" src="https://github.com/user-attachments/assets/3e97a4c2-1491-43a0-aa9f-dcb4f7a576ce" />



## 安装并启动游戏

需要 Windows x64、兼容的 NVIDIA RTX 显卡和 Microsoft WebView2 Runtime。RE 引擎游戏按配置页提示安装 [REFramework](https://github.com/praydog/REFramework-nightly/releases) 和 [ReShade](https://www.reshade.me/#download)。

1. 完整解压发布包，运行 DXL.exe。保留包内全部文件。
2. 在游戏库扫描已安装游戏，或点击“添加游戏”选择实际游戏 EXE。点击封面进入配置，检查路径；如游戏需要启动参数，在本游戏的启动项填写。
3. 首次保留“自动”，点击“从工具启动游戏”。也可保持 DXL 和全局监控开启，再从平台或官方启动器启动已加入库的游戏。
4. 进入游戏后按 Del 开启 NR，按 End 打开面板。首次总开关默认关闭；后续保留该游戏保存的状态。调整参数会自动保存。

## 游戏内快捷键

Del：开关效果  •  End：开关面板  •  Alt + F8：对当前游戏窗口手动尝试加载。Esc 或面板右上角 × 可关闭面板。快捷键可在全局设置修改。

如果加载无效或启动异常，退出游戏，切换“兼容模式”后重试。兼容模式可能无法获取原生 DLSS 数据；需要帧生成时，优先使用游戏内 DLSS 超分，并尝试“自动”或“尽早”。

## 调整 NR 效果

按 End 打开面板，在同一画面一次调整一项，再按 Del 对比。表中默认值适用于新配置。

| 参数 | 范围与默认 | 作用 |
| --- | --- | --- |
| 强度 Intensity | 0–1 / 1 | 调节增强强度。调低强度不会停止模型计算。 |
| 色彩强度 Colour Strength | 0–1 / 1 | 0 保留原图颜色比例，仍保留 NR 明暗与细节；1 保留 NR 色彩。 |
| 风格与预设 Style / Preset | 电影风格 预设 0 | 按喜好比较默认、自然、电影风格；预设先保留默认。 |
| 局部色调与结构 Local Tone / Structure | 0–2 / 1 | 分别调节局部色调响应和结构增强。 |
| 皮肤强度 Skin Strength | 面板 0–1 默认 0.6 | 调节皮肤相关结构；过高不一定更自然。 |
| NR 渲染缩放 NR Render Scale | 0.5–1 / 1 | 降低 NR 处理分辨率以节省性能，最终游戏输出分辨率不变。 |
| 自叠层 Self Layers | 1–3 连续值 默认 1 | 放大 NR 效果差值，不增加模型推理次数。 |
| 真叠层 True NR Layers | 1–5 整数 默认 1 | 连续执行多次 NR，增加 GPU 耗时和显存占用。 |
| 自动光流 Optical Flow | 默认开启 平衡档 | 优先使用已启用且可用的原生矢量，否则计算光流。性能、平衡、质量三档可选。 |

性能不足时，先将真叠层设为 1，再降低 NR 缩放，必要时降低光流质量。Auto Mask 和 UI Correction 可先保留默认；正常看画面时将 Debug view 设为 Off。

## 调色与滤镜

按 End 打开面板，在 **DLSSNR** 旁选择“**调色**”。开关紧邻参数值；重置只恢复默认数值，保留开关状态。设置按游戏分别保存，关闭的效果跳过处理。

基础调色包括曝光、对比度、饱和度、色温、色偏、阴影、中间调和高光，保留在游戏图像阶段。在原生 SR 路径中通常位于游戏 UI 之前，也可能受到游戏后续曝光与色调映射影响。

LUT、锐化、柔光、暗角、胶片颗粒、黑白与自定义效果改在最终画面上处理，也会影响游戏 UI。后处理不依赖 NR 开关或 NR 菜单暂停；需要可用的安全呈现路径，仍受帧生成兼容性保护。

锐化和柔光强度范围均为 0–3。展开“柔光细节”可调高光阈值（0–2，默认 0.65）、阈值柔和度（0–1，默认 0.5）、光晕半径（0.25–3，默认 1）、扩散（0–1，默认 0.7）及光晕饱和度（0–2，默认 1）。细节参数随柔光开关启用；各自的重置只恢复数值。

将 PNG 调色表放入 `DXL.exe` 旁的 `lut` 文件夹，再在面板选择文件、开启 LUT 并设置强度。附带 `Neutral-16.png` 中性模板；另存自制 LUT 时使用新文件名，不要缩放或裁切。普通照片不能作为 LUT，布局说明见 `lut/README.md`。

在工具的 **额外功能 → 后处理** 打开 `post-processing` 文件夹，放入原生 ReShade `.fx` 及所需 `.fxh`、纹理，保持相对目录结构，再点击游戏内“调色 → 自定义 FX”中的“重新加载自定义 FX”。每个 technique 独立开关，参数自动显示并按游戏保存；重置只恢复数值。原文件不转换、不归档，原有 DXL FX 示例仍可使用。DXL 效果先执行，ReShade 文件在其后按名称排序。制作及目录说明见 `post-processing/README.md`。

支持 ReShade 多遍处理和外部纹理；当前不提供游戏深度、运动矢量、add-on 或键鼠来源变量，需要这些输入的效果不在兼容范围内。修改随包示例时另存新文件名，更新会保留自行添加的文件。


## 语义蒙板为实验性功能

完整包已带识别模型，功能默认关闭。开启后，勾选人物、车辆等类别并调节区域强度；未勾选的对象按背景强度处理。可先试背景 1、人物 0，再用“蒙板预览（R）”检查位置。

强度范围 0–1，边缘羽化默认 8，可缓和硬边界。识别效果可能不好，特别是二次元、遮挡和小物体；出现误识别或不自然边缘时，可关闭此功能。

[构建与依赖](https://github.com/LCPD15/DXL/blob/v0.6/DEPENDENCIES.md) · [第三方组件](THIRD_PARTY_NOTICES.md) · [组件许可](LICENSE_STATUS.md)

## 开源许可

DXL 采用 [AGPL-3.0-only](LICENSE)，版权所有 (C) 2026 LCPD15。第三方组件保留各自许可。[对应源码说明](SOURCE_CODE.md)。

[自动更新说明](UPDATES.md)：启动时后台检查；下载完成后可安装重启或留待下次安装。
