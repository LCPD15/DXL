# Third-party components / 第三方组件

DXL uses the following SDKs, libraries and model. Copyright and license texts are provided in the accompanying `licenses/` directory.

DXL 使用以下 SDK、库与模型。相应版权声明与许可文本保存在 `licenses/` 目录。

Bloom's multiscale glow design references luluco250's MIT-licensed MagicBloom shader. Attribution and the license are in `licenses/MagicBloom-Reference-NOTICE.txt`.

柔光的多尺度光晕设计参考 luluco250 以 MIT 许可发布的 MagicBloom 着色器。署名与许可见 `licenses/MagicBloom-Reference-NOTICE.txt`。

| Component / 组件 | Purpose / 用途 | Project / 项目 |
|---|---|---|
| NVIDIA DLSS / NGX | Neural rendering and DLSS integration / 神经渲染与 DLSS 接入 | https://github.com/NVIDIA/DLSS |
| NVIDIA TensorRT Lean | Semantic model inference / 语义模型推理 | https://developer.nvidia.com/tensorrt |
| Ultralytics YOLO11n-seg | Semantic Mask / 语义 Mask | https://github.com/ultralytics/ultralytics |
| Microsoft WebView2 | Launcher interface / 启动器界面 | https://developer.microsoft.com/microsoft-edge/webview2/ |
| Dear ImGui | In-game interface / 游戏内界面 | https://github.com/ocornut/imgui |
| MinHook / HDE | API hooks / API 挂钩 | https://github.com/TsudaKageyu/minhook |
| AMD FidelityFX | Optical-flow motion estimation / 光流矢量估计 | https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK |
| Microsoft D3DX12 / PIX headers | Direct3D helpers and diagnostics / Direct3D 辅助与诊断 | https://github.com/microsoft/DirectX-Headers |
| ReShade 6.8.0 | ReShade FX compiler and effect runtime / ReShade FX 编译器与效果运行时 | https://github.com/crosire/reshade/tree/v6.8.0 |
| ReShade.fxh | ReShade FX standard shader helpers / ReShade FX 标准着色器辅助文件 | https://github.com/crosire/reshade-shaders |

NVIDIA CUDA and NVAPI interfaces use the installed NVIDIA driver.
NVIDIA CUDA 与 NVAPI 接口使用用户已安装的 NVIDIA 驱动。

NR color-conversion attribution and source links are in `licenses/NR-Color-Conversion-NOTICE.txt`.
NR 颜色转换部分的来源与许可声明见 `licenses/NR-Color-Conversion-NOTICE.txt`。

The private `DXL-ReShade.dll` is built from ReShade 6.8.0 with DXL-controlled lifecycle, configuration and presentation. ReShade's FX compiler and renderer are retained; its automatic hooks, add-on loading, overlay and separate update check are disabled. ReShade and its dependencies' notices are in `licenses/ReShade-LICENSE.txt` and `licenses/ReShade-ThirdParty-NOTICES.txt`.

私有的 `DXL-ReShade.dll` 基于 ReShade 6.8.0 构建，由 DXL 管理生命周期、配置和呈现。保留 ReShade FX 编译器与渲染器，关闭其自动挂钩、附加组件加载、浮层和独立更新检查。ReShade 及其依赖的许可文本见 `licenses/ReShade-LICENSE.txt` 和 `licenses/ReShade-ThirdParty-NOTICES.txt`。
