/**
 * @file widget_tree.cpp
 * @brief 四类 Element 与唯一对比规则（Element::updateChild）的实现。
 *
 * @details 头文件讲了 Widget / Element / View 三棵树的分工，这里是
 * Element 侧的实现。四类 Element 各司其职：
 *
 *   - **StatelessElement**：不产生 View；rebuild = 重新 build 出子
 *     Widget 后交给 updateChild 继续对比，renderObject() 透传孩子的 View；
 *   - **StatefulElement**：持有一个 State 对象。首次构建 = createState
 *     → attach → initState → build → didMount；导航事件先经它转发给
 *     State 的 hook，再下沉子树；
 *   - **RenderObjectElement**：唯一直接持有 View 的 Element——首次
 *     构建时 createRenderObject 造 View 并挂到 renderParent，同类型
 *     重建时 updateRenderObject 应用新参数，再逐子对比；
 *   - **ProxyElement**：不产生 View，只把子 Widget 转交给下一个 Element。
 *
 * 所有子位置的复用/重建判断都收敛到一条规则（Element::updateChild，
 * 见下方大段注释）：
 * @code
 *   新 Widget 为空            → 旧子树 unmount 销毁
 *   旧 Element 在且 canUpdate → update 就地微调：换 Widget 副本，状态保留
 *   其余情况                  → 旧子树 unmount，createElement → mount 重建
 * @endcode
 *
 * 一棵 Widget 树 mount 之后，Element 树与 View 树的关系：
 * @code
 *   Widget 树（每次重建全新）         Element 树（常驻）         View 树（常驻）
 *   Column ────────────→ RenderObjectElement ──→ FlexView
 *    ├─ Container              ├─ RenderObjectElement ──→ View
 *    └─ Expanded               │  （不产生 View）
 *         └─ ScrollViewWidget └─ RenderObjectElement ──→ ScrollView
 * @endcode
 */

#include "evk/ui/widget_tree.h"

#include <algorithm>
#include <cassert>
#include <utility>

#include "evk/frame_scheduler.h"
#include "evk/ui/controls/scroll_control.h"

namespace evk::ui {
namespace {

/**
 * @brief StatelessWidget 对应的 Element：不产生 View，rebuild = 重新
 *        build 出子 Widget，再交给 updateChild 对比下去。
 *
 * renderObject() 直接透传孩子的 View；导航事件也直接下沉（自己没有
 * State，不拦截）。
 */
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

/**
 * @brief StatefulWidget 对应的 Element：持有一个 State 对象，页面的
 *        可变状态都在它这里。
 *
 * 首次构建的生命周期（对照 Flutter 的 State）：
 *   createState（造 State）→ attach（State 绑定本 Element）→ initState
 *   （一次性初始化）→ build（生成子描述树）→ didMount（View 已挂树，
 *   此时才注册事件监听——HomePage 的 listen 就在 didMount 里）。
 *
 * unmount 时倒序：先拆子树 → dispose（取消在飞请求的最后时机）→
 * detach（State 与 Element 解绑）→ 销毁 State。
 *
 * 导航事件（RouteEvent）先翻译成 State 的 hook 调用：Will* 被 hook
 * 否决（返回 false）时向上返回 false，导航栈据此取消本次导航。
 */
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

/**
 * @brief RenderObjectWidget 对应的 Element：唯一直接持有 View 的
 *        Element，与 View 一一对应。
 *
 * 首次构建：createRenderObject 造 View → 挂到 renderParent（父 View）
 * 或暂存为游离 View（页面根，随后被导航栈接管）→ updateChildren 逐子
 * 对比构建。
 *
 * update（同类型重建）：updateRenderObject 把新参数应用到 View
 * （颜色、painter、样式……）→ updateChildren 逐子对比 → 通知 View
 * 「我的 bounds 变了」触发容器级联重排（FlexView 就是靠这个钩子布局）。
 *
 * unmount：先拆全部子 Element，再把 View 从父 View 上摘下销毁——
 * 顺序不能反，父 View 的孩子列表要在子 View 存活期间保持一致。
 */
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

/**
 * @brief ProxyWidget 对应的 Element：不产生 View，只把子 Widget 转交给
 *        下一个 Element。
 *
 * 对应 ProxyWidget（Expanded/Center）。它唯一的作用是在转交之前改一改
 * 子 Widget 的排布参数（flexParentData）；renderObject() 与导航事件
 * 都是透传。
 */
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

/**
 * @brief SizedBox 对应的 View：自己的 bounds 被（Flex 等）写入后，把
 *        孩子塞满。
 *
 * 注意这是布局链的典型形态：父布局只管写「这个节点多大」，节点内部
 * 如何分配由它自己的 handleBoundsChanged 决定——尺寸变化沿 View 树
 * 级联，App 全程不写布局函数。
 */
class SizedBoxView final : public View {
public:
    void handleBoundsChanged() override {
        if (!children.empty()) {
            children.front()->setBounds(0.0f, 0.0f, rect.w, rect.h);
        }
    }
};

/**
 * @brief Padding 对应的 View：孩子缩进 insets 后占据剩余空间。
 */
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

/**
 * @brief 触发重建：改状态 → build 出新描述 → 子树对比重建 → 标记重绘。
 *
 * 顺序是「先 mutate、再 build、再 rebuild」：子树重建发生在状态修改
 * 之后，因此 build() 里读到的状态一定是 mutate 之后的新状态。
 * mounted() == false（页面已 pop）的 setState 直接忽略——这是迟到
 * 数据的第一道防线（第二道是调用方自己检查 mounted/取消标志）。
 */
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

/**
 * @brief Flex 的构造：顺手算「固有主轴尺寸」（intrinsic main size）。
 *
 * 全部孩子都有固定主轴尺寸（mainSize≥0）时，把「尺寸 + 前后间距」累加
 * 成自己的固有尺寸，写进对外上报的排布参数（intrinsicData_）——这样
 * Column/Row 套在别的 Flex 里、或作为页面根时，父布局知道它
 * 「最小要多长」。有一个孩子是弹性的（flex>0）就放弃固有尺寸
 * （主尺寸为负 = 交给父布局）。
 */
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

/**
 * @brief Expanded/Center/SizedBox/Padding 共用的「参数改写」模式：
 *        构造时把孩子的 flexParentData 抄一份存着，被问时改几笔再上报。
 *
 * 这些转接件自己不产 View，却要参与父 Flex 的排布——靠的就是把孩子的
 * flexParentData 往上传时做修改：
 *   - Expanded：把 mainSize 置 -1、flex 置自己的系数（吃剩余空间）；
 *   - Center  ：把 crossAlignment 改成居中；
 *   - SizedBox：把主/交叉尺寸改成自己的固定值（变相固定孩子）；
 *   - Padding ：把内边距折算进孩子上报的尺寸。
 * 注意按轴向存两份（horizontal/vertical）：同一节点在两个方向的父容器里
 * 排布参数可以完全不同。
 */
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

/**
 * @brief 对比规则核心：全局唯一的「新旧对照」入口，相当于 Flutter 的
 *        Element::updateChild。
 *
 * @details 对一个子位置，对照新 Widget 与槽里的旧 Element：
 *
 * @code
 * 新 Widget 为空（这个子位置被撤了）？
 *   └─ 是 → 旧子树 unmount 销毁，槽位置空。
 * 旧 Element 在岗，且新旧 Widget「同一类型」（canUpdate）？
 *   └─ 是 → 旧 Element 就地复用：换掉 Widget 副本，updateElement 把
 *            新参数应用到现有结构。View 与其内部状态（滚动偏移、
 *            按压态……）保留。
 * 其余情况（类型变了 / 槽位本来就空）？
 *   └─ 旧子树先 unmount 销毁（拆 View、销毁 State），
 *      再按新 Widget createElement → mount 重建。
 * @endcode
 *
 * 这套规则是全机制的「守恒律」：
 *   - Widget 树怎么变都不直接操作 View——一切经对比后落成
 *     「微调」或「重建」二选一；
 *   - 滚动位置保留、列表数据到达自动增删行、主题切换只改色不重建，
 *     全是这条规则的自然推论，没有任何一处是特判。
 *
 * @param child        该位置的旧 Element 槽（按引用传入：可被替换成新 Element）
 * @param widget       新 Widget（unique_ptr：所有权交给 Element 持有）
 * @param renderParent 孩子的 View 应挂到的父 View
 */
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

/**
 * @brief 顶层入口：一张根 Widget → 一棵在岗的 Element 子树。
 *
 * 步骤：widget->createElement() 造根 Element → mount 绑定 Widget、
 * 挂靠点与导航器并标记 mounted → firstMount 完成首次构建。
 * 页面根（renderParent=nullptr）建出的 View 处于游离状态，由导航栈
 * 随后接管。
 */
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

// ============================================================================
// 小写工厂：build() 里的简写，一行造一个常用组件。
//
// 为什么有工厂还有类？类（Container/Button/……）适合直接 new 与继承扩展；
// 工厂是 build() 里的简写——组件树要一行行读下来像设计稿，而不是一摞
// new 表达式。HomePage 的 build 就是全部用工厂 + widgetList 拼的。
// ============================================================================

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
