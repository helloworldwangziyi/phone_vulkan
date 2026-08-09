#include "evk/esx_view.h"

#include <cmath>
#include <memory>
#include <unordered_map>
#include <vector>

#include "evk/event.h"
#include "evk/log.h"
#include "evk/ui/canvas.h"

namespace {

// 句柄注册表：uint32_t 句柄 ↔ View* 双向映射，句柄从 1 自增，0 无效。
std::unordered_map<uint32_t, evk::ui::View*> g_handles;
std::unordered_map<evk::ui::View*, uint32_t> g_views;
std::unordered_map<uint32_t, esx_view_type> g_types;
uint32_t g_nextHandle = 1;

// parent=0 创建、尚未挂载进树的视图，所有权由这里持有。
std::vector<std::unique_ptr<evk::ui::View>> g_unattached;

// 当前根视图（不持有所有权，所有权在 g_unattached 或已销毁时置空）。
evk::ui::View* g_root = nullptr;

// 当前帧 canvas：esxBuildFrame 期间有效，其余时间为 nullptr。
evk::ui::Canvas* g_canvas = nullptr;

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
    auto it = g_views.find(view);
    if (it != g_views.end()) {
        g_handles.erase(it->second);
        g_types.erase(it->second);
        g_views.erase(it);
    }
}

// 前序遍历：对每个可见且有背景的视图画背景。不可见视图整棵子树跳过。
void drawBackgrounds(evk::ui::View* view, evk::ui::Canvas& canvas) {
    if (!view->visible) {
        return;
    }
    if (view->hasBackground) {
        canvas.drawRect(view->actualRect(), clipFor(view), view->background);
    }
    for (auto& child : view->children) {
        drawBackgrounds(child.get(), canvas);
    }
}

// 前序遍历：对每个可见视图发 Draw 事件（含无背景的 GROUP）。
void dispatchDraws(evk::ui::View* view) {
    if (!view->visible) {
        return;
    }
    auto it = g_views.find(view);
    if (it != g_views.end()) {
        evk::DrawData data{it->second};
        evk::dispatchEvent(evk::EventId::Draw, &data);
    }
    for (auto& child : view->children) {
        dispatchDraws(child.get());
    }
}

// 触摸状态机。
struct TouchState {
    uint32_t target = 0;   // DOWN 时 hitTest 命中的句柄
    bool cancelled = false;
    float lastX = 0, lastY = 0;
    float moved = 0.0f;    // 累计位移（曼哈顿距离）
};
TouchState g_touch;
constexpr float kTouchSlop = 12.0f;

void resetTouch() {
    g_touch = TouchState{};
}

} // namespace

extern "C" {

esx_view esx_create_view(esx_view_type type, float x, float y, float w, float h, esx_view parent) {
    auto view = std::make_unique<evk::ui::View>();
    view->rect = {x, y, w, h};
    evk::ui::View* raw = view.get();

    if (parent != 0) {
        evk::ui::View* p = lookupView(parent, "esx_create_view");
        if (!p) {
            return 0;
        }
        p->addChild(std::move(view));
    } else {
        g_unattached.push_back(std::move(view));
    }

    uint32_t handle = g_nextHandle++;
    g_handles[handle] = raw;
    g_views[raw] = handle;
    g_types[handle] = type;
    return handle;
}

void esx_destroy_view(esx_view view) {
    evk::ui::View* v = lookupView(view, "esx_destroy_view");
    if (!v) {
        return;
    }

    unregisterSubtree(v);
    if (g_root == v) {
        g_root = nullptr;
    }
    if (g_touch.target == view) {
        resetTouch();
    }

    // 销毁对象本身：从父视图或未挂载列表里摘除（unique_ptr 释放整棵子树）。
    if (v->parent) {
        auto& siblings = v->parent->children;
        for (auto it = siblings.begin(); it != siblings.end(); ++it) {
            if (it->get() == v) {
                siblings.erase(it);
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
}

void esx_set_root_view(esx_view view) {
    evk::ui::View* v = lookupView(view, "esx_set_root_view");
    if (!v) {
        return;
    }
    if (v->parent != nullptr) {
        EVK_LOGW("esx_set_root_view: view {} is already attached", view);
        return;
    }
    g_root = v;
}

void esx_view_set_bounds(esx_view view, float x, float y, float w, float h) {
    evk::ui::View* v = lookupView(view, "esx_view_set_bounds");
    if (!v) {
        return;
    }
    v->rect = {x, y, w, h};
}

void esx_view_set_visible(esx_view view, int32_t visible) {
    evk::ui::View* v = lookupView(view, "esx_view_set_visible");
    if (!v) {
        return;
    }
    v->visible = (visible != 0);
}

void esx_view_set_background(esx_view view, uint32_t rgba) {
    evk::ui::View* v = lookupView(view, "esx_view_set_background");
    if (!v) {
        return;
    }
    v->background = evk::ui::Color::rgba(rgba);
    v->hasBackground = true;
}

void esx_view_clear_background(esx_view view) {
    evk::ui::View* v = lookupView(view, "esx_view_clear_background");
    if (!v) {
        return;
    }
    v->hasBackground = false;
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

void esxBuildFrame(evk::ui::Canvas& canvas) {
    if (!g_root) {
        canvas.clear();
        return;
    }
    g_root->updateActuals();
    canvas.clear();

    evk::ui::Canvas* prev = g_canvas;
    g_canvas = &canvas;
    drawBackgrounds(g_root, canvas);
    dispatchDraws(g_root);
    g_canvas = prev;
}

void esxDispatchTouch(int32_t action, float x, float y) {
    switch (action) {
        case 0: { // DOWN
            resetTouch();
            if (!g_root) {
                return;
            }
            g_root->updateActuals();
            evk::ui::View* hit = g_root->hitTest(x, y);
            if (!hit) {
                return;
            }
            auto it = g_views.find(hit);
            if (it == g_views.end()) {
                return;
            }
            g_touch.target = it->second;
            g_touch.lastX = x;
            g_touch.lastY = y;
            break;
        }
        case 2: { // MOVE
            if (g_touch.target == 0 || g_touch.cancelled) {
                return;
            }
            g_touch.moved += std::fabs(x - g_touch.lastX) + std::fabs(y - g_touch.lastY);
            g_touch.lastX = x;
            g_touch.lastY = y;
            if (g_touch.moved > kTouchSlop) {
                g_touch.cancelled = true;
            }
            break;
        }
        case 1: { // UP
            uint32_t target = g_touch.target;
            bool cancelled = g_touch.cancelled;
            resetTouch();
            if (target == 0 || cancelled) {
                return;
            }
            auto it = g_handles.find(target);
            if (it == g_handles.end()) {
                // 目标视图在按下期间已被销毁。
                return;
            }
            evk::ui::View* v = it->second;
            evk::UiClickData data{target, x - v->actualX, y - v->actualY};
            evk::dispatchEvent(evk::EventId::UiClick, &data);
            break;
        }
        case 3: { // CANCEL
            resetTouch();
            break;
        }
        default:
            break;
    }
}
