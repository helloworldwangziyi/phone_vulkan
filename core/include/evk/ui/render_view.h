#pragma once

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
    void drawRect(const Rect& rect, uint32_t rgba);
    void drawTriangle(float x1, float y1, float x2, float y2,
                      float x3, float y3,
                      uint32_t c1, uint32_t c2, uint32_t c3);
    /**
     * @brief 画一行文字（视图局部坐标 (0,0) = 行盒左上角）。
     * @param utf8 UTF-8 文本
     * @param font 首选字体；kFontAny 表示按注册顺序的回退链
     * @param sizePx 字号（像素高度）
     * @param rgba 文字颜色
     */
    void drawText(const char* utf8, int32_t font, float sizePx, uint32_t rgba);

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
