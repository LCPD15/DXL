# DXL 后处理扩展 / Post-processing extensions

## 使用

在工具的“额外功能 → 后处理”打开此文件夹，放入 `.fx` 文件。现在支持 **原生 ReShade FX** 和原有 **DXL FX** 两种格式。原文件保留，不自动修改、转换或归档。游戏内“调色”页分别显示两种格式，文件按名称排序；每个游戏分别保存开关和参数，重置只恢复参数值，保留效果开关。

附带“复古棕褐”和“电影黑边”两个示例，默认关闭。LUT PNG 仍放在相邻 `lut` 文件夹。LUT、常用滤镜和 `.fx` 作用于最终输出画面，会影响游戏 UI。基础调色继续在原场景路径工作。

## 使用自定义 FX

保留作者提供的文件夹结构，同时复制 `.fxh` 依赖和纹理。不同子目录中的 `.fx` 文件名也必须唯一，避免预设名称冲突。例如：

```text
post-processing/
  MyEffect.fx
  ReShade.fxh
  ReShadeUI.fxh
  Shaders/
    AnotherEffect.fx
    Include/MyHelpers.fxh
  Textures/
    MyTexture.png
```

在“调色 → 自定义 FX”点击“重新加载自定义 FX”。每个 `.fx` 可以包含多个 technique，每个 technique 单独开关，新增效果默认关闭。开关在参数值旁边。效果内声明的布尔、整数、浮点数、颜色、列表、向量和数组会自动显示为控件；无需另外编写 DXL 参数声明。ReShade 效果在 LUT、常用滤镜及 DXL FX 之后运行，ReShade 文件按名称排列，同一文件保留 technique 声明顺序。

DXL 使用 ReShade 的效果编译器和运行时，支持 `#include`、technique/pass、多遍处理和外部纹理。**这不等于支持所有 ReShade 插件：当前没有提供游戏深度、运动矢量或 ReShade add-on 接口。** 需要 `DEPTH` 的景深、AO 等效果不能据此正常工作；只依赖最终画面颜色的调色、LUT、锐化、颗粒等适合直接使用。时间、帧号等运行时变量可用，依赖键盘、鼠标输入的 `source` 变量暂不支持。效果所需的自定义 `.fxh`、纹理和宏也必须一并提供；不同作者的 HDR 处理方式可能不同。

文件编译失败时，该效果不会正常加载。错误日志位于 `%LOCALAPPDATA%\DXL\logs\DXL-ReShade-<进程ID>.log`。逐游戏预设位于 `%LOCALAPPDATA%\DXL\profiles\<游戏.exe>.reshade.ini`，不会放入游戏目录。刷新会重新编译效果，大型效果包首次加载可能较慢。

## 编写 ReShade FX

这是一个完整的原生 ReShade 示例，不含 DXL 格式标记：

```hlsl
#include "ReShade.fxh"

uniform float Strength <
    ui_type = "slider";
    ui_label = "Red adjustment";
    ui_label_zh = "红色调整";
    ui_min = -1.0; ui_max = 1.0; ui_step = 0.01;
> = 0.0;

float4 Adjust(float4 position : SV_Position, float2 uv : TEXCOORD) : SV_Target {
    float4 color = tex2D(ReShade::BackBuffer, uv);
    color.r += Strength;
    return color;
}

technique RedAdjustment {
    pass { VertexShader = PostProcessVS; PixelShader = Adjust; }
}
```

常用标注：`ui_label`、`ui_tooltip`、`ui_category`、`ui_type`、`ui_min`、`ui_max`、`ui_step`、`ui_items`。`ui_type="color"` 用于 `float3/float4` 颜色，`ui_type="combo"` 用于整数选项，选项用 `ui_items="第一项\0第二项\0"`。可选的 `_zh`、`_en` 标签用于 DXL 双语显示。`source` 驱动变量与 `hidden` 变量不显示；`noedit` 变量只读。请使用普通 ReShade FX 语法和相对依赖路径，不要将原有 DXL `DXL_Effect` 入口混进同一文件。

## DXL FX 格式

原有 DXL FX 是单遍（一次全屏处理）HLSL 格式；它的限制不适用于上面的原生 ReShade FX。DXL FX 不支持 technique/pass、独立纹理、深度输入、跨帧纹理、脚本及 `#include`。每个文件最多 16 个浮点参数；最多显示 64 个 DXL 文件，同时执行最多 32 个 DXL 效果。启用时编译一次并缓存；未启用的 DXL 效果不编译、不执行。

## 制作

复制一个示例后，修改显示名称、参数和 `DXL_Effect`。文件名决定运行顺序，不区分英文大小写。参数按声明顺序保存；发布后请保持旧参数顺序并将新参数追加到末尾。

```hlsl
// @dxl name_en Red adjustment
// @dxl name_zh 红色调整
// @dxl param strength -1 1 0 | Strength | 强度
float3 DXL_Effect(float2 uv, float3 color) {
    color.r += DXL_Parameter(0);
    return color;
}
```

参数行格式：`// @dxl param 标识符 最小值 最大值 默认值 | 英文名 | 中文名`。标识符只使用英文字母、数字和下划线；值需有限，范围在 -10000 到 10000 内。名称标记可省略，省略时显示文件名。参数声明可省略，以制作只有开关的效果。

可用输入：

| 名称 | 说明 |
|---|---|
| `uv` | 当前像素中心的 0–1 坐标 |
| `color` | 前一效果处理后的 RGB |
| `DXL_Parameter(i)` | 第 i 个参数，从 0 开始 |
| `DXL_Size` | `uint2` 输出尺寸 |
| `DXL_TexelSize()` | 单个像素的 UV 尺寸 |
| `DXL_Frame` | 帧序号，可用于噪点 |
| `DXL_LinearInput` | 1 表示线性颜色输入，0 表示显示编码输入 |
| `DXL_Sample(uv)` | 线性采样当前输入，越界时限制在边缘 |
| `DXL_Read(int2 pixel)` | 读取指定像素，越界时限制在边缘 |

返回 `float3` RGB；DXL 自动保留原 alpha。RGB 不强制裁剪到 0–1；制作效果时请考虑 HDR 和负值。非有限输出会退回原像素。编译失败的文件单独跳过，其他效果继续执行，调色页显示文件错误。没有多遍渲染依赖系统；多个文件依次执行可组合成处理链，每个启用文件增加一次全屏处理和相应的显存读写。

## Usage

Open this folder from **Extras → Post-processing** and copy your `.fx` files into it. Both **native ReShade FX** and the original **DXL FX** format are supported. Source files remain intact: DXL does not rewrite, convert or archive them. The in-game **Color Grading** page lists each format separately in alphabetical filename order. Toggles and values are saved per game. Reset restores values while preserving effect toggle states.

The bundled **Sepia** and **Letterbox** examples start disabled. PNG LUTs belong in the adjacent `lut` folder. LUTs, built-in filters and `.fx` effects process the final displayed image, including game UI. Basic grading keeps its original scene route.

## Using Custom FX

Keep the author's folder structure and include the required `.fxh` files and textures. Effect filenames must be unique even across subfolders, to avoid preset name conflicts. Use the folder layout shown above: `.fx` files may be in `post-processing` or its `Shaders` tree, with textures in `Textures` and relative includes kept alongside their shaders.

Click **Reload custom FX** under **Color Grading → Custom FX**. Each `.fx` may define multiple techniques with independent enable switches; new effects start disabled. Declared boolean, integer, floating-point, color, list, vector and array parameters become controls automatically. No DXL metadata is required. ReShade effects run after LUTs, built-in filters and DXL FX, in alphabetical file order, preserving technique declaration order inside each file.

DXL uses ReShade's effect compiler and runtime, supporting `#include`, techniques/passes, multi-pass rendering and external textures. **This does not enable every ReShade add-on: game depth, motion vectors and the ReShade add-on interface are not currently provided.** Depth-dependent effects such as depth of field or AO cannot work correctly from this integration alone. Effects using final image color, including grading, LUTs, sharpening and grain, are suitable. Runtime sources such as time and frame count work; keyboard/mouse source uniforms are not currently supported. Include any custom headers, textures and preprocessor definitions required by the author. HDR assumptions vary between shaders.

An effect that fails compilation cannot load correctly. Compiler errors are logged to `%LOCALAPPDATA%\DXL\logs\DXL-ReShade-<process-ID>.log`. Per-game presets are stored in `%LOCALAPPDATA%\DXL\profiles\<game.exe>.reshade.ini`, outside the game folder. Reloading recompiles shaders; large collections may take longer on first load.

## Authoring ReShade FX

The native example above defines a full-screen pixel shader, a `Strength` uniform, and a technique/pass. Common annotations are `ui_label`, `ui_tooltip`, `ui_category`, `ui_type`, `ui_min`, `ui_max`, `ui_step`, and `ui_items`. Use `ui_type="color"` for `float3/float4` colors; use `ui_type="combo"` with integer uniforms and `ui_items="First\0Second\0"` for lists. Optional `_en` and `_zh` label annotations select DXL's interface language. Variables with `source` or `hidden` annotations are omitted; `noedit` variables are read-only. Use normal ReShade FX syntax and relative dependencies. Do not mix a DXL `DXL_Effect` entry point into a ReShade effect file.

## DXL FX format

The original DXL FX contract is single-pass HLSL. Its restrictions do not apply to native ReShade FX above. DXL FX does not support techniques/passes, additional textures, depth inputs, previous-frame textures, scripts or `#include`. DXL limits: 16 float parameters per file, 64 listed files and 32 simultaneously active effects. Enabled DXL effects compile once and are cached until refresh; disabled DXL effects neither compile nor run.

## Authoring

Copy an example and edit its names, parameters and `DXL_Effect`. Case-insensitive filename order determines execution order. Values are stored in declaration order: preserve existing parameter positions when publishing updates and append new parameters at the end.

Metadata syntax:

`// @dxl param identifier minimum maximum default | English label | Chinese label`

Identifiers use ASCII letters, digits and underscores. Numeric values must be finite and within -10000 to 10000. Display names and parameter declarations are optional. A file with no parameters still has its enable toggle.

The DXL-format HLSL example above demonstrates the entry point. `uv` is the normalized pixel center and `color` is the preceding effect's RGB output. `DXL_Parameter(i)` reads a zero-based parameter; `DXL_Size` is the output `uint2` size; `DXL_TexelSize()` returns its reciprocal. `DXL_Frame` provides a frame counter. `DXL_LinearInput` is 1 for linear input and 0 for display-encoded input. `DXL_Sample(uv)` performs clamped linear sampling; `DXL_Read(int2 pixel)` reads a clamped pixel.

Return RGB as `float3`. DXL preserves alpha and does not clamp RGB to 0–1; account for HDR and negative values. Nonfinite results fall back to the original pixel. Invalid files are skipped individually and their errors appear in the grading page. Multiple files form a sequential processing chain; each active file adds a full-screen pass and its GPU bandwidth cost.
