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

typedef struct esx_view_click_event {
    float x; // 相对回调 View 左上角
    float y;
} esx_view_click_event;

typedef enum esx_view_pan_state {
    ESX_VIEW_PAN_BEGIN  = 0,
    ESX_VIEW_PAN_UPDATE = 1,
    ESX_VIEW_PAN_END    = 2,
    ESX_VIEW_PAN_CANCEL = 3,
} esx_view_pan_state;

typedef struct esx_view_pan_event {
    esx_view_pan_state state;
    float x;             // 相对回调 View 左上角
    float y;
    float delta_x;       // 相对上一次 Pointer 事件的位移
    float delta_y;
    float translation_x; // 相对本次手势 DOWN 点的总位移
    float translation_y;
    float velocity_x;    // 最近约 100ms 滑动速度（px/s），UPDATE/END 时有效
    float velocity_y;
} esx_view_pan_event;

typedef void (*esx_view_draw_func)(esx_view view, void *user_data);
typedef void (*esx_view_click_func)(esx_view view, const esx_view_click_event *event,
                                    void *user_data);
typedef void (*esx_view_pan_func)(esx_view view, const esx_view_pan_event *event,
                                  void *user_data);

// 创建视图。parent=0 表示暂不挂载（之后可作为根视图）；
// 否则作为 parent 的子视图（挂载在最上层）。
esx_view esx_create_view(float x, float y, float w, float h, esx_view parent);

// 递归销毁子树，注销所有相关句柄。
void esx_destroy_view(esx_view view);

// 设置根视图。必须是 parent=0 创建的、尚未挂载的视图。
void esx_set_root_view(esx_view view);

void esx_view_set_bounds(esx_view view, float x, float y, float w, float h);
void esx_view_set_visible(esx_view view, int32_t visible);
void esx_view_set_background(esx_view view, uint32_t rgba); // 0xRRGGBBAA
void esx_view_clear_background(esx_view view);

// 普通 View 可直接绑定语义化回调。标准控件（Button/ScrollView）会在 SDK
// 内部占用相应回调，App 只使用控件公开的 onClick/onScroll 接口。
void esx_view_set_draw_callback(esx_view view, esx_view_draw_func func, void *user_data);
void esx_view_set_click_callback(esx_view view, esx_view_click_func func, void *user_data);
void esx_view_set_pan_callback(esx_view view, esx_view_pan_func func, void *user_data);

// 以下两个绘制函数只在 View draw callback 内有效（操作"当前帧 canvas"），
// 回调外调用会被忽略并告警。
// 坐标均为相对该视图左上角的局部坐标；绘制位置 = view.actual + 局部坐标，
// clip = 视图自身 actual 矩形与父链各视图矩形的交集（沿 parent 链逐级 intersect）。
void esx_draw_rect(esx_view view, float x, float y, float w, float h, uint32_t rgba);
void esx_draw_triangle(esx_view view,
                       float x1, float y1, float x2, float y2, float x3, float y3,
                       uint32_t c1, uint32_t c2, uint32_t c3); // 三色均为 0xRRGGBBAA

#ifdef __cplusplus
} // extern "C"

#include <memory>

namespace evk::ui {
class View;
class Canvas;
}

// ---- core 内部函数（非 ABI，供 core/平台壳使用）----

// 把控件实现好的 View 节点挂进视图树并注册句柄，返回新句柄（0 表示失败）。
// 仅供 core/ui 控件以自定义 View 子类创建视图时使用。
esx_view esxAdoptViewNode(std::unique_ptr<evk::ui::View> view,
                          float x, float y, float w, float h, esx_view parent);

// 当前根视图（未设置返回 nullptr）。
evk::ui::View* esxRootView();

// 句柄查询仅供 core/ui 控件实现使用；App 仍只操作 esx_view 句柄。
evk::ui::View* esxViewFromHandle(esx_view view);

// 把 parent=0 创建、尚未挂载的 child 移动为 parent 的子视图（挂载在最上层）。
// child 已挂载、是当前根视图或句柄无效时告警并返回 false。仅供 core/ui 控件使用。
bool esxAdoptChild(esx_view parent, esx_view child);

// 帧构建：重算 actual、清 canvas，按 View 顺序执行背景、自定义绘制、子树绘制。
void esxBuildFrame(evk::ui::Canvas& canvas);
#endif
