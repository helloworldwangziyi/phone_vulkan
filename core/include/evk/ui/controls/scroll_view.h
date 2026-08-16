#pragma once

/**
 * @file scroll_view.h
 * @brief 可滚动容器控件（viewport + content 双层结构）。
 */
#include "evk/esx_view.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 滚动回调：offset 落定（含动画帧）时通知。
 */
typedef void (*esx_scroll_func)(esx_view scroll_view, float offset_x, float offset_y,
                                void *user_data);

/**
 * @brief 创建可滚动容器，返回作为 viewport 的 View；子内容应挂到
 * esx_scroll_view_get_content()。
 *
 * content 会按 viewport 自动裁剪；只有可滚动的方向参与拖动，越界带橡皮筋
 * 阻尼，松手按速度惯性滑动（fling），冲界后自动回弹。
 * viewport 初始矩形 {0,0,0,0}，由 set_bounds 或父容器布局赋予。
 *
 * @param content_width 内容宽度 px（决定横向可滚动范围）
 * @param content_height 内容高度 px（决定纵向可滚动范围）
 * @return viewport 视图句柄；失败返回 0
 */
esx_view esx_scroll_view_create(float content_width, float content_height,
                                esx_view parent);

/**
 * @brief content 视图句柄（内容挂载点）。
 * @return content 句柄；句柄无效返回 0
 */
esx_view esx_scroll_view_get_content(esx_view scroll_view);

/**
 * @brief 更新内容尺寸（决定 maxOffset），并 clamp 吸附当前 offset。
 */
void esx_scroll_view_set_content_size(esx_view scroll_view, float width, float height);

/**
 * @brief 程序设定滚动 offset（clamp 到合法范围，并打断进行中的动画）。
 */
void esx_scroll_view_set_offset(esx_view scroll_view, float offset_x, float offset_y);

/**
 * @brief 读取当前显示 offset。
 * @param offset_x 输出 X 轴 offset，可传 NULL
 * @param offset_y 输出 Y 轴 offset，可传 NULL
 */
void esx_scroll_view_get_offset(esx_view scroll_view, float *offset_x, float *offset_y);

/**
 * @brief 设置滚动回调（每次 offset 落定、含动画帧时通知）。
 */
void esx_scroll_view_set_on_scroll(esx_view scroll_view, esx_scroll_func on_scroll,
                                   void *user_data);

#ifdef __cplusplus
} // extern "C"
#endif
