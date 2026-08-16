#pragma once

/**
 * @file zy_paint_canvas.h
 * @brief 每帧 2D 绘制命令收集器（立即模式顶点流 + scissor 合批）。
 */
#include "evk/ui/zy_render_view.h"

#include <cstdint>
#include <initializer_list>
#include <vector>

namespace evk::ui {

/**
 * @brief UI 顶点：屏幕像素坐标 + 顶点色（分量 0~1）。
 *
 * Canvas 产出的是"已在 CPU 侧算好屏幕坐标、可直接交给 GPU"的顶点流，
 * 顶点着色器不需要任何模型/视图变换（绘制之前已做完坐标与裁剪计算）。
 */
struct UiVertex {
    float x, y;       ///< 屏幕像素坐标
    float r, g, b, a; ///< 顶点色（分量 0~1）
};

/**
 * @brief 一个 GPU 绘制批次：共享同一裁剪矩形（scissor）的连续顶点区间。
 *
 * 顶点统一顺序存放在 Canvas::vertices_，Batch 只记区间不复制数据；
 * GPU 侧一个 Batch 一次 draw call（设置一次 scissor 即可画完整批）。
 */
struct Batch {
    Rect clip; ///< 本批次共享的裁剪矩形（屏幕坐标）
    uint32_t firstVertex; ///< vertices_ 中的起始下标
    uint32_t vertexCount; ///< 顶点数（注意不是三角形数）
};

/**
 * @brief 每帧的 2D 绘制命令收集器（立即模式）。
 *
 * 每帧 buildFrame 时 clear() 后从零重建顶点流——视图树是 retained 的，
 * 绘制命令却每帧全新生成，帧间没有任何缓存或脏矩形机制；
 * 连续相同 clip 的绘制自动合并成同一 Batch（scissor 合批优化）。
 * 本类不持有任何 GPU 资源，只产出 CPU 侧顶点数据，由渲染层消费。
 */
class Canvas {
public:
    /**
     * @brief 每帧开始调用，清空上一帧的内容。
     */
    void clear();

    /**
     * @brief 画实心矩形（2 个三角形，6 顶点）。
     * @param r 矩形（屏幕像素坐标）
     * @param clip 裁剪矩形（屏幕坐标）
     * @param c 填充色
     */
    void drawRect(const Rect& r, const Rect& clip, Color c);

    /**
     * @brief 画三色渐变三角形。
     * @param x1 顶点 1 x（屏幕像素坐标）
     * @param y1 顶点 1 y
     * @param x2 顶点 2 x
     * @param y2 顶点 2 y
     * @param x3 顶点 3 x
     * @param y3 顶点 3 y
     * @param clip 裁剪矩形（屏幕坐标）
     * @param c1 顶点 1 颜色
     * @param c2 顶点 2 颜色
     * @param c3 顶点 3 颜色
     */
    void drawTriangle(float x1, float y1, float x2, float y2, float x3, float y3,
                      const Rect& clip, Color c1, Color c2, Color c3);

    /**
     * @brief 本帧顶点流（顺序追加，供渲染层消费）。
     */
    const std::vector<UiVertex>& vertices() const { return vertices_; }

    /**
     * @brief 本帧合批列表（共享同一 clip 的连续顶点区间）。
     */
    const std::vector<Batch>& batches() const { return batches_; }

private:
    std::vector<UiVertex> vertices_; ///< 顶点流（帧内顺序追加）
    std::vector<Batch> batches_; ///< 合批列表（只记顶点区间，不复制数据）
    /**
     * @brief 连续相同 clip 的绘制合并进同一个 Batch；clip 变化时开新 Batch。
     */
    void append(const Rect& clip, std::initializer_list<UiVertex> verts);
};

} // namespace evk::ui
