#pragma once

#include "evk/ui/path.h"
#include "evk/ui/texture_store.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <vector>

namespace evk::ui {

class Canvas;

struct Size {
    float width = 0.0f;
    float height = 0.0f;
};

struct Offset {
    float x = 0.0f;
    float y = 0.0f;
};

struct Rect {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;

    bool contains(float px, float py) const;
    static Rect intersect(const Rect& a, const Rect& b);
};

/**
 * @brief 盒约束：父布局下行给孩子的尺寸许可范围（Flutter 的 BoxConstraints）。
 *
 * 布局协议是双向的：父经 View::layout(constraints) 把 min/max 宽高下行，
 * 孩子在 performLayout 里自定尺寸并回报（上行），父随后用 setPosition
 * 写偏移。孩子回报的尺寸一定落在 [min, max] 区间内（layout 入口统一
 * constrain 钳制）。
 *
 * 无界轴用 kInfinite 表示（如 Column 给孩子测量的主轴上限、滚动方向）。
 * tight(w,h) = min=max 的强制尺寸；loose(w,h) = 只设上限、孩子自定。
 */
struct BoxConstraints {
    static constexpr float kInfinite = std::numeric_limits<float>::infinity();

    float minWidth = 0.0f;
    float maxWidth = kInfinite;
    float minHeight = 0.0f;
    float maxHeight = kInfinite;

    /// 强制尺寸：min=max（根视口、flex 份额、显式尺寸覆盖）。
    static BoxConstraints tight(float width, float height);
    /// 只设上限：孩子在 [0, max] 内自定尺寸（测量用）。
    static BoxConstraints loose(float width, float height);
    /// 四边内缩（Padding 用）：min/max 同步扣除，结果保持 min <= max。
    BoxConstraints deflate(float horizontal, float vertical) const;

    bool isWidthTight() const { return minWidth >= maxWidth; }
    bool isHeightTight() const { return minHeight >= maxHeight; }
    bool isWidthBounded() const { return maxWidth < kInfinite; }
    bool isHeightBounded() const { return maxHeight < kInfinite; }

    float constrainWidth(float value) const;
    float constrainHeight(float value) const;
    Size constrain(Size size) const;
    /// 有界轴取 max（撑满），无界轴取 min（默认 0：无约束时裸 View 不占位）。
    Size biggest() const;

    bool operator==(const BoxConstraints& other) const;
    bool operator!=(const BoxConstraints& other) const { return !(*this == other); }
};

struct Color {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 1.0f;

    static Color rgba(uint32_t value);
};

struct ClickEvent {
    float x = 0.0f;
    float y = 0.0f;
};

enum class PanState {
    Begin,
    Update,
    End,
    Cancel,
};

struct PanEvent {
    PanState state = PanState::Begin;
    float x = 0.0f;
    float y = 0.0f;
    float deltaX = 0.0f;
    float deltaY = 0.0f;
    float translationX = 0.0f;
    float translationY = 0.0f;
    float velocityX = 0.0f;
    float velocityY = 0.0f;
};

class PaintContext {
public:
    PaintContext(Canvas& canvas, const Rect& bounds, const Rect& clip);

    Size size() const { return {bounds_.w, bounds_.h}; }
    /// 裁剪矩形（视图局部坐标）：逐行/逐块自裁剪的 painter（如多行文本）
    /// 用它判断可见性。
    Rect clip() const {
        return {clip_.x - bounds_.x, clip_.y - bounds_.y, clip_.w, clip_.h};
    }
    void drawRect(const Rect& rect, uint32_t rgba);
    void drawTriangle(float x1, float y1, float x2, float y2,
                      float x3, float y3,
                      uint32_t c1, uint32_t c2, uint32_t c3);
    /**
     * @brief 画一行文字（(x, y) = 行盒左上角，视图局部坐标）。
     * @param utf8 UTF-8 文本
     * @param font 首选字体；kFontAny 表示按注册顺序的回退链
     * @param x 行盒左上角 x（视图局部坐标）
     * @param y 行盒左上角 y（视图局部坐标）
     * @param sizePx 字号（em 像素大小）
     * @param rgba 文字颜色
     */
    void drawText(const char* utf8, int32_t font, float x, float y, float sizePx,
                  uint32_t rgba);
    /// 画线段（视图局部坐标，宽度按像素展开成四边形）。
    void drawLine(float x1, float y1, float x2, float y2, float width, uint32_t rgba);
    /// 填充矢量路径（路径坐标为视图局部坐标，贝塞尔自适应细分后三角化）。
    void drawPath(const Path& path, uint32_t rgba, float tolerance = 0.5f);
    /// 描边矢量路径（视图局部坐标，平端头线段拼接）。
    void strokePath(const Path& path, float width, uint32_t rgba,
                    float tolerance = 0.5f);
    /// 画实心圆（圆心为视图局部坐标）。
    void drawCircle(float cx, float cy, float radius, uint32_t rgba, int segments = 0);
    /// 画实心椭圆。
    void drawEllipse(float cx, float cy, float rx, float ry, uint32_t rgba,
                     int segments = 0);
    /// 画实心圆角矩形（视图局部坐标，整个视图区域）。
    void drawRoundRect(const Rect& rect, float radius, uint32_t rgba, int segments = 0);
    /// 画圆弧/环带（角度定义见 Canvas::drawArc）。
    void drawArc(float cx, float cy, float radius, float thickness,
                 float startAngle, float sweepAngle, uint32_t rgba, int segments = 0);
    /// 画完整圆环（圆心为视图局部坐标）。
    void drawRing(float cx, float cy, float radius, float thickness, uint32_t rgba) {
        drawArc(cx, cy, radius, thickness, 0.0f, 6.2831853f, rgba);
    }
    /// 画凸多边形（顶点数组 [x0,y0,x1,y1,...]，视图局部坐标）。
    void drawConvexPolygon(const float* points, int count, uint32_t rgba);
    /// 画矩形描边（线宽向内侧）。
    void strokeRect(const Rect& rect, float width, uint32_t rgba);
    /// 画圆角矩形描边。
    void strokeRoundRect(const Rect& rect, float radius, float width, uint32_t rgba,
                         int segments = 0);
    /// 画双色线性渐变矩形（horizontal = 左→右，否则上→下）。
    void drawRectGradient(const Rect& rect, uint32_t rgba0, uint32_t rgba1,
                          bool horizontal);
    /// 画一张 TextureStore 纹理（拉伸到目标矩形，rgba 作染色，0xFFFFFFFF 原样）。
    void drawImage(TextureId texture, const Rect& rect, uint32_t rgba = 0xFFFFFFFF);

private:
    Canvas& canvas_;
    Rect bounds_;
    Rect clip_;
};

class View;

class ViewRef {
public:
    ViewRef() = default;
    explicit ViewRef(View* view);

    View* get() const;
    explicit operator bool() const { return get() != nullptr; }

private:
    View* view_ = nullptr;
    std::weak_ptr<uint8_t> lifetime_;
};

/**
 * @brief 渲染节点：持有实际尺寸、父子关系与交互状态（Flutter 的
 *        RenderObject 对应物）。
 *
 * 布局走双向约束协议：父经 layout(BoxConstraints) 下行约束，子经
 * performLayout 自报尺寸上行，父再 setPosition 写偏移。rect 语义拆分：
 * x/y 由父写（偏移），w/h 由自身 layout 写（尺寸）。除布局入口外，
 * 重建/增删触发的重排统一走 markNeedsLayout + flushLayout。
 */
class View {
public:
    View();
    virtual ~View();

    View(const View&) = delete;
    View& operator=(const View&) = delete;

    Rect rect;
    bool visible = true;
    bool hasBackground = false;
    Color background;
    View* parent = nullptr;
    std::vector<std::unique_ptr<View>> children;
    float actualX = 0.0f;
    float actualY = 0.0f;

    std::function<void(PaintContext&)> painter;
    std::function<void(const ClickEvent&)> onClick;
    std::function<void(const PanEvent&)> onPan;

    virtual bool acceptsPointerInput() const { return false; }
    virtual bool acceptsPanInput() const { return static_cast<bool>(onPan); }
    virtual void handlePointer(const struct PointerEvent& event);
    virtual void handlePan(const PanEvent& event);
    virtual void handleChildRemoved(size_t) {}

    /**
     * @brief 布局协议下行入口：按约束完成本节点（及所需子树）的布局，
     *        回报钳制后的尺寸。
     *
     * 幂等：约束与上次相同且子树无脏标记时直接返回缓存尺寸，不递归。
     * 否则存下约束、调 performLayout（子类在此布局孩子并自报尺寸）、
     * 把回报值 constrain 进约束区间后写入 rect.w/h。帧构建（paint）
     * 期间拒绝调用（与 setBounds 同级守卫）。
     */
    Size layout(const BoxConstraints& constraints);

    /**
     * @brief 布局协议子类钩子：在约束内完成自身布局并回报尺寸。
     *
     * 容器在此逐个 child->layout(childConstraints) 拿孩子尺寸、再
     * child->setPosition(...) 写偏移；叶子按内容自测（Text 量文字）。
     * 默认实现 = constraints.biggest()：有界撑满、无界取 min（0）——
     * 与「裸容器被 tight 填满、无约束不占位」的旧表现一致。
     */
    virtual Size performLayout(const BoxConstraints& constraints);

    /// 父写孩子偏移（只动 rect.x/y，不触发布局；转场/滚动逐帧位移用它）。
    void setPosition(float x, float y);

    /// 标脏并向上冒泡到根：重建、孩子增删后由框架调用。
    void markNeedsLayout();
    /// 从最顶祖先以其缓存约束重排整棵脏子树；根尚无约束（首帧前）则留脏待落。
    void flushLayout();
    bool needsLayout() const { return layoutDirty_; }
    bool hasConstraints() const { return hasConstraints_; }
    /// 最近一次 layout 的下行约束（flushLayout/帧兜底重排的依据）。
    const BoxConstraints& constraints() const { return constraints_; }

    /// 兼容壳：定位 + tight 约束布局（等价于父给 (x,y) 落位并强制宽高）。
    void setBounds(float x, float y, float width, float height);
    void setVisible(bool value);
    void setBackground(uint32_t rgba);
    void clearBackground();

    View* addChild(std::unique_ptr<View> child);
    std::unique_ptr<View> removeChild(View* child);
    std::unique_ptr<View> replaceChild(View* oldChild, std::unique_ptr<View> replacement);

    void updateActuals();
    View* hitTest(float px, float py);
    Rect actualRect() const { return {actualX, actualY, rect.w, rect.h}; }
    bool containsVisiblePoint(float px, float py) const;
    bool isDescendantOf(const View* ancestor) const;
    ViewRef ref() { return ViewRef(this); }

    void paint(Canvas& canvas, const Rect& parentClip);

private:
    std::shared_ptr<uint8_t> lifetime_;
    BoxConstraints constraints_;   ///< 最近一次 layout 的下行约束
    bool hasConstraints_ = false;  ///< 是否完成过至少一次 layout
    bool layoutDirty_ = true;      ///< 脏标记：冒泡至根，flush 时清

    friend class ViewRef;
};

View* rootView();
void setRootView(View* view);
bool isBuildingFrame();
void buildFrame(Canvas& canvas);

} // namespace evk::ui
