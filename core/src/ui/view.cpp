#include "evk/ui/view.h"

#include <algorithm>

namespace evk::ui {

bool Rect::contains(float px, float py) const {
    return px >= x && px < x + w && py >= y && py < y + h;
}

Rect Rect::intersect(const Rect& a, const Rect& b) {
    float x1 = std::max(a.x, b.x);
    float y1 = std::max(a.y, b.y);
    float x2 = std::min(a.x + a.w, b.x + b.w);
    float y2 = std::min(a.y + a.h, b.y + b.h);
    if (x2 <= x1 || y2 <= y1) {
        return {x1, y1, 0, 0};
    }
    return {x1, y1, x2 - x1, y2 - y1};
}

Color Color::rgba(uint32_t v) {
    Color c;
    c.r = static_cast<float>((v >> 24) & 0xFF) / 255.0f;
    c.g = static_cast<float>((v >> 16) & 0xFF) / 255.0f;
    c.b = static_cast<float>((v >> 8) & 0xFF) / 255.0f;
    c.a = static_cast<float>(v & 0xFF) / 255.0f;
    return c;
}

View* View::addChild(std::unique_ptr<View> child) {
    child->parent = this;
    children.push_back(std::move(child));
    return children.back().get();
}

void View::updateActuals() {
    if (parent) {
        actualX = parent->actualX + rect.x;
        actualY = parent->actualY + rect.y;
    } else {
        actualX = rect.x;
        actualY = rect.y;
    }
    for (auto& child : children) {
        child->updateActuals();
    }
}

View* View::hitTest(float px, float py) {
    if (!visible || !actualRect().contains(px, py)) {
        return nullptr;
    }
    // 最后添加的 child 在最上层，优先命中。
    for (auto it = children.rbegin(); it != children.rend(); ++it) {
        if (View* hit = (*it)->hitTest(px, py)) {
            return hit;
        }
    }
    return this;
}

} // namespace evk::ui
