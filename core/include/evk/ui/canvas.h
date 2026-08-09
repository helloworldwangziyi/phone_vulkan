#pragma once

#include "evk/ui/view.h"

#include <cstdint>
#include <initializer_list>
#include <vector>

namespace evk::ui {

// UI 顶点：屏幕像素坐标 + 顶点色。
struct UiVertex {
    float x, y;
    float r, g, b, a;
};

// 一批共享同一裁剪矩形的连续顶点。
struct Batch {
    Rect clip;
    uint32_t firstVertex;
    uint32_t vertexCount;
};

// 每帧的 2D 绘制命令收集器：顶点 + 按 clip 分批。
class Canvas {
public:
    // 每帧开始调用，清空上一帧的内容。
    void clear();

    // 画实心矩形（2 个三角形，6 顶点）。
    void drawRect(const Rect& r, const Rect& clip, Color c);

    // 画三色渐变三角形。
    void drawTriangle(float x1, float y1, float x2, float y2, float x3, float y3,
                      const Rect& clip, Color c1, Color c2, Color c3);

    const std::vector<UiVertex>& vertices() const { return vertices_; }
    const std::vector<Batch>& batches() const { return batches_; }

private:
    // 连续相同 clip 的绘制合并进同一个 Batch；clip 变化时开新 Batch。
    std::vector<UiVertex> vertices_;
    std::vector<Batch> batches_;
    void append(const Rect& clip, std::initializer_list<UiVertex> verts);
};

} // namespace evk::ui
