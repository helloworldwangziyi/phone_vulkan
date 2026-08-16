#include "evk/ui/render_view.h"

#include <algorithm>

#include "evk/log.h"
#include "evk/frame_scheduler.h"
#include "evk/ui/paint_canvas.h"
#include "evk/ui/pointer_input.h"

namespace {

evk::ui::View* g_rootView = nullptr;
bool g_buildingFrame = false;

class FrameBuildScope {
public:
    FrameBuildScope() { g_buildingFrame = true; }
    ~FrameBuildScope() { g_buildingFrame = false; }
};

} // namespace

namespace evk::ui {

bool Rect::contains(float px, float py) const {
    return px >= x && py >= y && px < x + w && py < y + h;
}

Rect Rect::intersect(const Rect& a, const Rect& b) {
    const float left = std::max(a.x, b.x);
    const float top = std::max(a.y, b.y);
    const float right = std::min(a.x + a.w, b.x + b.w);
    const float bottom = std::min(a.y + a.h, b.y + b.h);
    return {left, top, std::max(0.0f, right - left), std::max(0.0f, bottom - top)};
}

Color Color::rgba(uint32_t value) {
    constexpr float scale = 1.0f / 255.0f;
    return {
        static_cast<float>((value >> 24) & 0xFF) * scale,
        static_cast<float>((value >> 16) & 0xFF) * scale,
        static_cast<float>((value >> 8) & 0xFF) * scale,
        static_cast<float>(value & 0xFF) * scale,
    };
}

PaintContext::PaintContext(Canvas& canvas, const Rect& bounds, const Rect& clip)
    : canvas_(canvas), bounds_(bounds), clip_(clip) {}

void PaintContext::drawRect(const Rect& rect, uint32_t rgba) {
    canvas_.drawRect(
        {bounds_.x + rect.x, bounds_.y + rect.y, rect.w, rect.h},
        clip_, Color::rgba(rgba));
}

void PaintContext::drawTriangle(float x1, float y1, float x2, float y2,
                                float x3, float y3,
                                uint32_t c1, uint32_t c2, uint32_t c3) {
    canvas_.drawTriangle(
        bounds_.x + x1, bounds_.y + y1,
        bounds_.x + x2, bounds_.y + y2,
        bounds_.x + x3, bounds_.y + y3,
        clip_, Color::rgba(c1), Color::rgba(c2), Color::rgba(c3));
}

ViewRef::ViewRef(View* view)
    : view_(view), lifetime_(view ? view->lifetime_ : std::weak_ptr<uint8_t>{}) {}

View* ViewRef::get() const {
    return lifetime_.expired() ? nullptr : view_;
}

View::View() : lifetime_(std::make_shared<uint8_t>(0)) {}

View::~View() {
    discardPointerForView(this);
    if (g_rootView == this) {
        g_rootView = nullptr;
    }
}

void View::handlePointer(const PointerEvent&) {}

void View::handlePan(const PanEvent& event) {
    if (onPan) {
        onPan(event);
    }
}

void View::setBounds(float x, float y, float width, float height) {
    if (g_buildingFrame) {
        EVK_LOGW("View::setBounds cannot mutate the tree during paint");
        return;
    }
    rect = {x, y, std::max(0.0f, width), std::max(0.0f, height)};
    handleBoundsChanged();
    requestRender();
}

void View::setVisible(bool value) {
    if (visible == value) {
        return;
    }
    if (!value) {
        cancelPointerForView(this);
    }
    visible = value;
    requestRender();
}

void View::setBackground(uint32_t rgba) {
    hasBackground = true;
    background = Color::rgba(rgba);
    requestRender();
}

void View::clearBackground() {
    hasBackground = false;
    requestRender();
}

View* View::addChild(std::unique_ptr<View> child) {
    if (!child || child->parent || g_buildingFrame) {
        return nullptr;
    }
    View* raw = child.get();
    raw->parent = this;
    children.push_back(std::move(child));
    requestRender();
    return raw;
}

std::unique_ptr<View> View::removeChild(View* child) {
    if (!child || g_buildingFrame) {
        return nullptr;
    }
    const auto it = std::find_if(children.begin(), children.end(),
                                 [child](const std::unique_ptr<View>& item) {
                                     return item.get() == child;
                                 });
    if (it == children.end()) {
        return nullptr;
    }
    const size_t index = static_cast<size_t>(it - children.begin());
    cancelPointerForView(child);
    std::unique_ptr<View> removed = std::move(*it);
    children.erase(it);
    removed->parent = nullptr;
    handleChildRemoved(index);
    requestRender();
    return removed;
}

std::unique_ptr<View> View::replaceChild(
    View* oldChild, std::unique_ptr<View> replacement) {
    if (!oldChild || !replacement || replacement->parent || g_buildingFrame) {
        return nullptr;
    }
    const auto it = std::find_if(children.begin(), children.end(),
                                 [oldChild](const std::unique_ptr<View>& item) {
                                     return item.get() == oldChild;
                                 });
    if (it == children.end()) {
        return nullptr;
    }
    cancelPointerForView(oldChild);
    std::unique_ptr<View> removed = std::move(*it);
    removed->parent = nullptr;
    replacement->parent = this;
    *it = std::move(replacement);
    requestRender();
    return removed;
}

void View::updateActuals() {
    actualX = parent ? parent->actualX + rect.x : rect.x;
    actualY = parent ? parent->actualY + rect.y : rect.y;
    for (auto& child : children) {
        child->updateActuals();
    }
}

View* View::hitTest(float px, float py) {
    if (!visible || !actualRect().contains(px, py)) {
        return nullptr;
    }
    for (auto it = children.rbegin(); it != children.rend(); ++it) {
        if (View* hit = (*it)->hitTest(px, py)) {
            return hit;
        }
    }
    return this;
}

bool View::containsVisiblePoint(float px, float py) const {
    for (const View* view = this; view; view = view->parent) {
        if (!view->visible || !view->actualRect().contains(px, py)) {
            return false;
        }
    }
    return true;
}

bool View::isDescendantOf(const View* ancestor) const {
    for (const View* view = this; view; view = view->parent) {
        if (view == ancestor) {
            return true;
        }
    }
    return false;
}

void View::paint(Canvas& canvas, const Rect& parentClip) {
    if (!visible) {
        return;
    }
    const Rect bounds = actualRect();
    const Rect clip = Rect::intersect(parentClip, bounds);
    if (clip.w <= 0.0f || clip.h <= 0.0f) {
        return;
    }
    if (hasBackground) {
        canvas.drawRect(bounds, clip, background);
    }
    if (painter) {
        PaintContext context(canvas, bounds, clip);
        painter(context);
    }
    for (auto& child : children) {
        child->paint(canvas, clip);
    }
}

View* rootView() {
    return g_rootView;
}

void setRootView(View* view) {
    if (g_buildingFrame) {
        EVK_LOGW("setRootView cannot mutate the tree during paint");
        return;
    }
    if (view && view->parent) {
        EVK_LOGW("setRootView requires a detached view");
        return;
    }
    g_rootView = view;
    requestRender();
}

bool isBuildingFrame() {
    return g_buildingFrame;
}

void buildFrame(Canvas& canvas) {
    canvas.clear();
    if (!g_rootView) {
        return;
    }
    g_rootView->updateActuals();
    const Rect clip = g_rootView->actualRect();
    FrameBuildScope scope;
    g_rootView->paint(canvas, clip);
}

} // namespace evk::ui
