#pragma once

// Flex 线性布局容器（Flutter Column/Row 的对应物）。
//
// 子视图按主轴依次排布：先扣除固定项（main≥0）与 margin，剩余空间按
// weight 比例分配给 weight>0 的项（Expanded 语义）；交叉轴默认 stretch
// 占满（减 margin_cross），cross≥0 时按 align 对齐。
// 容器 bounds 变化（旋转/尺寸调整）自动重排，App 不写 layout 函数。
//
// 规则（v1）：主轴方向每个子视图必须 main≥0（固定）或 weight>0（弹性）
// 二选一；不测量子视图固有尺寸。

#include <stdint.h>

#include "evk/esx_view.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum esx_flex_align {
    ESX_FLEX_ALIGN_STRETCH = 0,  // 交叉轴占满（cross<0 时的默认行为）
    ESX_FLEX_ALIGN_START   = 1,
    ESX_FLEX_ALIGN_CENTER  = 2,
    ESX_FLEX_ALIGN_END     = 3,
} esx_flex_align;

typedef struct esx_flex_child {
    float main;              // 主轴固定 px；<0 = 由 weight 决定
    float weight;            // >0：按比例分主轴剩余空间
    float cross;             // 交叉轴固定 px；<0 = stretch（或按 align）
    esx_flex_align align;    // cross≥0 时的交叉轴对齐
    float margin_main_before;
    float margin_main_after;
    float margin_cross;      // 交叉轴两侧间距
} esx_flex_child;

// vertical!=0：主轴自上而下（Column）；否则主轴自左而右（Row）。
esx_view esx_flex_create(float x, float y, float w, float h, esx_view parent,
                         int32_t vertical);

// child 须 parent=0 创建、尚未挂载（挂载进 flex 后排布由容器接管）；
// 已是 flex 子视图时调用则更新 spec 并重排。spec 传 NULL 用全零默认。
void esx_flex_set_child(esx_view flex, esx_view child, const esx_flex_child* spec);

#ifdef __cplusplus
} // extern "C"
#endif
