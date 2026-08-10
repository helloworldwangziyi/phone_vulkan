#pragma once

// esx 视图 ABI：App 侧通过句柄操作 core 内的视图树。
//
// 线程契约：本文件所有函数只允许在 UI 线程调用
// （Android 上即 NativeBridge 各 native 方法所在线程）。

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 视图句柄。0 为无效句柄。
typedef uint32_t esx_view;

typedef enum {
    ESX_VIEW_GROUP = 0, // 容器，仅组织子视图
    ESX_VIEW_RECT  = 1, // 可设背景、可直接绘制
} esx_view_type;

// 创建视图。parent=0 表示暂不挂载（之后可作为根视图）；
// 否则作为 parent 的子视图（挂载在最上层）。
esx_view esx_create_view(esx_view_type type, float x, float y, float w, float h, esx_view parent);

// 递归销毁子树，注销所有相关句柄。
void esx_destroy_view(esx_view view);

// 设置根视图。必须是 parent=0 创建的、尚未挂载的视图。
void esx_set_root_view(esx_view view);

void esx_view_set_bounds(esx_view view, float x, float y, float w, float h);
void esx_view_set_visible(esx_view view, int32_t visible);
void esx_view_set_background(esx_view view, uint32_t rgba); // 0xRRGGBBAA
void esx_view_clear_background(esx_view view);

// 以下两个绘制函数只在 Draw 事件回调内有效（操作"当前帧 canvas"），
// 回调外调用会被忽略并告警。
// 坐标均为相对该视图左上角的局部坐标；绘制位置 = view.actual + 局部坐标，
// clip = 视图自身 actual 矩形与父链各视图矩形的交集（沿 parent 链逐级 intersect）。
void esx_draw_rect(esx_view view, float x, float y, float w, float h, uint32_t rgba);
void esx_draw_triangle(esx_view view,
                       float x1, float y1, float x2, float y2, float x3, float y3,
                       uint32_t c1, uint32_t c2, uint32_t c3); // 三色均为 0xRRGGBBAA

#ifdef __cplusplus
} // extern "C"

namespace evk::ui {
class View;
class Canvas;
}

// ---- core 内部函数（非 ABI，供 core/平台壳使用）----

// 当前根视图（未设置返回 nullptr）。
evk::ui::View* esxRootView();

// 帧构建：重算 actual、清 canvas、画背景、逐可见视图发 Draw 事件。
void esxBuildFrame(evk::ui::Canvas& canvas);

// 可选点击合成 helper：app 在收到原始 Touch 后可调用这里，
// DOWN 命中记录目标，MOVE 累计位移超 12px 取消，
// UP 未取消则发 UiClick，CANCEL(action=3) 重置。
void esxDispatchTouch(int32_t action, float x, float y);
#endif
