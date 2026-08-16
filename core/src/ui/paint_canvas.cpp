/**
 * @file paint_canvas.cpp
 * @brief Canvas 立即模式顶点收集与 scissor/纹理合批的实现。
 */
#include "evk/ui/paint_canvas.h"

#include "evk/ui/font_engine.h"

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
 * @brief 追加顶点并按 (clip, 纹理) 合批：连续且 clip、纹理都相同的绘制
 * 合并进同一个 Batch，GPU 只需设一次 scissor、绑一次纹理即可画完整个批次。
 *
 * clip 或纹理任一变化才开新 Batch；顶点始终顺序追加进全局顶点数组，
 * Batch 只记录 [firstVertex, vertexCount) 区间，不复制数据。
 */
void Canvas::append(const Rect& clip, uint32_t textureId,
                    std::initializer_list<UiVertex> verts) {
    if (batches_.empty() || !sameClip(batches_.back().clip, clip) ||
        batches_.back().textureId != textureId) {
        batches_.push_back(Batch{clip, textureId,
                                 static_cast<uint32_t>(vertices_.size()), 0});
    }
    vertices_.insert(vertices_.end(), verts.begin(), verts.end());
    batches_.back().vertexCount += static_cast<uint32_t>(verts.size());
}

/**
 * @brief 画实心矩形：矩形拆成 2 个三角形（6 顶点，顺时针/逆时针由渲染管线
 * 处理面剔除）。
 *
 * r 是屏幕像素坐标——调用方已把局部坐标平移成屏幕坐标，
 * Canvas 层不做任何坐标变换。纹理段固定为 0（白纹理直出顶点色）。
 */
void Canvas::drawRect(const Rect& r, const Rect& clip, Color c) {
    UiVertex v0{r.x,       r.y,       c.r, c.g, c.b, c.a, 0.0f, 0.0f};
    UiVertex v1{r.x + r.w, r.y,       c.r, c.g, c.b, c.a, 0.0f, 0.0f};
    UiVertex v2{r.x + r.w, r.y + r.h, c.r, c.g, c.b, c.a, 0.0f, 0.0f};
    UiVertex v3{r.x,       r.y + r.h, c.r, c.g, c.b, c.a, 0.0f, 0.0f};
    append(clip, 0, {v0, v1, v2, v0, v2, v3});
}

void Canvas::drawTriangle(float x1, float y1, float x2, float y2, float x3, float y3,
                          const Rect& clip, Color c1, Color c2, Color c3) {
    append(clip, 0, {
        UiVertex{x1, y1, c1.r, c1.g, c1.b, c1.a, 0.0f, 0.0f},
        UiVertex{x2, y2, c2.r, c2.g, c2.b, c2.a, 0.0f, 0.0f},
        UiVertex{x3, y3, c3.r, c3.g, c3.b, c3.a, 0.0f, 0.0f},
    });
}

/**
 * @brief 画一行文字：FontEngine 排版出的每个字形转成一个纹理四边形
 * （2 个三角形、6 顶点），颜色四角相同（纯色文字）。
 *
 * 字形所在的 atlas 页 +1 作为纹理段号（0 保留给白纹理的纯色批次）：
 * 不同页的字形自动落入不同 Batch，渲染器逐批换绑 descriptor。
 */
void Canvas::drawText(const char* utf8, int32_t font, float x, float y, float sizePx,
                      const Rect& clip, Color c) {
    FontEngine::instance().forEachGlyph(utf8, sizePx, font,
                                        [&](const PlacedGlyph& g) {
        const float x0 = x + g.x;
        const float y0 = y + g.y;
        const float x1 = x0 + g.w;
        const float y1 = y0 + g.h;
        UiVertex v0{x0, y0, c.r, c.g, c.b, c.a, g.u0, g.v0};
        UiVertex v1{x1, y0, c.r, c.g, c.b, c.a, g.u1, g.v0};
        UiVertex v2{x1, y1, c.r, c.g, c.b, c.a, g.u1, g.v1};
        UiVertex v3{x0, y1, c.r, c.g, c.b, c.a, g.u0, g.v1};
        append(clip, g.page + 1, {v0, v1, v2, v0, v2, v3});
    });
}

} // namespace evk::ui
