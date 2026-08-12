#pragma once

#include "evk/esx_view.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*esx_scroll_func)(esx_view scroll_view, float offset_x, float offset_y,
                                void *user_data);

// 返回作为 viewport 的 View；子内容应挂到 esx_scroll_view_get_content()。
// content 会按 viewport 自动裁剪；只有可滚动的方向参与拖动，越界带橡皮筋
// 阻尼，松手按速度惯性滑动（fling），冲界后自动回弹。
esx_view esx_scroll_view_create(float x, float y, float width, float height,
                                float content_width, float content_height,
                                esx_view parent);
esx_view esx_scroll_view_get_content(esx_view scroll_view);
void esx_scroll_view_set_content_size(esx_view scroll_view, float width, float height);
void esx_scroll_view_set_offset(esx_view scroll_view, float offset_x, float offset_y);
void esx_scroll_view_get_offset(esx_view scroll_view, float *offset_x, float *offset_y);
void esx_scroll_view_set_on_scroll(esx_view scroll_view, esx_scroll_func on_scroll,
                                   void *user_data);

#ifdef __cplusplus
} // extern "C"
#endif
