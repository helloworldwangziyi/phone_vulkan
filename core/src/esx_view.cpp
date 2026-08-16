/**
 * @file esx_view.cpp
 * @brief esx 视图 ABI 实现：句柄注册表、视图树所有权管理与帧构建（接口契约见 esx_view.h）。
 */
#include "evk/esx_view.h"

#include <memory>
#include <unordered_map>
#include <vector>

#include "evk/log.h"
#include "evk/render_loop.h"
#include "evk/ui/canvas.h"
#include "evk/ui/input.h"

namespace {

/// 句柄注册表：uint32 句柄 → View* 的一一映射（句柄从 1 自增，0 保留为无效）。
/// App 只持有句柄（可拷贝、可跨 C ABI），真正的对象所有权在视图树里；
/// 注册表本身不拥有 View，因此查询得到的裸指针可能因树变更而失效，
/// 每次使用前都应重新解析（esxViewFromHandle）。
std::unordered_map<uint32_t, evk::ui::View*> g_handles;
uint32_t g_nextHandle = 1;

/// "孤儿院"：parent=0 创建、尚未挂载进树的视图，所有权由这里持有。
/// 不变式：任何时刻每个视图恰好被两处之一拥有——某父节点的 children 或本列表。
/// 视图被领养（esxAdoptChild）、成为根（esx_set_root_view，所有权不动）
/// 或销毁时从这里摘除；列表持有全部元素保证"创建后从未挂载"的视图也不泄漏。
std::vector<std::unique_ptr<evk::ui::View>> g_unattached;

/// 当前根视图：绘制与命中测试的起点。不持有所有权（所有权在 g_unattached），
/// 所以根视图被销毁时只需置空本指针，对象由列表/树释放。
evk::ui::View* g_root = nullptr;

/// 当前帧 canvas：esxBuildFrame（FrameBuildScope）期间有效，其余时间为 nullptr；
/// esx_draw_* 据此判断"是否在 draw 回调内"并决定写哪个顶点流。
evk::ui::Canvas* g_canvas = nullptr;
/// 帧构建进行中标志：视图树正被遍历，期间禁止增删节点（防迭代器悬空）。
bool g_buildingFrame = false;

/**
 * @brief 帧构建期环境守卫（RAII scope guard）：管理 g_canvas 与 g_buildingFrame
 * 两个全局状态，作用域 = esxBuildFrame 调用栈的生命周期。
 *
 * 为什么必须是全局：draw 回调走 C ABI（esx_draw_* 签名里没有 canvas 参数），
 * 回调深处要画东西只能读"当前帧 canvas"这个环境状态；而绘制期间视图树
 * 正被遍历，任何增删节点都会让迭代器悬空，所以还需要 g_buildingFrame
 * 让改树 API（destroy/adopt/set_root）在入口拒绝执行。
 *
 * 设计要点：
 * - 构造/析构成对，任何退出路径（含异常）都会还原，不会把标志卡在 true；
 * - 保存并恢复"旧值"而非硬设 false/true，支持潜在的嵌套帧构建；
 * - 禁止拷贝：守卫必须与作用域一对一，两份守卫会重复还原同一组全局。
 */
class FrameBuildScope {
public:
    /// 进入帧构建：接管两个全局，旧值留待析构还原。
    explicit FrameBuildScope(evk::ui::Canvas& canvas)
        : previousCanvas_(g_canvas), previousBuildingFrame_(g_buildingFrame) {
        g_canvas = &canvas;
        g_buildingFrame = true;
    }

    /// 离开帧构建：先还原 building 标志再还原 canvas（与构造顺序相反）。
    ~FrameBuildScope() {
        g_buildingFrame = previousBuildingFrame_;
        g_canvas = previousCanvas_;
    }

    FrameBuildScope(const FrameBuildScope&) = delete;
    FrameBuildScope& operator=(const FrameBuildScope&) = delete;

private:
    evk::ui::Canvas* previousCanvas_; ///< 外层 canvas（嵌套帧构建时还原用）
    bool previousBuildingFrame_; ///< 外层 building 标志
};

/// 句柄 → 裸指针（仅供内部使用，不拥有所有权）。无效/未知句柄告警并返回
/// nullptr，caller 用于告警文案定位。注意返回的裸指针只保证"此刻有效"，
/// 若调用链上可能触发树变更（如调用回调），使用前应重新解析。
evk::ui::View* lookupView(esx_view view, const char* caller) {
    if (view == 0) {
        EVK_LOGW("{}: invalid view handle 0", caller);
        return nullptr;
    }
    auto it = g_handles.find(view);
    if (it == g_handles.end()) {
        EVK_LOGW("{}: unknown view handle {}", caller, view);
        return nullptr;
    }
    return it->second;
}

/// 裁剪矩形 = 视图自身 actual 矩形与父链各级 actual 矩形的逐级交集。
/// 意义：视图可以"溢出"父容器（页面滑出导航容器、滚动内容超出 viewport），
/// 绘制必须只落在实际可见区域——GPU 按 clip 做 scissor 裁剪，超出的部分画了也不显示。
/// clip 随节点走而非随绘制顺序走：每个节点的绘制都带自己的 clip
/// （Canvas 按 clip 分批），这正是"页面滑出容器自动被裁剪"的实现。
evk::ui::Rect clipFor(const evk::ui::View* view) {
    evk::ui::Rect clip = view->actualRect();
    for (const evk::ui::View* p = view->parent; p; p = p->parent) {
        clip = evk::ui::Rect::intersect(clip, p->actualRect());
    }
    return clip;
}

/// 递归注销整棵子树的句柄（不销毁 View 对象本身——对象由 unique_ptr 树销毁）。
/// 句柄注销与对象销毁分离：销毁流程先摘除节点（触发 handleChildRemoved），
/// 再释放对象；先注销句柄保证销毁期间的任何回调都查不到旧句柄。
void unregisterSubtree(evk::ui::View* view) {
    for (auto& child : view->children) {
        unregisterSubtree(child.get());
    }
    g_handles.erase(view->handle);
}

/// 递归构建一帧的绘制命令：深度优先、父先子后
/// （父背景 → 父自定义绘制 → 子节点递归）。
/// 顺序即层叠关系：children 越靠后越晚画 = 越在上层
/// （Navigation 的 bar 后于 container 创建，所以永远盖住页面）；
/// 隐藏节点在入口整棵子树跳过（visible 过滤）。
/// 注意：这里不产出像素，只往 Canvas 追加顶点流；GPU 绘制在帧构建之后。
void drawViewTree(evk::ui::View* view, evk::ui::Canvas& canvas) {
    if (!view->visible) {
        return;
    }
    if (view->hasBackground) {
        canvas.drawRect(view->actualRect(), clipFor(view), view->background);
    }
    if (view->drawFunc) {
        view->drawFunc(view->handle, view->drawUserData);
    }
    for (auto& child : view->children) {
        drawViewTree(child.get(), canvas);
    }
}

} // namespace

extern "C" {

esx_view esx_create_view(esx_view parent) {
    return esxAdoptViewNode(std::make_unique<evk::ui::View>(), parent);
}

void esx_destroy_view(esx_view view) {
    if (g_buildingFrame) {
        EVK_LOGW("esx_destroy_view: cannot change the View tree during draw");
        return;
    }
    evk::ui::View* v = lookupView(view, "esx_destroy_view");
    if (!v) {
        return;
    }

    evk::ui::discardPointerForView(view);
    unregisterSubtree(v);
    if (g_root == v) {
        g_root = nullptr;
    }
    // 销毁对象本身：从父视图或未挂载列表里摘除（unique_ptr 释放整棵子树）。
    if (v->parent) {
        evk::ui::View* parent = v->parent;
        auto& siblings = parent->children;
        for (auto it = siblings.begin(); it != siblings.end(); ++it) {
            if (it->get() == v) {
                const size_t removedIndex = static_cast<size_t>(it - siblings.begin());
                siblings.erase(it);
                // 钩子在摘除之后触发：父视图看到的是移除后的 children。
                parent->handleChildRemoved(removedIndex);
                break;
            }
        }
    } else {
        for (auto it = g_unattached.begin(); it != g_unattached.end(); ++it) {
            if (it->get() == v) {
                g_unattached.erase(it);
                break;
            }
        }
    }
    evk::requestRender();
}

void esx_set_root_view(esx_view view) {
    if (g_buildingFrame) {
        EVK_LOGW("esx_set_root_view: cannot change the View tree during draw");
        return;
    }
    evk::ui::View* v = lookupView(view, "esx_set_root_view");
    if (!v) {
        return;
    }
    if (v->parent != nullptr) {
        EVK_LOGW("esx_set_root_view: view {} is already attached", view);
        return;
    }
    if (g_root != v) {
        evk::ui::cancelAllPointerEvents();
        v = esxViewFromHandle(view);
        if (!v) {
            return;
        }
        g_root = v;
    }
    evk::requestRender();
}

/**
 * @brief 布局级联的唯一入口：写局部 rect + 触发 handleBoundsChanged 钩子。
 *
 * 钩子是布局传播机制——Flex 会重排子节点（写孙节点 rect 并再触发其钩子），
 * Navigation 会重排容器/导航栏/页面并取消转场，ScrollView 会 clamp offset。
 * 因此一次根视图 set_bounds（如 SurfaceChanged）会自动多米诺式重排整棵树，
 * App 永远不需要手写 layout 函数；每次变更都 requestRender 等下一帧重绘。
 */
void esx_view_set_bounds(esx_view view, float x, float y, float w, float h) {
    evk::ui::View* v = lookupView(view, "esx_view_set_bounds");
    if (!v) {
        return;
    }
    v->rect = {x, y, w, h};
    v->handleBoundsChanged();
    evk::requestRender();
}

void esx_view_set_visible(esx_view view, int32_t visible) {
    evk::ui::View* v = lookupView(view, "esx_view_set_visible");
    if (!v) {
        return;
    }
    const bool nextVisible = visible != 0;
    if (!nextVisible && v->visible) {
        evk::ui::cancelPointerForView(view);
        v = esxViewFromHandle(view);
        if (!v) {
            return;
        }
    }
    v->visible = nextVisible;
    evk::requestRender();
}

void esx_view_set_background(esx_view view, uint32_t rgba) {
    evk::ui::View* v = lookupView(view, "esx_view_set_background");
    if (!v) {
        return;
    }
    v->background = evk::ui::Color::rgba(rgba);
    v->hasBackground = true;
    evk::requestRender();
}

void esx_view_clear_background(esx_view view) {
    evk::ui::View* v = lookupView(view, "esx_view_clear_background");
    if (!v) {
        return;
    }
    v->hasBackground = false;
    evk::requestRender();
}

void esx_view_set_draw_callback(esx_view view, esx_view_draw_func func, void* user_data) {
    evk::ui::View* v = lookupView(view, "esx_view_set_draw_callback");
    if (!v) {
        return;
    }
    v->drawFunc = func;
    v->drawUserData = user_data;
    evk::requestRender();
}

void esx_view_set_click_callback(esx_view view, esx_view_click_func func, void* user_data) {
    evk::ui::View* v = lookupView(view, "esx_view_set_click_callback");
    if (!v) {
        return;
    }
    v->clickFunc = func;
    v->clickUserData = user_data;
}

void esx_view_set_pan_callback(esx_view view, esx_view_pan_func func, void* user_data) {
    evk::ui::View* v = lookupView(view, "esx_view_set_pan_callback");
    if (!v) {
        return;
    }
    v->panFunc = func;
    v->panUserData = user_data;
}

void esx_view_set_nav_callback(esx_view view, esx_view_nav_func func, void* user_data) {
    evk::ui::View* v = lookupView(view, "esx_view_set_nav_callback");
    if (!v) {
        return;
    }
    v->navFunc = func;
    v->navUserData = user_data;
}

/**
 * @brief draw callback 专用绘制 API（esxBuildFrame 期间有效，其余时间忽略并告警）。
 *
 * 关键坐标约定：x/y/w/h 是相对 view 左上角的局部坐标，这里加 actualX/Y
 * 平移成屏幕坐标——所以 App 的 draw 回调只按自身比例写局部坐标，
 * 视图被 Flex 排到任何位置、转场滑到任何进度、滚动到任何 offset，代码都不用改。
 * clip 取该视图沿父链的交集：转场滑出容器、滚动超出 viewport 的部分自动被裁。
 */
void esx_draw_rect(esx_view view, float x, float y, float w, float h, uint32_t rgba) {
    if (!g_canvas) {
        EVK_LOGW("esx_draw_rect: called outside Draw callback, ignored");
        return;
    }
    evk::ui::View* v = lookupView(view, "esx_draw_rect");
    if (!v) {
        return;
    }
    evk::ui::Rect r{v->actualX + x, v->actualY + y, w, h};
    g_canvas->drawRect(r, clipFor(v), evk::ui::Color::rgba(rgba));
}

/**
 * @brief 画三色渐变三角形（与 esx_draw_rect 相同的局部坐标约定）。
 *
 * 三个顶点各带一个颜色，GPU 在三角形内部插值出渐变——
 * sample 的 drawPanelGradient/drawHeroGradient 即靠三个顶点色生成渐变面板，
 * 返回箭头 drawBackArrow 则用同色三顶点画实心三角。
 */
void esx_draw_triangle(esx_view view,
                       float x1, float y1, float x2, float y2, float x3, float y3,
                       uint32_t c1, uint32_t c2, uint32_t c3) {
    if (!g_canvas) {
        EVK_LOGW("esx_draw_triangle: called outside Draw callback, ignored");
        return;
    }
    evk::ui::View* v = lookupView(view, "esx_draw_triangle");
    if (!v) {
        return;
    }
    float ox = v->actualX;
    float oy = v->actualY;
    g_canvas->drawTriangle(ox + x1, oy + y1, ox + x2, oy + y2, ox + x3, oy + y3,
                           clipFor(v),
                           evk::ui::Color::rgba(c1), evk::ui::Color::rgba(c2),
                           evk::ui::Color::rgba(c3));
}

} // extern "C"

evk::ui::View* esxRootView() {
    return g_root;
}

evk::ui::View* esxViewFromHandle(esx_view view) {
    auto it = g_handles.find(view);
    return it == g_handles.end() ? nullptr : it->second;
}

/**
 * @brief 把控件实现好的 View 节点（可为 Button/ScrollView 等子类）挂进视图树：
 * 挂到 parent（或暂存未挂载列表）、注册句柄；初始矩形 {0,0,0,0}，
 * 由调用方随后 set_bounds 或容器布局赋予。
 *
 * 句柄从 1 自增（0 无效），登记进 g_handles 后视图才可被 esx_view_* 系列操作；
 * parent=0 时所有权留在 g_unattached，等 esxAdoptChild / esx_set_root_view 接管。
 * 每次创建都 requestRender——视图树一变，下一帧必须重绘。
 */
esx_view esxAdoptViewNode(std::unique_ptr<evk::ui::View> view, esx_view parent) {
    if (g_buildingFrame) {
        EVK_LOGW("esxAdoptViewNode: cannot change the View tree during draw");
        return 0;
    }
    if (!evk::engineReady()) {
        // 告警不拦截：应在 EngineReady 事件后创建视图（见 event.h）。
        EVK_LOGW("esxAdoptViewNode: creating view before engine is ready");
    }
    evk::ui::View* raw = view.get();

    if (parent != 0) {
        evk::ui::View* p = lookupView(parent, "esxAdoptViewNode");
        if (!p) {
            return 0;
        }
        p->addChild(std::move(view));
    } else {
        g_unattached.push_back(std::move(view));
    }

    const uint32_t handle = g_nextHandle++;
    raw->handle = handle;
    g_handles[handle] = raw;
    evk::requestRender();
    return handle;
}

/**
 * @brief Navigation 等容器控件用它接管 App 以 parent=0 创建的视图：
 * 所有权从 g_unattached 列表移动为 parent 的子视图（unique_ptr 转移）。
 */
bool esxAdoptChild(esx_view parent, esx_view child) {
    if (g_buildingFrame) {
        EVK_LOGW("esxAdoptChild: cannot change the View tree during draw");
        return false;
    }
    evk::ui::View* p = lookupView(parent, "esxAdoptChild");
    evk::ui::View* c = lookupView(child, "esxAdoptChild");
    if (!p || !c) {
        return false;
    }
    if (c->parent != nullptr || c == g_root) {
        EVK_LOGW("esxAdoptChild: view {} is already attached", child);
        return false;
    }
    // 不能把祖先挂进自己的子树（会成环）。
    for (evk::ui::View* current = p; current; current = current->parent) {
        if (current == c) {
            EVK_LOGW("esxAdoptChild: view {} is an ancestor of {}", child, parent);
            return false;
        }
    }
    for (auto it = g_unattached.begin(); it != g_unattached.end(); ++it) {
        if (it->get() == c) {
            std::unique_ptr<evk::ui::View> owned = std::move(*it);
            g_unattached.erase(it);
            p->addChild(std::move(owned));
            evk::requestRender();
            return true;
        }
    }
    EVK_LOGW("esxAdoptChild: view {} is not unattached", child);
    return false;
}

/**
 * @brief 帧构建（每帧 VSync 到达后调用一次）。
 *
 * 1. updateActuals —— 用最新的 rect 重算整棵树屏幕坐标（布局可能刚变过）；
 * 2. canvas.clear —— 上一帧顶点流全部丢弃，每帧从零重建（绘制是立即模式）；
 * 3. FrameBuildScope —— 置 g_canvas/g_buildingFrame：期间 esx_draw_* 写入
 *    当前帧 canvas，且禁止改视图树（esxAdoptChild 等会告警拒绝）；
 * 4. drawViewTree —— 前序遍历，把背景/自定义绘制/子节点转成顶点+裁剪批次。
 *
 * 产出（Canvas 顶点流）随后交给渲染器走 Vulkan；帧构建本身不发射绘制。
 */
void esxBuildFrame(evk::ui::Canvas& canvas) {
    if (!g_root) {
        canvas.clear();
        return;
    }
    g_root->updateActuals();
    canvas.clear();

    const FrameBuildScope frameScope(canvas);
    drawViewTree(g_root, canvas);
}
