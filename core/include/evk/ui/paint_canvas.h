#pragma once

/**
 * @file paint_canvas.h
 * @brief 每帧 2D 绘制命令收集器（立即模式顶点流 + scissor/纹理合批）。
 *
 * 图元在 CPU 侧三角化成顶点流，GPU 只做"纹理 × 顶点色"一次着色：
 *   - 纯色/渐变图元走白纹理（采样恒为 1，直出顶点色，渐变靠顶点插值）；
 *   - 文字/图像走 TextureStore 登记的纹理（顶点色当文字颜色或染色）。
 * 弧/圆类图元用扇形/条带逼近（分段数可调），不做反走样几何——
 * 文字边缘的抗锯齿来自覆盖率采样，几何边缘目前直出（后续可拓展）。
 */
#include "evk/ui/path.h"
#include "evk/ui/render_view.h"
#include "evk/ui/texture_store.h"

#include <cstdint>
#include <vector>

namespace evk::ui {

/// 圆/弧类图元的默认分段数（视觉平滑与顶点数的折中）。
constexpr int kDefaultCircleSegments = 48;
/// 圆角矩形的默认角部分段数（四分之一圆）。
constexpr int kDefaultCornerSegments = 12;

/**
 * @brief UI 顶点：屏幕像素坐标 + 顶点色（分量 0~1）+ 纹理坐标。
 *
 * Canvas 产出的是"已在 CPU 侧算好屏幕坐标、可直接交给 GPU"的顶点流，
 * 顶点着色器不需要任何模型/视图变换（绘制之前已做完坐标与裁剪计算）。
 * uv 配合批次绑定的纹理使用：纯色几何全 0（绑定 1x1 白纹理，采样恒为 1，
 * 公式退化为直出顶点色）；文字/图像几何指向对应纹理区域。
 */
struct UiVertex {
    float x, y;       ///< 屏幕像素坐标
    float r, g, b, a; ///< 顶点色（分量 0~1）
    float u, v;       ///< 纹理坐标（纯色几何为 0）
};

/**
 * @brief 一个 GPU 绘制批次：共享同一裁剪矩形（scissor）与纹理的连续顶点区间。
 *
 * 顶点统一顺序存放在 Canvas::vertices_，Batch 只记区间不复制数据；
 * GPU 侧一个 Batch 一次 draw call（设一次 scissor + 绑一次纹理即可画完整批）。
 */
struct Batch {
    Rect clip; ///< 本批次共享的裁剪矩形（屏幕坐标）
    TextureId textureId; ///< 0 = 纯色批次（1x1 白纹理）；n = TextureStore 第 n 号纹理
    uint32_t firstVertex; ///< vertices_ 中的起始下标
    uint32_t vertexCount; ///< 顶点数（注意不是三角形数）
};

/**
 * @brief 每帧的 2D 绘制命令收集器（立即模式）。
 *
 * 每帧 buildFrame 时 clear() 后从零重建顶点流——视图树是 retained 的，
 * 绘制命令却每帧全新生成，帧间没有任何缓存或脏矩形机制；
 * 连续且 clip、纹理都相同的绘制自动合并成同一 Batch。
 * 本类不持有任何 GPU 资源，只产出 CPU 侧顶点数据，由渲染层消费。
 */
class Canvas {
public:
    /**
     * @brief 每帧开始调用，清空上一帧的内容。
     */
    void clear();

    // ---- 基础图元 ----

    /**
     * @brief 画实心矩形（2 个三角形，6 顶点）。
     * @param r 矩形（屏幕像素坐标）
     * @param clip 裁剪矩形（屏幕坐标）
     * @param c 填充色
     */
    void drawRect(const Rect& r, const Rect& clip, Color c);

    /**
     * @brief 画三色渐变三角形（任意图元的底料）。
     */
    void drawTriangle(float x1, float y1, float x2, float y2, float x3, float y3,
                      const Rect& clip, Color c1, Color c2, Color c3);

    /**
     * @brief 画线段（按宽度展开成四边形，平端头）。
     * @param x1 起点 x
     * @param y1 起点 y
     * @param x2 终点 x
     * @param y2 终点 y
     * @param width 线宽（像素，四边形厚度）
     * @param clip 裁剪矩形
     * @param c 颜色
     */
    void drawLine(float x1, float y1, float x2, float y2, float width,
                  const Rect& clip, Color c);

    /**
     * @brief 填充矢量路径（自适应细分 + ear clipping 三角化）。
     * @param path 路径（坐标已是屏幕绝对坐标）
     * @param clip 裁剪矩形
     * @param c 填充色
     * @param tolerance 细分容差（像素），0.5 左右即可
     */
    void drawPath(const Path& path, const Rect& clip, Color c,
                  float tolerance = 0.5f);

    /**
     * @brief 描边矢量路径（细分成线段，每段展开成四边形，平端头）。
     * @param path 路径（坐标已是屏幕绝对坐标）
     * @param width 线宽（像素）
     * @param clip 裁剪矩形
     * @param c 描边色
     * @param tolerance 细分容差（像素）
     */
    void strokePath(const Path& path, float width, const Rect& clip, Color c,
                    float tolerance = 0.5f);

    // ---- 圆弧类 ----

    /**
     * @brief 画实心圆（三角形扇逼近）。
     * @param cx 圆心 x
     * @param cy 圆心 y
     * @param radius 半径（像素）
     * @param clip 裁剪矩形
     * @param c 填充色
     * @param segments 圆周分段数；0 用默认 kDefaultCircleSegments
     */
    void drawCircle(float cx, float cy, float radius, const Rect& clip, Color c,
                    int segments = 0);

    /**
     * @brief 画实心椭圆（三角形扇逼近）。
     * @param cx 中心 x
     * @param cy 中心 y
     * @param rx 横向半轴
     * @param ry 纵向半轴
     * @param clip 裁剪矩形
     * @param c 填充色
     * @param segments 圆周分段数；0 用默认
     */
    void drawEllipse(float cx, float cy, float rx, float ry, const Rect& clip, Color c,
                     int segments = 0);

    /**
     * @brief 画实心圆角矩形（中部矩形 + 四角四分之一圆扇）。
     * @param r 矩形（屏幕像素坐标）
     * @param radius 圆角半径；自动钳到宽高一半
     * @param clip 裁剪矩形
     * @param c 填充色
     * @param segments 每个角的分段数；0 用默认 kDefaultCornerSegments
     */
    void drawRoundRect(const Rect& r, float radius, const Rect& clip, Color c,
                       int segments = 0);

    /**
     * @brief 画圆弧/环带（内外两条弧之间的条带）。
     *
     * 角度以屏幕坐标定义：0 = +x 方向，y 向下为正方向（顺时针增大）。
     * sweepAngle 为负时反向绘制。
     * @param cx 圆心 x
     * @param cy 圆心 y
     * @param radius 圆弧中心线半径
     * @param thickness 环带厚度（沿半径方向全宽）
     * @param startAngle 起始角（弧度）
     * @param sweepAngle 扫过角（弧度，正=顺时针；2π 即完整圆环）
     * @param clip 裁剪矩形
     * @param c 颜色
     * @param segments 分段数；0 按扫角自动取（每 1/64 圆至少一段）
     */
    void drawArc(float cx, float cy, float radius, float thickness,
                 float startAngle, float sweepAngle, const Rect& clip, Color c,
                 int segments = 0);

    /**
     * @brief 画完整圆环（drawArc 的 2π 特例）。
     */
    void drawRing(float cx, float cy, float radius, float thickness,
                  const Rect& clip, Color c) {
        drawArc(cx, cy, radius, thickness, 0.0f, 3.14159265f * 2.0f, clip, c);
    }

    // ---- 多边形与描边 ----

    /**
     * @brief 画凸多边形（从首顶点出发的三角形扇；凹多边形会被错误填充）。
     * @param points 顶点坐标数组，[x0,y0,x1,y1,...]
     * @param count 顶点数（>= 3 才绘制）
     * @param clip 裁剪矩形
     * @param c 填充色
     */
    void drawConvexPolygon(const float* points, int count, const Rect& clip, Color c);

    /**
     * @brief 画矩形描边（四条填充矩形拼成，线宽向内侧）。
     * @param r 矩形
     * @param width 描边宽度（像素）
     * @param clip 裁剪矩形
     * @param c 颜色
     */
    void strokeRect(const Rect& r, float width, const Rect& clip, Color c);

    /**
     * @brief 画圆角矩形描边（四条边矩形 + 四个角的圆弧环）。
     * @param r 矩形
     * @param radius 圆角半径（自动钳到 >= width）
     * @param width 描边宽度
     * @param clip 裁剪矩形
     * @param c 颜色
     * @param segments 每个角的分段数；0 用默认
     */
    void strokeRoundRect(const Rect& r, float radius, float width, const Rect& clip,
                         Color c, int segments = 0);

    // ---- 渐变与图像 ----

    /**
     * @brief 画双色线性渐变矩形（顶点色插值，GPU 免费渐变）。
     * @param r 矩形
     * @param c0 起始色
     * @param c1 结束色
     * @param horizontal true = 左→右；false = 上→下
     * @param clip 裁剪矩形
     */
    void drawRectGradient(const Rect& r, Color c0, Color c1, bool horizontal,
                          const Rect& clip);

    /**
     * @brief 画一张 TextureStore 纹理（整图拉伸到目标矩形）。
     * @param texture TextureStore 句柄（无效句柄静默跳过）
     * @param r 目标矩形（屏幕像素坐标）
     * @param clip 裁剪矩形
     * @param tint 染色；白色即原样贴图
     */
    void drawImage(TextureId texture, const Rect& r, const Rect& clip,
                   Color tint = Color{1.0f, 1.0f, 1.0f, 1.0f});

    /**
     * @brief 画纹理的子区域（九宫格/图集取块用）。
     * @param texture TextureStore 句柄
     * @param r 目标矩形
     * @param u0 源区域左上 u（0~1）
     * @param v0 源区域左上 v（0~1）
     * @param u1 源区域右下 u
     * @param v1 源区域右下 v
     * @param clip 裁剪矩形
     * @param tint 染色
     */
    void drawImageRect(TextureId texture, const Rect& r,
                       float u0, float v0, float u1, float v1,
                       const Rect& clip, Color tint = Color{1.0f, 1.0f, 1.0f, 1.0f});

    // ---- 文字 ----

    /**
     * @brief 画一行文字：按字体排布逐字形发射纹理四边形。
     *
     * (x, y) 是行盒子（ascent~descent）的左上角，不是基线——
     * 基线由 FontEngine 按字体度量换算，调用方按视觉盒子布局即可。
     * 首次遇到的字形会在 FontEngine 的 atlas 里按需光栅化（当帧生效）。
     * @param utf8 UTF-8 文本
     * @param font 首选字体；缺字时由 FontEngine 按注册顺序回退，kFontAny 表示无偏好
     * @param x 行盒子左上角 x（屏幕像素坐标）
     * @param y 行盒子左上角 y（屏幕像素坐标）
     * @param sizePx 字号（em 像素大小）
     * @param clip 裁剪矩形（屏幕坐标）
     * @param c 文字颜色
     */
    void drawText(const char* utf8, int32_t font, float x, float y, float sizePx,
                  const Rect& clip, Color c);

    // ---- 渲染层消费 ----

    /**
     * @brief 本帧顶点流（顺序追加，供渲染层消费）。
     */
    const std::vector<UiVertex>& vertices() const { return vertices_; }

    /**
     * @brief 本帧合批列表（共享同一 clip 与纹理的连续顶点区间）。
     */
    const std::vector<Batch>& batches() const { return batches_; }

private:
    std::vector<UiVertex> vertices_; ///< 顶点流（帧内顺序追加）
    std::vector<Batch> batches_; ///< 合批列表（只记顶点区间，不复制数据）
    /**
     * @brief 连续且 clip、纹理相同的绘制合并进同一个 Batch；任一变化时开新 Batch。
     */
    void append(const Rect& clip, TextureId textureId, const UiVertex* verts, size_t count);
};

} // namespace evk::ui
