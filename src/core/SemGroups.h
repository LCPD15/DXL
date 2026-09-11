// 语义整合组总数（#81 双模型）：两边共用 —— SegMaskFilter（两张组表）
// 和 OverlayRaster（UI 网格/强度行）都按它循环。改组数只改这一处。
// COCO 12 组（人物..杂物）+ ADE20K 场景 6 组（建筑/植被/天空/水域/地形/其他场景）。
#pragma once

#include <cstdint>

namespace DXL {

// Keep 18 storage slots for existing settings and IPC; only COCO groups are active.
inline constexpr uint32_t SEM_GROUP_COUNT = 18;
inline constexpr uint32_t SEM_OBJECT_GROUP_COUNT = 12;
inline constexpr uint32_t SEM_OBJECT_GROUP_MASK = (1u << SEM_OBJECT_GROUP_COUNT) - 1;

}  // namespace DXL
