#pragma once

#include <stdint.h>

#include "evk/esx_view.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct esx_navigation_style {
    uint32_t bar_color;                 // 导航栏背景 0xRRGGBBAA
    uint32_t bar_line_color;            // 导航栏底部分隔线
    uint32_t back_button_color;         // 返回按钮 normal
    uint32_t back_button_pressed_color;
    uint32_t back_arrow_color;          // 返回箭头颜色
} esx_navigation_style;

// 页面被弹出、销毁前回调：App 在此释放挂在页面上的自有数据。
// 回调返回后 page 句柄立即失效。
typedef void (*esx_navigation_pop_func)(esx_view nav, esx_view page, void *user_data);

// 页面栈导航容器：push/pop 带 iOS 风格滑动转场，栈深 >1 时导航栏显示
// 返回按钮，左缘 48px 内右滑可交互返回。
// 页面生命周期：页面用 esx_view_set_nav_callback 注册导航钩子（WILL/DID ×
// ENTER/LEAVE），进入前刷新数据、离开前保存状态；WILL_* 返回非 0 可取消导航。
// Navigation 在 SDK 内部占用自身的 pan callback 和导航栏 Button 的
// pointer callback，App 只使用本头文件的接口。
// 引擎暂无文字渲染，导航栏只提供返回箭头，不显示标题。
// nav_bar_height=0 时不显示导航栏。
esx_view esx_navigation_create(float x, float y, float w, float h,
                               esx_view parent, float nav_bar_height,
                               const esx_navigation_style *style); // style 可 NULL

// page 必须是 parent=0 创建、尚未挂载的视图；push 后其布局由 Navigation
// 接管（页面区域 = 导航栏以下全部空间）。转场动画进行中调用会被忽略。
void esx_navigation_push(esx_view nav, esx_view page, int32_t animated);

// 弹出栈顶页面并销毁（on_pop 回调后句柄失效）。栈深 ≤1 时告警忽略。
void esx_navigation_pop(esx_view nav, int32_t animated);

int32_t esx_navigation_depth(esx_view nav);
esx_view esx_navigation_top_page(esx_view nav);
// 就地更换导航栏样式（主题切换时用；页面内容由事件总线广播换肤）。
void esx_navigation_set_style(esx_view nav, const esx_navigation_style *style);
void esx_navigation_set_on_pop(esx_view nav, esx_navigation_pop_func on_pop,
                               void *user_data);

#ifdef __cplusplus
} // extern "C"
#endif
