#pragma once

/**
 * @file navigation.h
 * @brief 页面栈导航容器（push/pop 带 iOS 风格滑动转场）。
 */
#include <stdint.h>

#include "evk/esx_view.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 导航栏配色样式（0xRRGGBBAA）。
 */
typedef struct esx_navigation_style {
    uint32_t bar_color; ///< 导航栏背景 0xRRGGBBAA
    uint32_t bar_line_color; ///< 导航栏底部分隔线
    uint32_t back_button_color; ///< 返回按钮 normal
    uint32_t back_button_pressed_color; ///< 返回按钮 pressed
    uint32_t back_arrow_color; ///< 返回箭头颜色
} esx_navigation_style;

/**
 * @brief 页面被弹出、销毁前回调：App 在此释放挂在页面上的自有数据。
 *
 * 回调返回后 page 句柄立即失效。
 */
typedef void (*esx_navigation_pop_func)(esx_view nav, esx_view page, void *user_data);

/**
 * @brief 创建页面栈导航容器：push/pop 带 iOS 风格滑动转场，栈深 >1 时导航栏
 * 显示返回按钮，左缘 48px 内右滑可交互返回。
 *
 * 页面生命周期：页面用 esx_view_set_nav_callback 注册导航钩子（WILL/DID ×
 * ENTER/LEAVE），进入前刷新数据、离开前保存状态；WILL_* 返回非 0 可取消导航。
 *
 * @param nav_bar_height 导航栏高度 px；=0 时不显示导航栏
 * @param style 导航栏配色，可 NULL（用内置默认配色）
 * @return 导航容器视图句柄；失败返回 0。初始矩形 {0,0,0,0}，
 *         用 esx_view_set_bounds 赋予（导航栏/页面区域随 bounds 自动布局）
 *
 * @note Navigation 在 SDK 内部占用自身的 pan callback 和导航栏 Button 的
 *       pointer callback，App 只使用本头文件的接口。
 * @note 引擎暂无文字渲染，导航栏只提供返回箭头，不显示标题。
 */
esx_view esx_navigation_create(esx_view parent, float nav_bar_height,
                               const esx_navigation_style *style);

/**
 * @brief push 页面入栈。
 *
 * @param page 必须是 parent=0 创建、尚未挂载的视图；push 后其布局由 Navigation
 *        接管（页面区域 = 导航栏以下全部空间）
 * @param animated 非 0 播放滑动转场动画
 * @note 转场动画进行中调用会被忽略。
 */
void esx_navigation_push(esx_view nav, esx_view page, int32_t animated);

/**
 * @brief 弹出栈顶页面并销毁（on_pop 回调后句柄失效）。
 * @param animated 非 0 播放滑动转场动画
 * @note 栈深 ≤1 时告警忽略。
 */
void esx_navigation_pop(esx_view nav, int32_t animated);

/**
 * @brief 页面栈深度。
 * @return 栈内页面数；句柄无效返回 0
 */
int32_t esx_navigation_depth(esx_view nav);

/**
 * @brief 栈顶页面句柄。
 * @return 栈顶页面；空栈或句柄无效返回 0
 */
esx_view esx_navigation_top_page(esx_view nav);

/**
 * @brief 就地更换导航栏样式（主题切换时用；页面内容由事件总线广播换肤）。
 */
void esx_navigation_set_style(esx_view nav, const esx_navigation_style *style);

/**
 * @brief 设置页面弹出回调（页面销毁前通知 App 清理挂在页面上的自有数据）。
 */
void esx_navigation_set_on_pop(esx_view nav, esx_navigation_pop_func on_pop,
                               void *user_data);

#ifdef __cplusplus
} // extern "C"
#endif
