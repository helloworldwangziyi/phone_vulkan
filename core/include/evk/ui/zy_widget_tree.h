#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <typeinfo>
#include <utility>
#include <vector>

#include "evk/ui/controls/zy_button_control.h"
#include "evk/ui/zy_event_bus.h"
#include "evk/ui/layout/zy_flex_layout.h"
#include "evk/ui/zy_render_view.h"

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
    std::function<void()> onTap;
    std::function<void(PaintContext&)> painter;

    explicit Container(uint32_t color = 0);
    std::unique_ptr<View> createRenderObject() const override;
    void updateRenderObject(View& view) const override;
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
    std::unique_ptr<View> createRenderObject() const override;
    void updateRenderObject(View& view) const override;
    std::vector<std::unique_ptr<Widget>>& children() override { return children_; }
    void configureChild(View& parent, const Widget& child, View& childView) const override;

private:
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
std::unique_ptr<Widget> scrollView(
    std::unique_ptr<Widget> child,
    float contentHeight,
    std::function<void(float, float)> onScroll = {});

} // namespace evk::ui
