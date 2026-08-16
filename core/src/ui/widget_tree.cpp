#include "evk/ui/widget_tree.h"

#include <algorithm>
#include <cassert>
#include <utility>

#include "evk/frame_scheduler.h"
#include "evk/ui/controls/scroll_control.h"

namespace evk::ui {
namespace {

class StatelessElement final : public Element {
public:
    View* renderObject() const override {
        return child_ ? child_->renderObject() : nullptr;
    }

    void rebuild() override {
        const auto& config = dynamic_cast<const StatelessWidget&>(*widget_);
        updateChild(child_, config.build(*this), renderParent_);
    }

    void unmount() override {
        if (child_) {
            child_->unmount();
            child_.reset();
        }
        Element::unmount();
    }

    bool dispatchRouteEvent(RouteEvent event, bool forward) override {
        return !child_ || child_->dispatchRouteEvent(event, forward);
    }

protected:
    void firstMount() override { rebuild(); }
    void updateElement() override { rebuild(); }

private:
    std::unique_ptr<Element> child_;
};

class StatefulElement final : public Element {
public:
    View* renderObject() const override {
        return child_ ? child_->renderObject() : nullptr;
    }

    void rebuild() override {
        if (!state_) {
            return;
        }
        updateChild(child_, state_->build(*this), renderParent_);
    }

    void unmount() override {
        if (child_) {
            child_->unmount();
            child_.reset();
        }
        if (state_) {
            state_->dispose();
            state_->detach();
            state_.reset();
        }
        Element::unmount();
    }

    bool dispatchRouteEvent(RouteEvent event, bool forward) override {
        if (!state_) {
            return true;
        }
        switch (event) {
            case RouteEvent::WillEnter:
                if (!state_->onWillEnter(forward)) {
                    return false;
                }
                break;
            case RouteEvent::DidEnter:
                state_->onDidEnter(forward);
                break;
            case RouteEvent::WillLeave:
                if (!state_->onWillLeave(forward)) {
                    return false;
                }
                break;
            case RouteEvent::DidLeave:
                state_->onDidLeave(forward);
                break;
        }
        return !child_ || child_->dispatchRouteEvent(event, forward);
    }

protected:
    void firstMount() override {
        const auto& config = dynamic_cast<const StatefulWidget&>(*widget_);
        state_ = config.createState();
        assert(state_);
        state_->attach(this);
        state_->initState();
        rebuild();
        state_->didMount();
    }

    void updateElement() override {
        rebuild();
    }

private:
    std::unique_ptr<State> state_;
    std::unique_ptr<Element> child_;
};

class RenderObjectElement final : public Element {
public:
    View* renderObject() const override { return view_; }

    void rebuild() override {
        updateElement();
    }

    void unmount() override {
        for (auto& child : children_) {
            if (child) {
                child->unmount();
            }
        }
        children_.clear();

        if (view_) {
            if (view_->parent) {
                std::unique_ptr<View> removed = view_->parent->removeChild(view_);
                removed.reset();
            } else {
                detachedView_.reset();
            }
            view_ = nullptr;
        }
        Element::unmount();
    }

    bool dispatchRouteEvent(RouteEvent event, bool forward) override {
        for (auto& child : children_) {
            if (child && !child->dispatchRouteEvent(event, forward)) {
                return false;
            }
        }
        return true;
    }

protected:
    void firstMount() override {
        const auto& config = dynamic_cast<const RenderObjectWidget&>(*widget_);
        std::unique_ptr<View> created = config.createRenderObject();
        if (renderParent_) {
            view_ = renderParent_->addChild(std::move(created));
        } else {
            detachedView_ = std::move(created);
            view_ = detachedView_.get();
        }
        updateChildren(config);
    }

    void updateElement() override {
        const auto& config = dynamic_cast<const RenderObjectWidget&>(*widget_);
        config.updateRenderObject(*view_);
        updateChildren(config);
    }

private:
    void updateChildren(const RenderObjectWidget& config) {
        View* childParent = config.childParent(*view_);
        auto& specs = const_cast<RenderObjectWidget&>(config).children();
        const size_t newCount = specs.size();
        if (children_.size() < newCount) {
            children_.resize(newCount);
        }
        for (size_t i = 0; i < newCount; ++i) {
            updateChild(children_[i], std::move(specs[i]), childParent);
            if (children_[i] && children_[i]->renderObject()) {
                config.configureChild(
                    *view_, children_[i]->widget(), *children_[i]->renderObject());
            }
        }
        for (size_t i = newCount; i < children_.size(); ++i) {
            if (children_[i]) {
                children_[i]->unmount();
            }
        }
        children_.resize(newCount);
        view_->handleBoundsChanged();
    }

    View* view_ = nullptr;
    std::unique_ptr<View> detachedView_;
    std::vector<std::unique_ptr<Element>> children_;
};

class ProxyElement final : public Element {
public:
    View* renderObject() const override {
        return child_ ? child_->renderObject() : nullptr;
    }

    void rebuild() override {
        auto& config = dynamic_cast<ProxyWidget&>(*widget_);
        updateChild(child_, std::move(config.child()), renderParent_);
    }

    void unmount() override {
        if (child_) {
            child_->unmount();
            child_.reset();
        }
        Element::unmount();
    }

    bool dispatchRouteEvent(RouteEvent event, bool forward) override {
        return !child_ || child_->dispatchRouteEvent(event, forward);
    }

protected:
    void firstMount() override { rebuild(); }
    void updateElement() override { rebuild(); }

private:
    std::unique_ptr<Element> child_;
};

class SizedBoxView final : public View {
public:
    void handleBoundsChanged() override {
        if (!children.empty()) {
            children.front()->setBounds(0.0f, 0.0f, rect.w, rect.h);
        }
    }
};

class PaddingView final : public View {
public:
    EdgeInsets insets;

    void handleBoundsChanged() override {
        if (children.empty()) {
            return;
        }
        children.front()->setBounds(
            insets.left,
            insets.top,
            std::max(0.0f, rect.w - insets.left - insets.right),
            std::max(0.0f, rect.h - insets.top - insets.bottom));
    }
};

FlexParentData childDataOf(const std::unique_ptr<Widget>& child, Axis axis) {
    return child ? child->flexParentData(axis) : FlexParentData{};
}

} // namespace

FlexParentData Widget::flexParentData(Axis) const {
    return {};
}

std::unique_ptr<Element> StatelessWidget::createElement() const {
    return std::make_unique<StatelessElement>();
}

std::unique_ptr<Element> StatefulWidget::createElement() const {
    return std::make_unique<StatefulElement>();
}

bool State::mounted() const {
    return element_ && element_->mounted();
}

BuildContext& State::context() const {
    assert(element_);
    return *element_;
}

const StatefulWidget& State::widget() const {
    assert(element_);
    return dynamic_cast<const StatefulWidget&>(element_->widget());
}

void State::setState(std::function<void()> mutate) {
    if (!mounted()) {
        return;
    }
    if (mutate) {
        mutate();
    }
    element_->rebuild();
    requestRender();
}

void State::listen(
    int32_t eventId,
    EventPriority priority,
    std::function<void(const void*)> handler) {
    assert(mounted());
    subscriptions_.push_back(EventBus::instance().subscribe(
        eventId,
        priority,
        element_->renderObject(),
        [handler = std::move(handler)](const void* data) {
            handler(data);
            return false;
        }));
}

void State::attach(Element* element) {
    element_ = element;
}

void State::detach() {
    subscriptions_.clear();
    element_ = nullptr;
}

std::unique_ptr<Element> RenderObjectWidget::createElement() const {
    return std::make_unique<RenderObjectElement>();
}

std::vector<std::unique_ptr<Widget>>& RenderObjectWidget::children() {
    static std::vector<std::unique_ptr<Widget>> empty;
    return empty;
}

ProxyWidget::ProxyWidget(std::unique_ptr<Widget> child)
    : child_(std::move(child)) {}

std::unique_ptr<Element> ProxyWidget::createElement() const {
    return std::make_unique<ProxyElement>();
}

EdgeInsets EdgeInsets::all(float value) {
    return {value, value, value, value};
}

EdgeInsets EdgeInsets::symmetric(float horizontal, float vertical) {
    return {horizontal, vertical, horizontal, vertical};
}

EdgeInsets EdgeInsets::only(float left, float top, float right, float bottom) {
    return {left, top, right, bottom};
}

Container::Container(uint32_t colorValue) : color(colorValue) {}

std::unique_ptr<View> Container::createRenderObject() const {
    auto view = std::make_unique<View>();
    updateRenderObject(*view);
    return view;
}

void Container::updateRenderObject(View& view) const {
    // 圆角/描边装饰：背景不再是整块矩形，改由 painter 按视图实际尺寸绘制；
    // 用户 painter（若给了）在装饰之后执行，两者可以叠加。
    const bool decorated =
        cornerRadius > 0.0f || (borderWidth > 0.0f && borderColor != 0);
    if (decorated) {
        view.clearBackground();
    } else if (color == 0) {
        view.clearBackground();
    } else {
        view.setBackground(color);
    }
    view.onClick = onTap
        ? [callback = onTap](const ClickEvent&) { callback(); }
        : std::function<void(const ClickEvent&)>{};
    if (decorated) {
        view.painter = [fill = color, radius = cornerRadius, border = borderColor,
                        borderWidth = borderWidth, user = painter](PaintContext& paint) {
            const Size size = paint.size();
            const Rect bounds = {0.0f, 0.0f, size.width, size.height};
            if (fill != 0) {
                paint.drawRoundRect(bounds, radius, fill);
            }
            if (borderWidth > 0.0f && border != 0) {
                paint.strokeRoundRect(bounds, radius, borderWidth, border);
            }
            if (user) {
                user(paint);
            }
        };
    } else {
        view.painter = painter;
    }
}

ImageWidget::ImageWidget(TextureId texture) : texture_(texture) {}

std::unique_ptr<View> ImageWidget::createRenderObject() const {
    auto view = std::make_unique<View>();
    updateRenderObject(*view);
    return view;
}

void ImageWidget::updateRenderObject(View& view) const {
    view.clearBackground();
    view.painter = [texture = texture_](PaintContext& paint) {
        const Size size = paint.size();
        paint.drawImage(texture, {0.0f, 0.0f, size.width, size.height});
    };
}

Button::Button(ButtonStyle value, std::function<void()> callback)
    : style(value), onPressed(std::move(callback)) {}

std::unique_ptr<View> Button::createRenderObject() const {
    return createButtonView(style, onPressed);
}

void Button::updateRenderObject(View& view) const {
    updateButtonView(view, style, onPressed, enabled);
}

Flex::Flex(Axis direction, std::vector<std::unique_ptr<Widget>> childWidgets)
    : axis(direction), children_(std::move(childWidgets)) {
    float intrinsicMainSize = 0.0f;
    for (const auto& child : children_) {
        if (!child) {
            continue;
        }
        const FlexParentData childData = child->flexParentData(axis);
        if (childData.mainSize < 0.0f) {
            return;
        }
        intrinsicMainSize += childData.before + childData.mainSize + childData.after;
    }
    intrinsicData_.mainSize = intrinsicMainSize;
}

FlexParentData Flex::flexParentData(Axis parentAxis) const {
    return parentAxis == axis ? intrinsicData_ : FlexParentData{};
}

std::unique_ptr<View> Flex::createRenderObject() const {
    auto view = createFlexView(axis);
    updateRenderObject(*view);
    return view;
}

void Flex::updateRenderObject(View& view) const {
    if (color == 0) {
        view.clearBackground();
    } else {
        view.setBackground(color);
    }
}

void Flex::configureChild(View& parent, const Widget& child, View& childView) const {
    setFlexChild(parent, childView, child.flexParentData(axis));
}

Column::Column(std::vector<std::unique_ptr<Widget>> childWidgets)
    : Flex(Axis::Vertical, std::move(childWidgets)) {}

Row::Row(std::vector<std::unique_ptr<Widget>> childWidgets)
    : Flex(Axis::Horizontal, std::move(childWidgets)) {}

ScrollViewWidget::ScrollViewWidget(
    std::unique_ptr<Widget> child,
    float height,
    std::function<void(float, float)> callback)
    : contentHeight(height), onScroll(std::move(callback)) {
    children_.push_back(std::move(child));
}

std::unique_ptr<View> ScrollViewWidget::createRenderObject() const {
    return createScrollView(contentWidth, contentHeight, onScroll);
}

void ScrollViewWidget::updateRenderObject(View& view) const {
    updateScrollView(view, contentWidth, contentHeight, onScroll);
}

View* ScrollViewWidget::childParent(View& view) const {
    return scrollContent(view);
}

void ScrollViewWidget::configureChild(
    View& parent, const Widget&, View& childView) const {
    View* content = scrollContent(parent);
    if (content) {
        childView.setBounds(0.0f, 0.0f, content->rect.w, content->rect.h);
    }
}

Expanded::Expanded(std::unique_ptr<Widget> child, float flex)
    : ProxyWidget(std::move(child)), flex_(flex) {
    horizontalData_ = child_ ? child_->flexParentData(Axis::Horizontal)
                             : FlexParentData{};
    verticalData_ = child_ ? child_->flexParentData(Axis::Vertical)
                           : FlexParentData{};
}

FlexParentData Expanded::flexParentData(Axis axis) const {
    FlexParentData data = axis == Axis::Vertical ? verticalData_ : horizontalData_;
    data.mainSize = -1.0f;
    data.flex = std::max(0.0f, flex_);
    return data;
}

SizedBox::SizedBox(float width, float height, std::unique_ptr<Widget> child)
    : width_(width),
      height_(height),
      horizontalData_(childDataOf(child, Axis::Horizontal)),
      verticalData_(childDataOf(child, Axis::Vertical)) {
    children_.push_back(std::move(child));
}

FlexParentData SizedBox::flexParentData(Axis axis) const {
    FlexParentData data = axis == Axis::Vertical ? verticalData_ : horizontalData_;
    data.mainSize = axis == Axis::Vertical ? height_ : width_;
    data.crossSize = axis == Axis::Vertical ? width_ : height_;
    data.flex = 0.0f;
    return data;
}

std::unique_ptr<View> SizedBox::createRenderObject() const {
    return std::make_unique<SizedBoxView>();
}

void SizedBox::updateRenderObject(View&) const {}

Padding::Padding(EdgeInsets insets, std::unique_ptr<Widget> child)
    : insets_(insets),
      horizontalData_(childDataOf(child, Axis::Horizontal)),
      verticalData_(childDataOf(child, Axis::Vertical)) {
    children_.push_back(std::move(child));
}

FlexParentData Padding::flexParentData(Axis axis) const {
    FlexParentData data = axis == Axis::Vertical ? verticalData_ : horizontalData_;
    const float mainPadding = axis == Axis::Vertical
        ? insets_.top + insets_.bottom
        : insets_.left + insets_.right;
    const float crossPadding = axis == Axis::Vertical
        ? insets_.left + insets_.right
        : insets_.top + insets_.bottom;
    if (data.mainSize >= 0.0f) {
        data.mainSize += mainPadding;
    }
    if (data.crossSize >= 0.0f) {
        data.crossSize += crossPadding;
    }
    return data;
}

std::unique_ptr<View> Padding::createRenderObject() const {
    auto view = std::make_unique<PaddingView>();
    view->insets = insets_;
    return view;
}

void Padding::updateRenderObject(View& view) const {
    if (auto* paddingView = dynamic_cast<PaddingView*>(&view)) {
        paddingView->insets = insets_;
        paddingView->handleBoundsChanged();
    }
}

Center::Center(std::unique_ptr<Widget> child)
    : ProxyWidget(std::move(child)) {
    horizontalData_ = child_ ? child_->flexParentData(Axis::Horizontal)
                             : FlexParentData{};
    verticalData_ = child_ ? child_->flexParentData(Axis::Vertical)
                           : FlexParentData{};
}

FlexParentData Center::flexParentData(Axis axis) const {
    FlexParentData data = axis == Axis::Vertical ? verticalData_ : horizontalData_;
    data.crossAlignment = CrossAxisAlignment::Center;
    return data;
}

Element::~Element() = default;

Navigator& Element::navigator() const {
    assert(navigator_);
    return *navigator_;
}

void Element::mount(
    std::unique_ptr<Widget> widget,
    Element* parent,
    View* renderParent,
    Navigator* navigatorValue) {
    widget_ = std::move(widget);
    parent_ = parent;
    renderParent_ = renderParent;
    navigator_ = navigatorValue;
    mounted_ = true;
    firstMount();
}

void Element::update(std::unique_ptr<Widget> widget) {
    widget_ = std::move(widget);
    updateElement();
}

void Element::unmount() {
    mounted_ = false;
    widget_.reset();
    parent_ = nullptr;
    renderParent_ = nullptr;
    navigator_ = nullptr;
}

bool Element::dispatchRouteEvent(RouteEvent, bool) {
    return true;
}

void Element::updateChild(
    std::unique_ptr<Element>& child,
    std::unique_ptr<Widget> widget,
    View* renderParent) {
    if (!widget) {
        if (child) {
            child->unmount();
            child.reset();
        }
        return;
    }
    if (child && child->widget().canUpdate(*widget)) {
        child->update(std::move(widget));
        return;
    }
    if (child) {
        child->unmount();
        child.reset();
    }
    child = widget->createElement();
    child->mount(std::move(widget), this, renderParent, navigator_);
}

std::unique_ptr<Element> mountWidget(
    std::unique_ptr<Widget> widget,
    View* renderParent,
    Navigator* navigator,
    Element* parent) {
    if (!widget) {
        return nullptr;
    }
    std::unique_ptr<Element> element = widget->createElement();
    element->mount(std::move(widget), parent, renderParent, navigator);
    return element;
}

std::unique_ptr<Widget> container(
    uint32_t color,
    std::function<void()> onTap,
    std::function<void(PaintContext&)> painter) {
    auto result = makeWidget<Container>(color);
    auto* config = static_cast<Container*>(result.get());
    config->onTap = std::move(onTap);
    config->painter = std::move(painter);
    return result;
}

std::unique_ptr<Widget> button(
    ButtonStyle style,
    std::function<void()> onPressed) {
    return makeWidget<Button>(style, std::move(onPressed));
}

std::unique_ptr<Widget> expanded(std::unique_ptr<Widget> child, float flex) {
    return makeWidget<Expanded>(std::move(child), flex);
}

std::unique_ptr<Widget> sizedBox(
    float width, float height, std::unique_ptr<Widget> child) {
    return makeWidget<SizedBox>(width, height, std::move(child));
}

std::unique_ptr<Widget> padding(
    EdgeInsets insets, std::unique_ptr<Widget> child) {
    return makeWidget<Padding>(insets, std::move(child));
}

TextWidget::TextWidget(std::string content, float fontSize, uint32_t color,
                       FontId font)
    : content_(std::move(content)), fontSize_(fontSize), color_(color), font_(font) {}

std::unique_ptr<View> TextWidget::createRenderObject() const {
    auto view = std::make_unique<View>();
    updateRenderObject(*view);
    return view;
}

void TextWidget::updateRenderObject(View& view) const {
    // 文本视图无背景无手势，只有一个 painter：内容/字号/颜色变了就整体换闭包。
    view.clearBackground();
    view.painter = [content = content_, font = font_, size = fontSize_,
                    color = color_](PaintContext& paint) {
        paint.drawText(content.c_str(), font, 0.0f, 0.0f, size, color);
    };
}

FlexParentData TextWidget::flexParentData(Axis axis) const {
    // 主轴尺寸 = 测量值（纵向容器报行高、横向容器报行宽）；
    // 交叉轴不约束，沿用容器的对齐/拉伸规则。
    FlexParentData data;
    float width = 0.0f;
    float height = 0.0f;
    FontEngine::instance().measureText(content_.c_str(), fontSize_, font_,
                                       &width, &height);
    data.mainSize = axis == Axis::Vertical ? height : width;
    return data;
}

std::unique_ptr<Widget> image(TextureId texture) {
    return makeWidget<ImageWidget>(texture);
}

std::unique_ptr<Widget> center(std::unique_ptr<Widget> child) {
    return makeWidget<Center>(std::move(child));
}
std::unique_ptr<Widget> text(std::string content, float fontSize, uint32_t color,
                             FontId font) {
    return makeWidget<TextWidget>(std::move(content), fontSize, color, font);
}


std::unique_ptr<Widget> scrollView(
    std::unique_ptr<Widget> child,
    float contentHeight,
    std::function<void(float, float)> onScroll) {
    return makeWidget<ScrollViewWidget>(
        std::move(child), contentHeight, std::move(onScroll));
}

} // namespace evk::ui
