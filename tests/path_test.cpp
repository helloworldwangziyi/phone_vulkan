/// @file path_test.cpp
/// Path（矢量路径）的主机侧验证：
///   1. 填充三角化面积守恒（五角星凹多边形、顺时针矩形、共线顶点）；
///   2. 描边：闭合路径补画闭合段、开放路径不补、两点线段有效；
///   3. 容差为 0/负值/NaN 时细分有界（不无限递归）。
///
/// 运行：tests/run_path_test.sh
#include <cmath>
#include <cstdio>
#include <vector>

#include "evk/ui/path.h"

namespace {

using evk::ui::Path;
using evk::ui::Point;

int g_failures = 0;

void expect(bool cond, const char* what) {
    if (!cond) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    } else {
        std::printf("ok: %s\n", what);
    }
}

float triArea(Point a, Point b, Point c) {
    return 0.5f * ((b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x));
}

float fillArea(const Path& p, float tolerance = 0.5f) {
    std::vector<Point> tris;
    p.fill(tolerance, tris);
    float sum = 0.0f;
    for (size_t i = 0; i + 2 < tris.size(); i += 3) {
        sum += std::fabs(triArea(tris[i], tris[i + 1], tris[i + 2]));
    }
    return sum;
}

size_t strokeSegments(const Path& p, float tolerance = 0.5f) {
    std::vector<Point> out;
    p.stroke(tolerance, 2.0f, out);
    return out.size() / 6;
}

bool near(float a, float b, float eps = 1.0f) {
    return std::fabs(a - b) < eps;
}

Path makeStar() { // 五角星（凹多边形）
    Path p;
    const float cx = 50, cy = 50, R = 40, r = 16.8f;
    for (int i = 0; i < 10; ++i) {
        const float a = (-90.0f + i * 36.0f) * 3.14159265f / 180.0f;
        const float rr = (i % 2 == 0) ? R : r;
        const float x = cx + rr * std::cos(a);
        const float y = cy + rr * std::sin(a);
        if (i == 0) p.moveTo(x, y); else p.lineTo(x, y);
    }
    p.close();
    return p;
}

} // namespace

int main() {
    // ---- 填充：面积守恒 ----
    expect(near(fillArea(makeStar()), 1975.0f, 1.0f),
           "star fill triangulates to analytic area");

    { // 顺时针矩形（内部应自动反转成逆时针）
        Path p; p.moveTo(0, 0).lineTo(0, 100).lineTo(100, 100).lineTo(100, 0).close();
        expect(near(fillArea(p), 10000.0f), "clockwise rect fill area correct");
    }

    { // 一条边中间带共线顶点，不卡死、面积正确
        Path p; p.moveTo(0, 0).lineTo(50, 0).lineTo(100, 0)
                 .lineTo(100, 100).lineTo(0, 100).close();
        expect(near(fillArea(p), 10000.0f), "collinear vertex fill area correct");
    }

    // ---- 描边：闭合段处理 ----
    { // 闭合三角形：3 段（含末→首闭合段）
        Path p; p.moveTo(0, 0).lineTo(10, 0).lineTo(10, 10).close();
        expect(strokeSegments(p) == 3, "closed triangle stroke has closing segment");
    }
    { // 同样的三角形不写 close()：开放路径，2 段
        Path p; p.moveTo(0, 0).lineTo(10, 0).lineTo(10, 10);
        expect(strokeSegments(p) == 2, "open triangle stroke has no closing segment");
    }
    { // 两点线段（moveTo + lineTo）是合法的描边路径：1 段
        Path p; p.moveTo(0, 0).lineTo(10, 10);
        expect(strokeSegments(p) == 1, "two-point line strokes as one segment");
    }
    { // 闭合五角星：10 段
        expect(strokeSegments(makeStar()) == 10, "closed star stroke has 10 segments");
    }

    // ---- 容差健壮性：0/负值必须正常返回（此前会无限递归栈溢出）----
    {
        Path p; p.moveTo(0, 0).quadTo(50, 100, 100, 0);
        expect(strokeSegments(p, 0.0f) > 0, "zero tolerance stroke terminates");
    }
    {
        Path p; p.moveTo(0, 0).cubicTo(30, 100, 70, -100, 100, 0).close();
        expect(fillArea(p, -1.0f) > 0.0f, "negative tolerance fill terminates");
    }
    {
        Path p; p.moveTo(0, 0).quadTo(50, 100, 100, 0);
        expect(strokeSegments(p, NAN) > 0, "NaN tolerance stroke terminates");
    }

    if (g_failures > 0) {
        std::fprintf(stderr, "path_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("path_test: all passed\n");
    return 0;
}
