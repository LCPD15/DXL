# PNG LUT / PNG 调色表

## 中文

将自制或下载的 PNG LUT 放在 `DXL.exe` 同目录的 `lut` 文件夹内，在游戏内面板的“调色”分页选择文件、开启 LUT 并调整强度。普通照片和截图不能作为 LUT。

- 支持 RGB 三维查找表展开为二维 PNG：水平条带、垂直条带或矩形网格。
- 每个方块大小为 `N × N`，共 `N` 个方块，`N` 为 2–64。红色从左向右增加，绿色从上向下增加；每块代表一个蓝色切片，按从左到右、从上到下排列。
- 示例：16 阶为 `256 × 16` 或 `16 × 256`；64 阶可为 `512 × 512`（8 × 8 个 64 × 64 方块）。
- `Neutral-16.png` 为 16 阶中性模板。可在图像软件中只对它进行颜色调整，另存为新文件名。不要缩放、裁切、旋转、模糊或使用有损压缩。
- 自制文件请使用新名称；更新保留额外添加的文件，包内同名模板可能被更新替换。LUT 只调整颜色，不承载锐化、柔光等空间效果。

## English

Place your PNG LUTs directly in the `lut` folder beside `DXL.exe`. Select a file in the in-game **Color Grading** tab, enable LUT and adjust its strength. A regular photograph or screenshot is not a LUT.

- Supported layouts are a horizontal strip, vertical strip or rectangular tile grid containing a flattened RGB 3D lookup table.
- Each tile is `N × N` pixels, with exactly `N` tiles; `N` can be 2–64. Red increases from left to right, green from top to bottom. Tiles represent increasing blue, ordered left to right, then top to bottom.
- Examples: a 16-level LUT is `256 × 16` or `16 × 256`; a 64-level LUT can be `512 × 512` (an 8 × 8 grid of 64 × 64 tiles).
- `Neutral-16.png` is a neutral 16-level template. Apply color-only adjustments in an image editor and save it under a new name. Do not resize, crop, rotate, blur or use lossy compression.
- Give custom files new names. Updates preserve added files but may replace bundled templates with the same name. LUTs change colors; they cannot store spatial effects such as sharpening or bloom.
