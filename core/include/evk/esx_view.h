#pragma once

/**
 * @file esx_view.h
 * @brief esx 视图 ABI：App 侧通过句柄操作 core 内的视图树。
 *
 * 线程契约：本文件所有函数只允许在 UI 线程调用
 * （Android 上即 NativeBridge 各 native 方法所在线程）。
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 视图句柄。0 为无效句柄。 */
typedef uint32_t esx_view;

typedef struct esx_view_click_event {
    float x; ///< 相对回调 View 左上角
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
    float x; ///< 相对回调 View 左上角
    float y;
    float delta_x; ///< 相对上一次 Pointer 事件的位移
    float delta_y;
    float translation_x; ///< 相对本次手势 DOWN 点的总位移
    float translation_y;
    float velocity_x; ///< 最近约 100ms 滑动速度（px/s），UPDATE/END 时有效
    float velocity_y;
} esx_view_pan_event;

typedef void (*esx_view_draw_func)(esx_view view, void *user_data);
typedef void (*esx_view_click_func)(esx_view view, const esx_view_click_event *event,
                                    void *user_data);
typedef void (*esx_view_pan_func)(esx_view view, const esx_view_pan_event *event,
                                  void *user_data);

/**
 * @brief 页面在导航容器中的生命周期事件（目前由 Navigation 触发）。
 *
 * 调用顺序（对照 estarx App view_init_func 的 8 态，前进/返回由 forward 区分）：
 * - push：       旧页 WILL_LEAVE → 新页 WILL_ENTER →（转场）→ 旧页 DID_LEAVE → 新页 DID_ENTER
 * - pop/左滑返回：顶页 WILL_LEAVE → 下页 WILL_ENTER →（转场）→ 顶页 DID_LEAVE → 下页 DID_ENTER
 *
 * 转场被取消（左滑回弹、尺寸变化吸附）时按最终归属收尾：留在台前的页面收
 * DID_ENTER，另一页收 DID_LEAVE——每个 WILL 始终配对且仅配对一次 DID。
 */
typedef enum esx_view_nav_event {
    ESX_VIEW_NAV_WILL_ENTER = 0, ///< 将进入台前（转场开始前）
    ESX_VIEW_NAV_DID_ENTER  = 1, ///< 已进入台前（转场结束后）
    ESX_VIEW_NAV_WILL_LEAVE = 2, ///< 将离开台前（被新页覆盖，或将 pop 销毁）
    ESX_VIEW_NAV_DID_LEAVE  = 3, ///< 已离开台前；pop 方向时页面随后销毁
} esx_view_nav_event;

/**
 * @brief 页面导航生命周期回调。
 * @param forward 1 = push 前进，0 = pop 返回
 * @return WILL_* 时返回非 0 取消本次导航：push 被取消时 page 未被 Navigation 接管，
 *         App 自行销毁或复用；钩子里改跳其他页面（如未登录拦截）时同样返回非 0
 *         取消原导航。DID_* 返回值忽略。
 * @warning 钩子里不允许销毁收到事件的页面自身。
 */
typedef int32_t (*esx_view_nav_func)(esx_view nav, esx_view page,
                                     esx_view_nav_event event, int32_t forward,
                                     void *user_data);

/**
 * @brief 创建视图。创建时矩形为 {0,0,0,0}——位置与尺寸一律后置：
 *        用 esx_view_set_bounds 摆放，或由父容器（Flex/ScrollView/Navigation）
 *        的布局接管。
 * @param parent =0 表示暂不挂载（之后可作为根视图）；否则作为 parent 的子视图
 *        （挂载在最上层）
 * @return 新视图句柄；失败返回 0
 */
esx_view esx_create_view(esx_view parent);

/** @brief 递归销毁子树，注销所有相关句柄。 */
void esx_destroy_view(esx_view view);

/**
 * @brief 设置根视图。
 * @param view 必须是 parent=0 创建的、尚未挂载的视图
 */
void esx_set_root_view(esx_view view);

/**
 * @brief 设置布局矩形（相对父视图左上角的局部坐标，像素）。
 *
 * 这是普通视图唯一的定位手段；Flex/ScrollView/Navigation 的子视图由容器
 * 布局接管，不要对它们手动 set_bounds（会被容器重排覆盖）。修改会触发
 * 视图的 handleBoundsChanged 钩子（Flex 借此级联重排子节点）。
 */
void esx_view_set_bounds(esx_view view, float x, float y, float w, float h);
void esx_view_set_visible(esx_view view, int32_t visible);
void esx_view_set_background(esx_view view, uint32_t rgba); ///< 0xRRGGBBAA
void esx_view_clear_background(esx_view view);

/**
 * @name 语义化回调绑定
 *
 * 普通 View 可直接绑定语义化回调。标准控件（Button/ScrollView）会在 SDK
 * 内部占用相应回调，App 只使用控件公开的 onClick/onScroll 接口。
 * @{
 */
void esx_view_set_draw_callback(esx_view view, esx_view_draw_func func, void *user_data);
void esx_view_set_click_callback(esx_view view, esx_view_click_func func, void *user_data);
void esx_view_set_pan_callback(esx_view view, esx_view_pan_func func, void *user_data);
/** @} */

/**
 * @brief 注册页面导航生命周期回调（func=NULL 清除）。
 *
 * 只有 push 进 Navigation 页面栈的视图才会收到事件；普通视图/控件注册无效。
 */
void esx_view_set_nav_callback(esx_view view, esx_view_nav_func func, void *user_data);

/**
 * @name 帧内绘制函数
 *
 * 以下两个绘制函数只在 View draw callback 内有效（操作"当前帧 canvas"），
 * 回调外调用会被忽略并告警。
 * 坐标均为相对该视图左上角的局部坐标；绘制位置 = view.actual + 局部坐标，
 * clip = 视图自身 actual 矩形与父链各视图矩形的交集（沿 parent 链逐级 intersect）。
 * @{
 */
void esx_draw_rect(esx_view view, float x, float y, float w, float h, uint32_t rgba);
void esx_draw_triangle(esx_view view,
                       float x1, float y1, float x2, float y2, float x3, float y3,
                       uint32_t c1, uint32_t c2, uint32_t c3); ///< 三色均为 0xRRGGBBAA
/** @} */

#ifdef __cplusplus
} // extern "C"

#include <memory>

namespace evk::ui {
class View;
class Canvas;
}

// ---- core 内部函数（非 ABI，供 core/平台壳使用）----

/**
 * @brief 把控件实现好的 View 节点挂进视图树并注册句柄（初始矩形 {0,0,0,0}，
 *        由调用方随后 set_bounds 或容器布局赋予）。
 * @param view 控件实现好的 View 节点（所有权移交视图树/未挂载列表）
 * @param parent 挂载父视图；=0 暂不挂载
 * @return 新句柄（0 表示失败）
 * @note 仅供 core/ui 控件以自定义 View 子类创建视图时使用。
 */
esx_view esxAdoptViewNode(std::unique_ptr<evk::ui::View> view, esx_view parent);

/**
 * @brief 当前根视图。
 * @return 根视图指针；未设置返回 nullptr
 */
evk::ui::View* esxRootView();

/**
 * @brief 句柄 → View 指针查询。
 * @note 仅供 core/ui 控件实现使用；App 仍只操作 esx_view 句柄。
 */
evk::ui::View* esxViewFromHandle(esx_view view);

/**
 * @brief 把 parent=0 创建、尚未挂载的 child 移动为 parent 的子视图（挂载在最上层）。
 * @return false 表示失败：child 已挂载、是当前根视图或句柄无效（告警）
 * @note 仅供 core/ui 控件使用。
 */
bool esxAdoptChild(esx_view parent, esx_view child);

/**
 * @brief 帧构建：重算 actual、清 canvas，按 View 顺序执行背景、自定义绘制、子树绘制。
 */
void esxBuildFrame(evk::ui::Canvas& canvas);
#endif
