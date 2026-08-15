#pragma once

/**
 * @file button.h
 * @brief 带点击处理的 Button 控件。
 */
#include <stdint.h>

#include "evk/esx_view.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 按钮三态配色（0xRRGGBBAA）。
 */
typedef struct esx_button_style {
    uint32_t normal_color; ///< 0xRRGGBBAA
    uint32_t pressed_color; ///< 按下态 0xRRGGBBAA
    uint32_t disabled_color; ///< 禁用态 0xRRGGBBAA
} esx_button_style;

/**
 * @brief 语义化点击回调（按下+抬起都在按钮内才触发）。
 */
typedef void (*esx_button_click_func)(esx_view button, void *user_data);

/**
 * @brief 创建按钮：Button 自己处理 pressed/cancel/up 和重绘，App 只接收一次
 * 语义化点击。
 * @param style 三态配色，可 NULL（用内置默认配色）
 * @return 按钮视图句柄；失败返回 0
 */
esx_view esx_button_create(float x, float y, float width, float height,
                           esx_view parent, const esx_button_style *style,
                           esx_button_click_func on_click, void *user_data);

/**
 * @brief 启用/禁用按钮；禁用时取消进行中的手势并显示 disabled 色。
 */
void esx_button_set_enabled(esx_view button, int32_t enabled);

/**
 * @brief 更换点击回调。
 */
void esx_button_set_on_click(esx_view button, esx_button_click_func on_click,
                             void *user_data);

/**
 * @brief 就地换样式（声明式层重建时保留按下等内部状态）。
 */
void esx_button_set_style(esx_view button, const esx_button_style *style);

#ifdef __cplusplus
} // extern "C"
#endif
