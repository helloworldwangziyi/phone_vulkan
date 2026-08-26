#pragma once

/**
 * @file path.h
 * @brief 矢量路径：直线/二次/三次贝塞尔命令 + 自适应细分与三角化。
 *
 * 参考 Flutter/Skia 的 Path 抽象：框架层只描述"画什么"（moveTo/lineTo/
 * quadTo/cubicTo/close），细分与三角化在本模块完成。贝塞尔曲线用
 * de Casteljau 递归细分，曲率大的地方多分、平直的地方少分；任意多边形
 * 用 ear clipping 三角化成独立三角形，走和矩形/圆完全相同的顶点流。
 *
 * 当前限制：单轮廓填充（不带洞）；描边用平端头线段拼接，连接处
 * 低线宽下可接受，后续可加 round join。
 */
#include <cstdint>
#include <vector>

namespace evk::ui {

/// 二维点。
struct Point {
    float x = 0.0f;
    float y = 0.0f;
};

/**
 * @brief 矢量路径：由 moveTo/lineTo/quadTo/cubicTo/close 命令组成的轮廓。
 *
 * 支持多个子路径（moveTo 分隔）；fill() 对每个闭合子路径单独三角化，
 * stroke() 把所有子路径细分成线段。
 */
class Path {
public:
    Path() = default;

    /// 移动到起点（开始新子路径）。
    Path& moveTo(float x, float y);
    /// 画直线到 (x, y)。
    Path& lineTo(float x, float y);
    /// 画二次贝塞尔曲线到 (x, y)，控制点 (cx, cy)。
    Path& quadTo(float cx, float cy, float x, float y);
    /// 画三次贝塞尔曲线到 (x, y)，控制点 (c1x,c1y)、(c2x,c2y)。
    Path& cubicTo(float c1x, float c1y, float c2x, float c2y, float x, float y);
    /// 闭合当前子路径（回到最近 moveTo 的起点）。
    Path& close();

    /// 清空所有命令，回到空路径。
    void reset();

    /// 返回平移 (dx, dy) 后的新路径（PaintContext 做局部→屏幕坐标转换用）。
    Path translated(float dx, float dy) const;

    /// 返回以原点为中心缩放 (sx, sy) 后的新路径。
    Path scaled(float sx, float sy) const;

    /**
     * @brief 填充路径：自适应细分 + ear clipping 三角化。
     * @param tolerance 细分容差（像素）；控制点到弦的距离小于此值时停止细分。
     *                  0.5 左右即可得到平滑曲线，越小顶点越多。
     *                  内部会钳制到 0.01 下限，传 0/负值是安全的。
     * @param outTriangles 输出三角形顶点（每 3 个 Point 一个三角形，已展开）。
     *                     结果为追加写入，调用前如需清空请自行 clear()。
     */
    void fill(float tolerance, std::vector<Point>& outTriangles) const;

    /**
     * @brief 描边路径：细分成线段，每段展开成四边形（平端头）；
     *        close() 闭合的轮廓会补画末点回到起点的闭合段。
     * @param tolerance 细分容差（像素），同 fill()。
     * @param width 线宽（像素）。
     * @param outTriangles 输出三角形顶点（每 6 个 Point 一个四边形），追加写入。
     */
    void stroke(float tolerance, float width, std::vector<Point>& outTriangles) const;

private:
    /// 路径命令类型。
    enum class Verb : uint8_t {
        Move,   ///< 移动到新起点
        Line,   ///< 直线
        Quad,   ///< 二次贝塞尔
        Cubic,  ///< 三次贝塞尔
        Close,  ///< 闭合
    };

    /// 一条命令：verb + 终点 + 控制点（直线/move 只用 x,y）。
    struct VerbPoint {
        Verb verb = Verb::Move;
        float x = 0.0f, y = 0.0f;
        float cx = 0.0f, cy = 0.0f;
        float c2x = 0.0f, c2y = 0.0f;
    };

    std::vector<VerbPoint> verbs_;

    /// 细分后的子轮廓：点序列 + 是否以 close() 显式闭合。
    struct Contour {
        std::vector<Point> points;
        bool closed = false;
    };

    /// 把所有命令细分成多个子轮廓（fill 按多边形三角化，stroke 按线段展开）。
    void flatten(float tolerance, std::vector<Contour>& contours) const;
};

} // namespace evk::ui
