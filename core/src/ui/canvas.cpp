#include "evk/ui/canvas.h"

namespace evk::ui {

namespace {

bool sameClip(const Rect& a, const Rect& b) {
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

} // namespace

void Canvas::clear() {
    vertices_.clear();
    batches_.clear();
}

void Canvas::append(const Rect& clip, std::initializer_list<UiVertex> verts) {
    if (batches_.empty() || !sameClip(batches_.back().clip, clip)) {
        batches_.push_back(Batch{clip, static_cast<uint32_t>(vertices_.size()), 0});
    }
    vertices_.insert(vertices_.end(), verts.begin(), verts.end());
    batches_.back().vertexCount += static_cast<uint32_t>(verts.size());
}

void Canvas::drawRect(const Rect& r, const Rect& clip, Color c) {
    UiVertex v0{r.x,       r.y,       c.r, c.g, c.b, c.a};
    UiVertex v1{r.x + r.w, r.y,       c.r, c.g, c.b, c.a};
    UiVertex v2{r.x + r.w, r.y + r.h, c.r, c.g, c.b, c.a};
    UiVertex v3{r.x,       r.y + r.h, c.r, c.g, c.b, c.a};
    append(clip, {v0, v1, v2, v0, v2, v3});
}

void Canvas::drawTriangle(float x1, float y1, float x2, float y2, float x3, float y3,
                          const Rect& clip, Color c1, Color c2, Color c3) {
    append(clip, {
        UiVertex{x1, y1, c1.r, c1.g, c1.b, c1.a},
        UiVertex{x2, y2, c2.r, c2.g, c2.b, c2.a},
        UiVertex{x3, y3, c3.r, c3.g, c3.b, c3.a},
    });
}

} // namespace evk::ui
