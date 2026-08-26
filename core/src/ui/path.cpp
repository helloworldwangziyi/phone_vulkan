/**
 * @file path.cpp
 * @brief 矢量路径实现：de Casteljau 自适应细分 + ear clipping 三角化。
 */
#include "evk/ui/path.h"

#include <algorithm>
#include <cmath>

namespace evk::ui {

namespace {

// ---- 基础几何工具 ----

float cross(Point a, Point b, Point c) {
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

/// 点 p 到线段 a-b 的距离。
float distanceToSegment(Point p, Point a, Point b) {
    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    const float len2 = dx * dx + dy * dy;
    if (len2 < 1e-12f) {
        return std::sqrt((p.x - a.x) * (p.x - a.x) + (p.y - a.y) * (p.y - a.y));
    }
    float t = ((p.x - a.x) * dx + (p.y - a.y) * dy) / len2;
    t = std::max(0.0f, std::min(1.0f, t));
    const float px = a.x + t * dx;
    const float py = a.y + t * dy;
    return std::sqrt((p.x - px) * (p.x - px) + (p.y - py) * (p.y - py));
}

Point midpoint(Point a, Point b) {
    return {(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f};
}

// ---- 贝塞尔自适应细分（de Casteljau）----

/// 细分递归深度上限：防止退化输入（NaN 坐标、零距离等）无限递归。
/// 2^24 段远超任何屏幕分辨率的平滑需求。
constexpr int kMaxFlattenDepth = 24;

/// 二次贝塞尔递归细分：控制点到弦的距离 < tolerance 时停止。
void flattenQuad(Point p0, Point p1, Point p2, float tolerance, int depth,
                 std::vector<Point>& out) {
    if (depth <= 0 || distanceToSegment(p1, p0, p2) < tolerance) {
        out.push_back(p2);
        return;
    }
    const Point p01 = midpoint(p0, p1);
    const Point p12 = midpoint(p1, p2);
    const Point p012 = midpoint(p01, p12);
    flattenQuad(p0, p01, p012, tolerance, depth - 1, out);
    flattenQuad(p012, p12, p2, tolerance, depth - 1, out);
}

/// 三次贝塞尔递归细分：两个控制点到弦的距离都 < tolerance 时停止。
void flattenCubic(Point p0, Point p1, Point p2, Point p3, float tolerance,
                  int depth, std::vector<Point>& out) {
    if (depth <= 0 ||
        (distanceToSegment(p1, p0, p3) < tolerance &&
         distanceToSegment(p2, p0, p3) < tolerance)) {
        out.push_back(p3);
        return;
    }
    const Point p01 = midpoint(p0, p1);
    const Point p12 = midpoint(p1, p2);
    const Point p23 = midpoint(p2, p3);
    const Point p012 = midpoint(p01, p12);
    const Point p123 = midpoint(p12, p23);
    const Point p0123 = midpoint(p012, p123);
    flattenCubic(p0, p01, p012, p0123, tolerance, depth - 1, out);
    flattenCubic(p0123, p123, p23, p3, tolerance, depth - 1, out);
}

// ---- Ear clipping 三角化 ----

/// 多边形有向面积（正=逆时针，负=顺时针）。
float signedArea(const std::vector<Point>& poly) {
    float area = 0.0f;
    const size_t n = poly.size();
    for (size_t i = 0; i < n; ++i) {
        const Point& a = poly[i];
        const Point& b = poly[(i + 1) % n];
        area += a.x * b.y - b.x * a.y;
    }
    return area * 0.5f;
}

/// 点 p 是否在三角形 a-b-c 内部（含边界，a-b-c 须为逆时针）。
bool pointInTriangle(Point p, Point a, Point b, Point c) {
    return cross(a, b, p) >= -1e-6f &&
           cross(b, c, p) >= -1e-6f &&
           cross(c, a, p) >= -1e-6f;
}

/**
 * @brief ear clipping 三角化简单多边形（无自交、无洞）。
 * @param poly 多边形顶点（会被修改：顺时针时自动反转）
 * @param out 输出三角形（每 3 个 Point 一个）
 */
void triangulateEarClipping(std::vector<Point>& poly, std::vector<Point>& out) {
    const size_t n = poly.size();
    if (n < 3) return;

    // 统一为逆时针方向。
    if (signedArea(poly) < 0.0f) {
        std::reverse(poly.begin(), poly.end());
    }

    // 用索引链表管理剩余顶点。
    std::vector<int> next(n);
    for (size_t i = 0; i < n; ++i) {
        next[i] = static_cast<int>((i + 1) % n);
    }
    std::vector<int> prev(n);
    for (size_t i = 0; i < n; ++i) {
        prev[i] = static_cast<int>((i + n - 1) % n);
    }

    int remaining = static_cast<int>(n);
    int guard = 0; // 防止退化多边形死循环
    int ear = 0;

    while (remaining > 3 && guard < static_cast<int>(n) * 3) {
        const int a = prev[ear];
        const int b = ear;
        const int c = next[ear];

        bool isEar = false;
        // 凸顶点（逆时针时叉积 > 0）。
        if (cross(poly[a], poly[b], poly[c]) > 1e-6f) {
            isEar = true;
            // 检查没有其他顶点落在三角形内部。
            int k = next[c];
            while (k != a) {
                if (pointInTriangle(poly[k], poly[a], poly[b], poly[c])) {
                    isEar = false;
                    break;
                }
                k = next[k];
            }
        }

        if (isEar) {
            out.push_back(poly[a]);
            out.push_back(poly[b]);
            out.push_back(poly[c]);
            // 切掉耳朵：b 从链表移除。
            next[a] = c;
            prev[c] = a;
            --remaining;
            guard = 0;
            ear = c;
        } else {
            ++guard;
            ear = next[ear];
        }
    }

    // 最后三个顶点组成最后一个三角形。
    if (remaining == 3) {
        const int a = prev[ear];
        const int b = ear;
        const int c = next[ear];
        out.push_back(poly[a]);
        out.push_back(poly[b]);
        out.push_back(poly[c]);
    }
}

} // namespace

// ---- Path 命令追加 ----

Path& Path::moveTo(float x, float y) {
    verbs_.push_back({Verb::Move, x, y, 0, 0, 0, 0});
    return *this;
}

Path& Path::lineTo(float x, float y) {
    verbs_.push_back({Verb::Line, x, y, 0, 0, 0, 0});
    return *this;
}

Path& Path::quadTo(float cx, float cy, float x, float y) {
    verbs_.push_back({Verb::Quad, x, y, cx, cy, 0, 0});
    return *this;
}

Path& Path::cubicTo(float c1x, float c1y, float c2x, float c2y, float x, float y) {
    verbs_.push_back({Verb::Cubic, x, y, c1x, c1y, c2x, c2y});
    return *this;
}

Path& Path::close() {
    verbs_.push_back({Verb::Close, 0, 0, 0, 0, 0, 0});
    return *this;
}

void Path::reset() {
    verbs_.clear();
}

Path Path::translated(float dx, float dy) const {
    Path result;
    result.verbs_.reserve(verbs_.size());
    for (const VerbPoint& v : verbs_) {
        VerbPoint nv = v;
        nv.x += dx;   nv.y += dy;
        nv.cx += dx;  nv.cy += dy;
        nv.c2x += dx; nv.c2y += dy;
        result.verbs_.push_back(nv);
    }
    return result;
}

Path Path::scaled(float sx, float sy) const {
    Path result;
    result.verbs_.reserve(verbs_.size());
    for (const VerbPoint& v : verbs_) {
        VerbPoint nv = v;
        nv.x *= sx;   nv.y *= sy;
        nv.cx *= sx;  nv.cy *= sy;
        nv.c2x *= sx; nv.c2y *= sy;
        result.verbs_.push_back(nv);
    }
    return result;
}

// ---- 细分：命令 → 多个子轮廓 ----

void Path::flatten(float tolerance, std::vector<Contour>& contours) const {
    contours.clear();
    // 钳制容差下限：0/负值/NaN 会让递归细分的停止条件永不成立。
    tolerance = std::max(0.01f, tolerance);
    Contour current;
    Point cur{0.0f, 0.0f};
    Point subStart{0.0f, 0.0f};

    // 轮廓最少 2 点：fill 需要 3 点会自行跳过，stroke 两点成线仍有效。
    for (const VerbPoint& v : verbs_) {
        switch (v.verb) {
            case Verb::Move:
                if (current.points.size() >= 2) {
                    contours.push_back(std::move(current));
                    current = Contour{};
                }
                cur = {v.x, v.y};
                subStart = cur;
                current.points.push_back(cur);
                break;

            case Verb::Line:
                cur = {v.x, v.y};
                current.points.push_back(cur);
                break;

            case Verb::Quad:
                flattenQuad(cur, {v.cx, v.cy}, {v.x, v.y}, tolerance,
                            kMaxFlattenDepth, current.points);
                cur = {v.x, v.y};
                break;

            case Verb::Cubic:
                flattenCubic(cur, {v.cx, v.cy}, {v.c2x, v.c2y},
                             {v.x, v.y}, tolerance, kMaxFlattenDepth,
                             current.points);
                cur = {v.x, v.y};
                break;

            case Verb::Close:
                if (current.points.size() >= 2) {
                    // close 不追加起点（轮廓已隐含闭合），标记闭合后收存。
                    current.closed = true;
                    contours.push_back(std::move(current));
                    current = Contour{};
                }
                cur = subStart;
                current.points.push_back(cur);
                break;
        }
    }
    if (current.points.size() >= 2) {
        contours.push_back(std::move(current));
    }
}

// ---- 填充：细分 + ear clipping ----

void Path::fill(float tolerance, std::vector<Point>& outTriangles) const {
    std::vector<Contour> contours;
    flatten(tolerance, contours);
    for (auto& contour : contours) {
        if (contour.points.size() >= 3) {
            triangulateEarClipping(contour.points, outTriangles);
        }
    }
}

// ---- 描边：细分 + 每段线段展开成四边形 ----

void Path::stroke(float tolerance, float width,
                  std::vector<Point>& outTriangles) const {
    std::vector<Contour> contours;
    flatten(tolerance, contours);
    const float hw = width * 0.5f;

    for (const auto& contour : contours) {
        const std::vector<Point>& pts = contour.points;
        if (pts.size() < 2) continue;
        // 闭合轮廓多画一段：末点回到起点，不留缺口。
        const size_t segCount = contour.closed ? pts.size() : pts.size() - 1;
        for (size_t i = 0; i < segCount; ++i) {
            const Point p1 = pts[i];
            const Point p2 = pts[(i + 1) % pts.size()];
            const float dx = p2.x - p1.x;
            const float dy = p2.y - p1.y;
            const float len = std::sqrt(dx * dx + dy * dy);
            if (len < 1e-6f) continue;
            // 垂直方向单位向量 × 半线宽。
            const float nx = -dy / len * hw;
            const float ny = dx / len * hw;
            // 四边形拆成两个三角形（6 顶点）。
            outTriangles.push_back({p1.x + nx, p1.y + ny});
            outTriangles.push_back({p2.x + nx, p2.y + ny});
            outTriangles.push_back({p2.x - nx, p2.y - ny});
            outTriangles.push_back({p1.x + nx, p1.y + ny});
            outTriangles.push_back({p2.x - nx, p2.y - ny});
            outTriangles.push_back({p1.x - nx, p1.y - ny});
        }
    }
}

} // namespace evk::ui
