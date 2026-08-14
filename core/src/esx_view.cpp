#include "evk/esx_view.h"

#include <memory>
#include <unordered_map>
#include <vector>

#include "evk/log.h"
#include "evk/render_loop.h"
#include "evk/ui/canvas.h"
#include "evk/ui/input.h"

namespace {

// 句柄注册表：句柄从 1 自增，0 无效。
std::unordered_map<uint32_t, evk::ui::View*> g_handles;
uint32_t g_nextHandle = 1;

// parent=0 创建、尚未挂载进树的视图，所有权由这里持有。
std::vector<std::unique_ptr<evk::ui::View>> g_unattached;

// 当前根视图（不持有所有权，所有权在 g_unattached 或已销毁时置空）。
evk::ui::View* g_root = nullptr;

// 当前帧 canvas：esxBuildFrame 期间有效，其余时间为 nullptr。
evk::ui::Canvas* g_canvas = nullptr;
bool g_buildingFrame = false;

class FrameBuildScope {
public:
    explicit FrameBuildScope(evk::ui::Canvas& canvas)
        : previousCanvas_(g_canvas), previousBuildingFrame_(g_buildingFrame) {
        g_canvas = &canvas;
        g_buildingFrame = true;
    }

    ~FrameBuildScope() {
        g_buildingFrame = previousBuildingFrame_;
        g_canvas = previousCanvas_;
    }

    FrameBuildScope(const FrameBuildScope&) = delete;
    FrameBuildScope& operator=(const FrameBuildScope&) = delete;

private:
    evk::ui::Canvas* previousCanvas_;
    bool previousBuildingFrame_;
};

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

// clip = 视图自身 actual 矩形与父链各视图矩形的交集。
evk::ui::Rect clipFor(const evk::ui::View* view) {
    evk::ui::Rect clip = view->actualRect();
    for (const evk::ui::View* p = view->parent; p; p = p->parent) {
        clip = evk::ui::Rect::intersect(clip, p->actualRect());
    }
    return clip;
}

// 递归注销整棵子树的句柄（不销毁 View 对象本身）。
void unregisterSubtree(evk::ui::View* view) {
    for (auto& child : view->children) {
        unregisterSubtree(child.get());
    }
    g_handles.erase(view->handle);
}

// 每个节点依次绘制背景、自定义内容、子节点，保证父内容不会盖住子节点。
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

esx_view esx_create_view(float x, float y, float w, float h, esx_view parent) {
    return esxAdoptViewNode(std::make_unique<evk::ui::View>(), x, y, w, h, parent);
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

// 把控件实现好的 View 节点（可为 Button/ScrollView 等子类）挂进视图树：
// 设置布局矩形、挂到 parent（或暂存未挂载列表）、注册句柄。
esx_view esxAdoptViewNode(std::unique_ptr<evk::ui::View> view,
                          float x, float y, float w, float h, esx_view parent) {
    if (g_buildingFrame) {
        EVK_LOGW("esxAdoptViewNode: cannot change the View tree during draw");
        return 0;
    }
    if (!evk::engineReady()) {
        // 告警不拦截：应在 EngineReady 事件后创建视图（见 event.h）。
        EVK_LOGW("esxAdoptViewNode: creating view before engine is ready");
    }
    view->rect = {x, y, w, h};
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

// Navigation 等容器控件用它接管 App 以 parent=0 创建的视图：
// 所有权从 g_unattached 列表移动为 parent 的子视图（unique_ptr 转移）。
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
