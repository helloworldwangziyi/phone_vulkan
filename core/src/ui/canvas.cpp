/**
 * @file canvas.cpp
 * @brief Canvas 立即模式顶点收集与 scissor 合批的实现。
 */
#include "evk/ui/canvas.h"

namespace evk::ui {

namespace {

/// 两裁剪矩形是否完全相等（合批判定用）。
bool sameClip(const Rect& a, const Rect& b) {
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

} // namespace

void Canvas::clear() {
    vertices_.clear();
    batches_.clear();
}

/**
 * @brief 追加顶点并按 clip 合批：连续相同 clip 的绘制合并进同一个 Batch，
 * GPU 只需设置一次 scissor 即可画完整个批次（渲染合批的关键优化）。
 *
 * clip 变化才开新 Batch——一帧内 Batch 数取决于"裁剪矩形切换次数"
 * 而非绘制次数；顶点始终顺序追加进全局顶点数组，Batch 只记录
 * [firstVertex, vertexCount) 区间，不复制数据。
 */
void Canvas::append(const Rect& clip, std::initializer_list<UiVertex> verts) {
    if (batches_.empty() || !sameClip(batches_.back().clip, clip)) {
        batches_.push_back(Batch{clip, static_cast<uint32_t>(vertices_.size()), 0});
    }
    vertices_.insert(vertices_.end(), verts.begin(), verts.end());
    batches_.back().vertexCount += static_cast<uint32_t>(verts.size());
}

/**
 * @brief 画实心矩形：矩形拆成 2 个三角形（6 顶点，顺时针/逆时针由渲染管线
 * 处理面剔除）。
 *
 * r 是屏幕像素坐标——调用方 esx_draw_* 已把局部坐标平移成屏幕坐标，
 * Canvas 层不做任何坐标变换。
 */
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
