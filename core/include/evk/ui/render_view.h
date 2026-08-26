#pragma once

#include "evk/ui/path.h"
#include "evk/ui/texture_store.h"

#include <cstddef>
#include <cstdint>
#include <functional>
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
    virtual void handleBoundsChanged() {}
    virtual void handleChildRemoved(size_t) {}

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

    friend class ViewRef;
};

View* rootView();
void setRootView(View* view);
bool isBuildingFrame();
void buildFrame(Canvas& canvas);

} // namespace evk::ui
