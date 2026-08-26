/**
 * @file paint_canvas.cpp
 * @brief Canvas 立即模式顶点收集：图元三角化 + scissor/纹理合批。
 */
#include "evk/ui/paint_canvas.h"

#include <cmath>

#include "evk/ui/font_engine.h"

namespace evk::ui {

namespace {

/// 两裁剪矩形是否完全相等（合批判定用）。
bool sameClip(const Rect& a, const Rect& b) {
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = kPi * 2.0f;

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
void Canvas::append(const Rect& clip, TextureId textureId, const UiVertex* verts,
                    size_t count) {
    if (count == 0) {
        return;
    }
    if (batches_.empty() || !sameClip(batches_.back().clip, clip) ||
        batches_.back().textureId != textureId) {
        batches_.push_back(Batch{clip, textureId,
                                 static_cast<uint32_t>(vertices_.size()), 0});
    }
    vertices_.insert(vertices_.end(), verts, verts + count);
    batches_.back().vertexCount += static_cast<uint32_t>(count);
}

/**
 * @brief 画实心矩形：矩形拆成 2 个三角形（6 顶点）。
 *
 * r 是屏幕像素坐标——调用方已把局部坐标平移成屏幕坐标，
 * Canvas 层不做任何坐标变换。纹理段固定为 0（白纹理直出顶点色）。
 */
void Canvas::drawRect(const Rect& r, const Rect& clip, Color c) {
    const UiVertex v[6] = {
        {r.x,         r.y,         c.r, c.g, c.b, c.a, 0.0f, 0.0f},
        {r.x + r.w,   r.y,         c.r, c.g, c.b, c.a, 0.0f, 0.0f},
        {r.x + r.w,   r.y + r.h,   c.r, c.g, c.b, c.a, 0.0f, 0.0f},
        {r.x,         r.y,         c.r, c.g, c.b, c.a, 0.0f, 0.0f},
        {r.x + r.w,   r.y + r.h,   c.r, c.g, c.b, c.a, 0.0f, 0.0f},
        {r.x,         r.y + r.h,   c.r, c.g, c.b, c.a, 0.0f, 0.0f},
    };
    append(clip, 0, v, 6);
}

void Canvas::drawTriangle(float x1, float y1, float x2, float y2, float x3, float y3,
                          const Rect& clip, Color c1, Color c2, Color c3) {
    const UiVertex v[3] = {
        {x1, y1, c1.r, c1.g, c1.b, c1.a, 0.0f, 0.0f},
        {x2, y2, c2.r, c2.g, c2.b, c2.a, 0.0f, 0.0f},
        {x3, y3, c3.r, c3.g, c3.b, c3.a, 0.0f, 0.0f},
    };
    append(clip, 0, v, 3);
}

void Canvas::drawLine(float x1, float y1, float x2, float y2, float width,
                      const Rect& clip, Color c) {
    const float dx = x2 - x1;
    const float dy = y2 - y1;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len <= 0.0001f || width <= 0.0f) {
        return;
    }
    // 垂直方向单位向量 × 半线宽：线段撑成一个四边形（平端头）。
    const float hw = width * 0.5f;
    const float nx = -dy / len * hw;
    const float ny = dx / len * hw;
    const UiVertex v[6] = {
        {x1 + nx, y1 + ny, c.r, c.g, c.b, c.a, 0.0f, 0.0f},
        {x2 + nx, y2 + ny, c.r, c.g, c.b, c.a, 0.0f, 0.0f},
        {x2 - nx, y2 - ny, c.r, c.g, c.b, c.a, 0.0f, 0.0f},
        {x1 + nx, y1 + ny, c.r, c.g, c.b, c.a, 0.0f, 0.0f},
        {x2 - nx, y2 - ny, c.r, c.g, c.b, c.a, 0.0f, 0.0f},
        {x1 - nx, y1 - ny, c.r, c.g, c.b, c.a, 0.0f, 0.0f},
    };
    append(clip, 0, v, 6);
}

void Canvas::drawPath(const Path& path, const Rect& clip, Color c, float tolerance) {
    // Path::fill 输出的是屏幕绝对坐标的三角形顶点（每 3 个 Point 一个三角形），
    // 直接转成 UiVertex 追加；白纹理批次，和矩形/圆走同一条管线。
    std::vector<Point> triangles;
    path.fill(tolerance, triangles);
    if (triangles.empty()) {
        return;
    }
    std::vector<UiVertex> verts;
    verts.reserve(triangles.size());
    for (const Point& p : triangles) {
        verts.push_back({p.x, p.y, c.r, c.g, c.b, c.a, 0.0f, 0.0f});
    }
    append(clip, 0, verts.data(), verts.size());
}

void Canvas::strokePath(const Path& path, float width, const Rect& clip, Color c,
                        float tolerance) {
    // Path::stroke 输出的是四边形顶点（每 6 个 Point 一个四边形），
    // 同样转成 UiVertex 追加。
    std::vector<Point> triangles;
    path.stroke(tolerance, width, triangles);
    if (triangles.empty()) {
        return;
    }
    std::vector<UiVertex> verts;
    verts.reserve(triangles.size());
    for (const Point& p : triangles) {
        verts.push_back({p.x, p.y, c.r, c.g, c.b, c.a, 0.0f, 0.0f});
    }
    append(clip, 0, verts.data(), verts.size());
}

void Canvas::drawCircle(float cx, float cy, float radius, const Rect& clip, Color c,
                        int segments) {
    if (radius <= 0.0f) {
        return;
    }
    if (segments <= 0) {
        segments = kDefaultCircleSegments;
    }
    // 三角形扇：圆心一个顶点 + 圆周上相邻两点，转一圈。
    for (int i = 0; i < segments; ++i) {
        const float a0 = kTwoPi * static_cast<float>(i) / segments;
        const float a1 = kTwoPi * static_cast<float>(i + 1) / segments;
        const UiVertex v[3] = {
            {cx, cy, c.r, c.g, c.b, c.a, 0.0f, 0.0f},
            {cx + std::cos(a0) * radius, cy + std::sin(a0) * radius,
             c.r, c.g, c.b, c.a, 0.0f, 0.0f},
            {cx + std::cos(a1) * radius, cy + std::sin(a1) * radius,
             c.r, c.g, c.b, c.a, 0.0f, 0.0f},
        };
        append(clip, 0, v, 3);
    }
}

void Canvas::drawEllipse(float cx, float cy, float rx, float ry, const Rect& clip,
                         Color c, int segments) {
    if (rx <= 0.0f || ry <= 0.0f) {
        return;
    }
    if (segments <= 0) {
        segments = kDefaultCircleSegments;
    }
    for (int i = 0; i < segments; ++i) {
        const float a0 = kTwoPi * static_cast<float>(i) / segments;
        const float a1 = kTwoPi * static_cast<float>(i + 1) / segments;
        const UiVertex v[3] = {
            {cx, cy, c.r, c.g, c.b, c.a, 0.0f, 0.0f},
            {cx + std::cos(a0) * rx, cy + std::sin(a0) * ry,
             c.r, c.g, c.b, c.a, 0.0f, 0.0f},
            {cx + std::cos(a1) * rx, cy + std::sin(a1) * ry,
             c.r, c.g, c.b, c.a, 0.0f, 0.0f},
        };
        append(clip, 0, v, 3);
    }
}

void Canvas::drawRoundRect(const Rect& r, float radius, const Rect& clip, Color c,
                           int segments) {
    if (r.w <= 0.0f || r.h <= 0.0f) {
        return;
    }
    // 半径钳到宽高一半；退化成直角就走矩形。
    if (radius > r.w * 0.5f) radius = r.w * 0.5f;
    if (radius > r.h * 0.5f) radius = r.h * 0.5f;
    if (radius <= 0.5f) {
        drawRect(r, clip, c);
        return;
    }
    if (segments <= 0) {
        segments = kDefaultCornerSegments;
    }
    // 十字分解：竖矩形 + 横矩形 + 四角四分之一圆扇。
    drawRect({r.x + radius, r.y, r.w - radius * 2.0f, r.h}, clip, c);
    drawRect({r.x, r.y + radius, radius, r.h - radius * 2.0f}, clip, c);
    drawRect({r.x + r.w - radius, r.y + radius, radius, r.h - radius * 2.0f}, clip, c);
    const float centers[4][2] = {
        {r.x + radius, r.y + radius},
        {r.x + r.w - radius, r.y + radius},
        {r.x + r.w - radius, r.y + r.h - radius},
        {r.x + radius, r.y + r.h - radius},
    };
    // 四个角各占四分之一圆：左上 [π, 3π/2]、右上 [3π/2, 2π]、
    // 右下 [0, π/2]、左下 [π/2, π]（屏幕坐标 y 向下）。
    const float starts[4] = {kPi, kPi * 1.5f, 0.0f, kPi * 0.5f};
    for (int corner = 0; corner < 4; ++corner) {
        const float cx = centers[corner][0];
        const float cy = centers[corner][1];
        for (int i = 0; i < segments; ++i) {
            const float a0 = starts[corner] +
                             (kPi * 0.5f) * static_cast<float>(i) / segments;
            const float a1 = starts[corner] +
                             (kPi * 0.5f) * static_cast<float>(i + 1) / segments;
            const UiVertex v[3] = {
                {cx, cy, c.r, c.g, c.b, c.a, 0.0f, 0.0f},
                {cx + std::cos(a0) * radius, cy + std::sin(a0) * radius,
                 c.r, c.g, c.b, c.a, 0.0f, 0.0f},
                {cx + std::cos(a1) * radius, cy + std::sin(a1) * radius,
                 c.r, c.g, c.b, c.a, 0.0f, 0.0f},
            };
            append(clip, 0, v, 3);
        }
    }
}

void Canvas::drawArc(float cx, float cy, float radius, float thickness,
                     float startAngle, float sweepAngle, const Rect& clip, Color c,
                     int segments) {
    if (radius <= 0.0f || thickness <= 0.0f || sweepAngle == 0.0f) {
        return;
    }
    if (segments <= 0) {
        // 每 1/64 圆至少一段，扫得越多分得越细。
        segments = std::max(8, static_cast<int>(std::abs(sweepAngle) / kTwoPi * 64.0f));
    }
    const float outer = radius + thickness * 0.5f;
    const float inner = std::max(0.0f, radius - thickness * 0.5f);
    for (int i = 0; i < segments; ++i) {
        const float a0 = startAngle +
                         sweepAngle * static_cast<float>(i) / segments;
        const float a1 = startAngle +
                         sweepAngle * static_cast<float>(i + 1) / segments;
        // 内外弧之间的四边形：条带环的基本单元。
        const UiVertex v[6] = {
            {cx + std::cos(a0) * inner, cy + std::sin(a0) * inner,
             c.r, c.g, c.b, c.a, 0.0f, 0.0f},
            {cx + std::cos(a0) * outer, cy + std::sin(a0) * outer,
             c.r, c.g, c.b, c.a, 0.0f, 0.0f},
            {cx + std::cos(a1) * outer, cy + std::sin(a1) * outer,
             c.r, c.g, c.b, c.a, 0.0f, 0.0f},
            {cx + std::cos(a0) * inner, cy + std::sin(a0) * inner,
             c.r, c.g, c.b, c.a, 0.0f, 0.0f},
            {cx + std::cos(a1) * outer, cy + std::sin(a1) * outer,
             c.r, c.g, c.b, c.a, 0.0f, 0.0f},
            {cx + std::cos(a1) * inner, cy + std::sin(a1) * inner,
             c.r, c.g, c.b, c.a, 0.0f, 0.0f},
        };
        append(clip, 0, v, 6);
    }
}

void Canvas::drawConvexPolygon(const float* points, int count, const Rect& clip, Color c) {
    if (!points || count < 3) {
        return;
    }
    // 凸多边形：从首顶点出发的三角形扇，count-2 个三角形。
    for (int i = 1; i + 1 < count; ++i) {
        const UiVertex v[3] = {
            {points[0], points[1], c.r, c.g, c.b, c.a, 0.0f, 0.0f},
            {points[i * 2], points[i * 2 + 1], c.r, c.g, c.b, c.a, 0.0f, 0.0f},
            {points[(i + 1) * 2], points[(i + 1) * 2 + 1],
             c.r, c.g, c.b, c.a, 0.0f, 0.0f},
        };
        append(clip, 0, v, 3);
    }
}

void Canvas::strokeRect(const Rect& r, float width, const Rect& clip, Color c) {
    if (r.w <= 0.0f || r.h <= 0.0f || width <= 0.0f) {
        return;
    }
    if (width > r.w * 0.5f) width = r.w * 0.5f;
    if (width > r.h * 0.5f) width = r.h * 0.5f;
    // 四条填充矩形（线宽向内侧），角上重叠一点无碍（同色不透明）。
    drawRect({r.x, r.y, r.w, width}, clip, c);
    drawRect({r.x, r.y + r.h - width, r.w, width}, clip, c);
    drawRect({r.x, r.y + width, width, r.h - width * 2.0f}, clip, c);
    drawRect({r.x + r.w - width, r.y + width, width, r.h - width * 2.0f}, clip, c);
}

void Canvas::strokeRoundRect(const Rect& r, float radius, float width, const Rect& clip,
                             Color c, int segments) {
    if (r.w <= 0.0f || r.h <= 0.0f || width <= 0.0f) {
        return;
    }
    if (radius <= width) {
        strokeRect(r, width, clip, c);
        return;
    }
    // 半径钳到宽高一半。
    if (radius > r.w * 0.5f) radius = r.w * 0.5f;
    if (radius > r.h * 0.5f) radius = r.h * 0.5f;
    if (segments <= 0) {
        segments = kDefaultCornerSegments;
    }
    // 四条边矩形 + 四个角圆弧环（环中心线半径 = radius - width/2，
    // 外缘恰与圆角矩形轮廓重合，内缘与边矩形对齐）。
    const float rc = radius - width * 0.5f;
    drawRect({r.x + radius, r.y, r.w - radius * 2.0f, width}, clip, c);
    drawRect({r.x + radius, r.y + r.h - width, r.w - radius * 2.0f, width}, clip, c);
    drawRect({r.x, r.y + radius, width, r.h - radius * 2.0f}, clip, c);
    drawRect({r.x + r.w - width, r.y + radius, width, r.h - radius * 2.0f}, clip, c);
    const float centers[4][2] = {
        {r.x + radius, r.y + radius},
        {r.x + r.w - radius, r.y + radius},
        {r.x + r.w - radius, r.y + r.h - radius},
        {r.x + radius, r.y + r.h - radius},
    };
    const float starts[4] = {kPi, kPi * 1.5f, 0.0f, kPi * 0.5f};
    for (int corner = 0; corner < 4; ++corner) {
        drawArc(centers[corner][0], centers[corner][1], rc, width,
                starts[corner], kPi * 0.5f, clip, c, segments);
    }
}

void Canvas::drawRectGradient(const Rect& r, Color c0, Color c1, bool horizontal,
                              const Rect& clip) {
    if (r.w <= 0.0f || r.h <= 0.0f) {
        return;
    }
    // 两个颜色放在梯度方向的两端，四个角按位置取色，
    // GPU 光栅化时逐像素插值——"免费"的线性渐变。
    const Color left0 = horizontal ? c0 : c0;
    const Color right0 = horizontal ? c1 : c0;
    const Color left1 = horizontal ? c0 : c1;
    const Color right1 = horizontal ? c1 : c1;
    const UiVertex v[6] = {
        {r.x,         r.y,         left0.r,  left0.g,  left0.b,  left0.a,  0.0f, 0.0f},
        {r.x + r.w,   r.y,         right0.r, right0.g, right0.b, right0.a, 0.0f, 0.0f},
        {r.x + r.w,   r.y + r.h,   right1.r, right1.g, right1.b, right1.a, 0.0f, 0.0f},
        {r.x,         r.y,         left0.r,  left0.g,  left0.b,  left0.a,  0.0f, 0.0f},
        {r.x + r.w,   r.y + r.h,   right1.r, right1.g, right1.b, right1.a, 0.0f, 0.0f},
        {r.x,         r.y + r.h,   left1.r,  left1.g,  left1.b,  left1.a,  0.0f, 0.0f},
    };
    append(clip, 0, v, 6);
}

void Canvas::drawImage(TextureId texture, const Rect& r, const Rect& clip, Color tint) {
    drawImageRect(texture, r, 0.0f, 0.0f, 1.0f, 1.0f, clip, tint);
}

void Canvas::drawImageRect(TextureId texture, const Rect& r,
                           float u0, float v0, float u1, float v1,
                           const Rect& clip, Color tint) {
    if (texture == kInvalidTexture || r.w <= 0.0f || r.h <= 0.0f) {
        return;
    }
    const UiVertex v[6] = {
        {r.x,         r.y,         tint.r, tint.g, tint.b, tint.a, u0, v0},
        {r.x + r.w,   r.y,         tint.r, tint.g, tint.b, tint.a, u1, v0},
        {r.x + r.w,   r.y + r.h,   tint.r, tint.g, tint.b, tint.a, u1, v1},
        {r.x,         r.y,         tint.r, tint.g, tint.b, tint.a, u0, v0},
        {r.x + r.w,   r.y + r.h,   tint.r, tint.g, tint.b, tint.a, u1, v1},
        {r.x,         r.y + r.h,   tint.r, tint.g, tint.b, tint.a, u0, v1},
    };
    append(clip, texture, v, 6);
}

/**
 * @brief 画一行文字：FontEngine 排版出的每个字形转成一个纹理四边形
 * （2 个三角形、6 顶点），颜色四角相同（纯色文字）。
 *
 * 字形所在 atlas 页的 TextureStore 句柄直接作为批次纹理号
 * （0 保留给白纹理的纯色批次）：不同页的字形自动落入不同 Batch，
 * 渲染器逐批换绑 descriptor。
 */
void Canvas::drawText(const char* utf8, int32_t font, float x, float y, float sizePx,
                      const Rect& clip, Color c) {
    FontEngine::instance().forEachGlyph(utf8, sizePx, font,
                                        [&](const PlacedGlyph& g) {
        const float x0 = x + g.x;
        const float y0 = y + g.y;
        const float x1 = x0 + g.w;
        const float y1 = y0 + g.h;
        const UiVertex v[6] = {
            {x0, y0, c.r, c.g, c.b, c.a, g.u0, g.v0},
            {x1, y0, c.r, c.g, c.b, c.a, g.u1, g.v0},
            {x1, y1, c.r, c.g, c.b, c.a, g.u1, g.v1},
            {x0, y0, c.r, c.g, c.b, c.a, g.u0, g.v0},
            {x1, y1, c.r, c.g, c.b, c.a, g.u1, g.v1},
            {x0, y1, c.r, c.g, c.b, c.a, g.u0, g.v1},
        };
        append(clip, g.texture, v, 6);
    });
}

} // namespace evk::ui
