#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <typeinfo>
#include <utility>
#include <vector>

#include "evk/ui/controls/button_control.h"
#include "evk/ui/event_bus.h"
#include "evk/ui/font_engine.h"
#include "evk/ui/layout/flex_layout.h"
#include "evk/ui/render_view.h"

namespace evk::ui {

class Element;
class Navigator;
class State;
class StatefulWidget;

class BuildContext {
public:
    virtual ~BuildContext() = default;
    virtual Navigator& navigator() const = 0;
    virtual View* renderObject() const = 0;
};

enum class RouteEvent {
    WillEnter,
    DidEnter,
    WillLeave,
    DidLeave,
};

class Widget {
public:
    virtual ~Widget() = default;

    Widget(const Widget&) = delete;
    Widget& operator=(const Widget&) = delete;
    Widget(Widget&&) = default;
    Widget& operator=(Widget&&) = default;

    virtual std::unique_ptr<Element> createElement() const = 0;
    virtual bool canUpdate(const Widget& other) const {
        return typeid(*this) == typeid(other);
    }
    virtual FlexParentData flexParentData(Axis axis) const;

protected:
    Widget() = default;
};

class StatelessWidget : public Widget {
public:
    std::unique_ptr<Element> createElement() const override;
    virtual std::unique_ptr<Widget> build(BuildContext& context) const = 0;
};

class StatefulWidget : public Widget {
public:
    std::unique_ptr<Element> createElement() const override;
    virtual std::unique_ptr<State> createState() const = 0;
};

class State {
public:
    virtual ~State() = default;

    virtual void initState() {}
    virtual void didMount() {}
    virtual void dispose() {}
    virtual std::unique_ptr<Widget> build(BuildContext& context) = 0;

    virtual bool onWillEnter(bool) { return true; }
    virtual void onDidEnter(bool) {}
    virtual bool onWillLeave(bool) { return true; }
    virtual void onDidLeave(bool) {}

    bool mounted() const;
    BuildContext& context() const;
    const StatefulWidget& widget() const;

    template <typename T>
    const T& widgetAs() const {
        return dynamic_cast<const T&>(widget());
    }

    void setState(std::function<void()> mutate = {});
    void listen(
        int32_t eventId,
        EventPriority priority,
        std::function<void(const void* data)> handler);

    void attach(Element* element);
    void detach();

private:
    Element* element_ = nullptr;
    std::vector<EventSubscription> subscriptions_;

};

class RenderObjectWidget : public Widget {
public:
    std::unique_ptr<Element> createElement() const override;

    virtual std::unique_ptr<View> createRenderObject() const = 0;
    virtual void updateRenderObject(View&) const {}
    virtual std::vector<std::unique_ptr<Widget>>& children();
    virtual View* childParent(View& view) const { return &view; }
    virtual void configureChild(View&, const Widget&, View&) const {}
};

class ProxyWidget : public Widget {
public:
    explicit ProxyWidget(std::unique_ptr<Widget> child);
    std::unique_ptr<Element> createElement() const override;
    std::unique_ptr<Widget>& child() { return child_; }
    const Widget& childWidget() const { return *child_; }

protected:
    std::unique_ptr<Widget> child_;
};

struct EdgeInsets {
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;

    static EdgeInsets all(float value);
    static EdgeInsets symmetric(float horizontal, float vertical);
    static EdgeInsets only(float left, float top, float right, float bottom);
};

class Container final : public RenderObjectWidget {
public:
    uint32_t color = 0;
    uint32_t borderColor = 0; ///< 描边色；非 0 且 borderWidth > 0 时生效
    float borderWidth = 0.0f; ///< 描边宽度（像素，向内侧）
    float cornerRadius = 0.0f; ///< 圆角半径；> 0 时背景/描边走圆角绘制
    std::function<void()> onTap;
    std::function<void(PaintContext&)> painter;

    explicit Container(uint32_t color = 0);
    std::unique_ptr<View> createRenderObject() const override;
    void updateRenderObject(View& view) const override;
};

/**
 * @brief 位图 Widget：把 TextureStore 纹理拉伸铺满自身边界。
 *
 * 无固有尺寸——在容器里跟随拉伸（配合 SizedBox/Expanded 控制大小）；
 * 顶点色恒白即原样贴图。要染色/九宫格时用 Container::painter 自绘。
 */
class ImageWidget final : public RenderObjectWidget {
public:
    explicit ImageWidget(TextureId texture);

    std::unique_ptr<View> createRenderObject() const override;
    void updateRenderObject(View& view) const override;

private:
    TextureId texture_; ///< TextureStore 句柄
};

class Button final : public RenderObjectWidget {
public:
    ButtonStyle style;
    std::function<void()> onPressed;
    bool enabled = true;

    Button(ButtonStyle style, std::function<void()> onPressed);
    std::unique_ptr<View> createRenderObject() const override;
    void updateRenderObject(View& view) const override;
};

class Flex : public RenderObjectWidget {
public:
    Axis axis;
    uint32_t color = 0;

    Flex(Axis axis, std::vector<std::unique_ptr<Widget>> children);
    FlexParentData flexParentData(Axis parentAxis) const override;
    std::unique_ptr<View> createRenderObject() const override;
    void updateRenderObject(View& view) const override;
    std::vector<std::unique_ptr<Widget>>& children() override { return children_; }
    void configureChild(View& parent, const Widget& child, View& childView) const override;

private:
    FlexParentData intrinsicData_;
    std::vector<std::unique_ptr<Widget>> children_;
};

class Column final : public Flex {
public:
    explicit Column(std::vector<std::unique_ptr<Widget>> children);
};

class Row final : public Flex {
public:
    explicit Row(std::vector<std::unique_ptr<Widget>> children);
};

class ScrollViewWidget final : public RenderObjectWidget {
public:
    float contentWidth = 0.0f;
    float contentHeight = 0.0f;
    std::function<void(float, float)> onScroll;

    ScrollViewWidget(
        std::unique_ptr<Widget> child,
        float contentHeight,
        std::function<void(float, float)> onScroll = {});

    std::unique_ptr<View> createRenderObject() const override;
    void updateRenderObject(View& view) const override;
    std::vector<std::unique_ptr<Widget>>& children() override { return children_; }
    View* childParent(View& view) const override;
    void configureChild(View& parent, const Widget& child, View& childView) const override;

private:
    std::vector<std::unique_ptr<Widget>> children_;
};

class Expanded final : public ProxyWidget {
public:
    Expanded(std::unique_ptr<Widget> child, float flex = 1.0f);
    FlexParentData flexParentData(Axis axis) const override;

private:
    float flex_;
    FlexParentData horizontalData_;
    FlexParentData verticalData_;
};

class SizedBox final : public RenderObjectWidget {
public:
    SizedBox(float width, float height, std::unique_ptr<Widget> child);

    FlexParentData flexParentData(Axis axis) const override;
    std::unique_ptr<View> createRenderObject() const override;
    void updateRenderObject(View& view) const override;
    std::vector<std::unique_ptr<Widget>>& children() override { return children_; }

private:
    float width_;
    float height_;
    FlexParentData horizontalData_;
    FlexParentData verticalData_;
    std::vector<std::unique_ptr<Widget>> children_;
};

class Padding final : public RenderObjectWidget {
public:
    Padding(EdgeInsets insets, std::unique_ptr<Widget> child);

    FlexParentData flexParentData(Axis axis) const override;
    std::unique_ptr<View> createRenderObject() const override;
    void updateRenderObject(View& view) const override;
    std::vector<std::unique_ptr<Widget>>& children() override { return children_; }

private:
    EdgeInsets insets_;
    FlexParentData horizontalData_;
    FlexParentData verticalData_;
    std::vector<std::unique_ptr<Widget>> children_;
};

class Center final : public ProxyWidget {
public:
    explicit Center(std::unique_ptr<Widget> child);
    FlexParentData flexParentData(Axis axis) const override;

private:
    FlexParentData horizontalData_;
    FlexParentData verticalData_;
};

/**
 * @brief 单行文本 Widget：内容、字号、颜色与首选字体。
 *
 * 尺寸由 FontEngine 测量得出（布局期间调用，不触发光栅化）：
 * 纵向容器里占一行高度、横向容器里占实测宽度；绘制在 painter 里经
 * PaintContext::drawText 完成，字形按需进 atlas。首选字体缺字时
 * 自动按注册顺序回退（如 Latin 字体 + CJK 字体混排一行）。
 */
class TextWidget final : public RenderObjectWidget {
public:
    TextWidget(std::string content, float fontSize, uint32_t color,
               FontId font = kFontAny);

    std::unique_ptr<View> createRenderObject() const override;
    void updateRenderObject(View& view) const override;
    FlexParentData flexParentData(Axis axis) const override;

private:
    std::string content_; ///< 文本内容（UTF-8）
    float fontSize_;      ///< 字号（像素高度）
    uint32_t color_;      ///< 文字颜色（RGBA）
    FontId font_;         ///< 首选字体；kFontAny = 按注册顺序回退
};

class Element : public BuildContext {
public:
    virtual ~Element();

    Element(const Element&) = delete;
    Element& operator=(const Element&) = delete;

    Navigator& navigator() const override;
    const Widget& widget() const { return *widget_; }
    bool mounted() const { return mounted_; }

    void mount(
        std::unique_ptr<Widget> widget,
        Element* parent,
        View* renderParent,
        Navigator* navigator);
    void update(std::unique_ptr<Widget> widget);
    virtual void rebuild() = 0;
    virtual void unmount();
    virtual bool dispatchRouteEvent(RouteEvent event, bool forward);

protected:
    Element() = default;

    virtual void firstMount() = 0;
    virtual void updateElement() = 0;
    void updateChild(
        std::unique_ptr<Element>& child,
        std::unique_ptr<Widget> widget,
        View* renderParent);

    std::unique_ptr<Widget> widget_;
    Element* parent_ = nullptr;
    View* renderParent_ = nullptr;
    Navigator* navigator_ = nullptr;
    bool mounted_ = false;
};

std::unique_ptr<Element> mountWidget(
    std::unique_ptr<Widget> widget,
    View* renderParent,
    Navigator* navigator,
    Element* parent = nullptr);

template <typename W, typename... Args>
std::unique_ptr<Widget> makeWidget(Args&&... args) {
    return std::make_unique<W>(std::forward<Args>(args)...);
}

template <typename... Widgets>
std::vector<std::unique_ptr<Widget>> widgetList(Widgets&&... widgets) {
    std::vector<std::unique_ptr<Widget>> result;
    result.reserve(sizeof...(widgets));
    (result.push_back(std::forward<Widgets>(widgets)), ...);
    return result;
}

template <typename... Widgets>
std::unique_ptr<Widget> column(Widgets&&... widgets) {
    return makeWidget<Column>(widgetList(std::forward<Widgets>(widgets)...));
}

inline std::unique_ptr<Widget> column(std::vector<std::unique_ptr<Widget>> widgets) {
    return makeWidget<Column>(std::move(widgets));
}

template <typename... Widgets>
std::unique_ptr<Widget> row(Widgets&&... widgets) {
    return makeWidget<Row>(widgetList(std::forward<Widgets>(widgets)...));
}

inline std::unique_ptr<Widget> row(std::vector<std::unique_ptr<Widget>> widgets) {
    return makeWidget<Row>(std::move(widgets));
}

std::unique_ptr<Widget> container(
    uint32_t color = 0,
    std::function<void()> onTap = {},
    std::function<void(PaintContext&)> painter = {});
std::unique_ptr<Widget> button(
    ButtonStyle style,
    std::function<void()> onPressed);
std::unique_ptr<Widget> expanded(
    std::unique_ptr<Widget> child,
    float flex = 1.0f);
std::unique_ptr<Widget> sizedBox(
    float width,
    float height,
    std::unique_ptr<Widget> child);
std::unique_ptr<Widget> padding(
    EdgeInsets insets,
    std::unique_ptr<Widget> child);
std::unique_ptr<Widget> center(std::unique_ptr<Widget> child);
std::unique_ptr<Widget> image(TextureId texture);
std::unique_ptr<Widget> text(std::string content, float fontSize, uint32_t color,
                             FontId font = kFontAny);
std::unique_ptr<Widget> scrollView(
    std::unique_ptr<Widget> child,
    float contentHeight,
    std::function<void(float, float)> onScroll = {});

} // namespace evk::ui
