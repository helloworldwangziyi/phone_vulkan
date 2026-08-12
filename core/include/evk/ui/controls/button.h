#pragma once

#include <stdint.h>

#include "evk/esx_view.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct esx_button_style {
    uint32_t normal_color;   // 0xRRGGBBAA
    uint32_t pressed_color;
    uint32_t disabled_color;
} esx_button_style;

typedef void (*esx_button_click_func)(esx_view button, void *user_data);

// Button 自己处理 pressed/cancel/up 和重绘，App 只接收一次语义化点击。
esx_view esx_button_create(float x, float y, float width, float height,
                           esx_view parent, const esx_button_style *style,
                           esx_button_click_func on_click, void *user_data);
void esx_button_set_enabled(esx_view button, int32_t enabled);
void esx_button_set_on_click(esx_view button, esx_button_click_func on_click,
                             void *user_data);

#ifdef __cplusplus
} // extern "C"
#endif
