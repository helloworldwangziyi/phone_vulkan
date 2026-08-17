/**
 * @file widget_tree.cpp
 * @brief 工地的施工队：四种工位（Element）与交接算法（updateChild）的实现。
 *
 * @details 头文件讲了「图纸 ↔ 工地」的比喻，这里是把比喻变成代码的地方。
 * 工地上一共四类工人，各干各的活：
 *
 *   - **StatelessElement**（无记忆分包工）：每次 rebuild 重画子图纸，
 *     自己没有实体，report 实体时直接报孩子的那件；
 *   - **StatefulElement**（带记事本的工头）：手上一本 State 记事本，
 *     首次施工 = 领记事本（createState）→ 初始化（initState）→ 按记事本
 *     画图纸（build）→ 报完工（didMount）；导航旅程事件全部先经他手
 *     转发给记事本的 hook；
 *   - **RenderObjectElement**（泥瓦匠）：唯一真正砌墙的工人——
 *     首次施工按图纸 createRenderObject 造实体并挂到 renderParent，
 *     换图纸时 updateRenderObject 微调实体，再逐子交接；
 *   - **ProxyElement**（包工头）：不砌墙，只把子图纸转手给下一个工人。
 *
 * 全工地只有一条交接规则（Element::updateChild，见下方大段注释）：
 * @code
 *   新图纸为空        → 撤掉孩子工位
 *   旧工位在且 canUpdate → 老工人接新图纸，就地微调（update → updateElement）
 *   其余情况          → 拆旧工位（unmount），新工人持新图纸开工（createElement → mount）
 * @endcode
 *
 * 一棵描述树 mount 之后，工位树与实体树的关系：
 * @code
 *   描述树（每帧全新）        工位树（存活）           实体树（存活）
 *   Column ────────────→ RenderObjectElement ──→ FlexView
 *    ├─ Container           ├─ RenderObjectElement ──→ View
 *    └─ Expanded            │  （不产实体）
 *        └─ ScrollViewWidget └─ RenderObjectElement ──→ ScrollView
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
 * @brief 无记忆分包工：自己不砌墙，只反复「重画子图纸 → 交接给子工位」。
 *
 * 对应 StatelessWidget。实体报告直接透传孩子的实体；导航事件也一路
 * 下沉（自己无记事本，无权拦截）。
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
 * @brief 带记事本的工头：手上握着 State，页面的一切可变状态都在他这里。
 *
 * 首次施工的仪式（对照 Flutter 的 State 生命周期）：
 *   createState（领新记事本）→ attach（记事本写上工位地址）→ initState
 *   （一次性初始化）→ build（按记事本画图纸）→ didMount（实体已落位，
 *   此时才注册事件监听——HomePage 的 listen 就在 didMount 里）。
 *
 * 拆除时倒序：拆子工位 → dispose（最后救生索，取消在飞请求）→
 * detach（撕掉记事本与工位的绑定）→ 撕记事本。
 *
 * 导航旅程（RouteEvent）到 State hook 的翻译官：Will* 被记事本否决时
 * 向上返回 false，导航栈据此取消本次导航。
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
 * @brief 泥瓦匠：唯一真正砌墙的工人，工位与实体（View）一一对应。
 *
 * 首次施工：createRenderObject 造实体 → 挂到 renderParent（父实体）或
 * 暂存为游离实体（页面根，随后被导航栈接管）→ updateChildren 逐子开工。
 *
 * 换图纸（updateElement）：updateRenderObject 微调实体本身
 * （颜色、painter、样式……）→ updateChildren 逐子交接 → 通知实体
 * 「我的 bounds 变了」触发容器级联重排（FlexView 就是靠这个钩子布局）。
 *
 * 拆除：先拆全部子工位，再把实体从父实体上摘下来销毁——顺序不能反，
 * 父实体的孩子列表要在子实体存活期间保持一致。
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
 * @brief 包工头：不砌墙、无实体，只把一张子图纸转手给下一道工序的工人。
 *
 * 对应 ProxyWidget（Expanded/Center）。他的全部价值在「转手之前改一改
 * 孩子图纸上的便签（flexParentData）」，实体报告与导航事件都是透传。
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
 * @brief SizedBox 的实体：自己的 bounds 被（Flex 等）写入后，把孩子塞满。
 *
 * 注意这是布局链的典型形态：父布局只管写「我这个盒子多大」，盒内如何
 * 分配是实体自己 handleBoundsChanged 里的事——尺寸变化沿树级联，
 * App 全程不写 layout 函数。
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
 * @brief Padding 的实体：孩子缩进 insets 后占据剩余空间。
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
 * @brief 翻新入口：记事本改一笔 → 设计师重画图纸 → 工位微调 → 请求渲染。
 *
 * 顺序是「先 mutate、再 build、再 rebuild」：工位重画发生在实体修改之前，
 * 因此 build() 里读到的状态一定是 mutate 之后的新状态。未在岗（页面已
 * pop、mounted() == false）的 setState 直接忽略——这是迟到数据的第一道
 * 防线（第二道是调用方自己检查 mounted/取消标志）。
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
 * @brief Flex 图纸的构造：顺手算「固有主轴尺寸」（intrinsic main size）。
 *
 * 全部孩子都有固定主轴尺寸（mainSize≥0）时，把「尺寸 + 前后间距」累加
 * 成自己的固有尺寸，写进对外便签（intrinsicData_）——这样 Column/Row
 * 套在别的 Flex 里、或作为页面根时，父布局知道它「最小要多长」。
 * 有一个孩子是弹性的（flex>0）就放弃固有尺寸（主尺寸为负 = 交给父布局）。
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
 * @brief Expanded/Center/SizedBox/Padding 共用的「便签接力」模式：
 *        构造时把孩子对外报的便签抄一份存着，回答时改几笔再上报。
 *
 * 这些转接件自己不产实体，却要参与父 Flex 的排布——靠的就是把孩子的
 * flexParentData 往上「接力」时做手脚：
 *   - Expanded：把 mainSize 置 -1、flex 置自己的系数（吃剩余空间）；
 *   - Center  ：把 crossAlignment 改成正中；
 *   - SizedBox：把主/交叉尺寸改成自己的固定值（变相固定孩子）；
 *   - Padding ：把内边距折算进孩子报的尺寸。
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
 * @brief 交接算法核心：全工地唯一的「新旧对照」规则，相当于 Flutter 的
 *        Element::updateChild。
 *
 * @details 把一次交接想成包工头站在工位槽前，手里拿着新图纸：
 *
 * @code
 * 新图纸是空（这张子图纸被撤了）？
 *   └─ 是 → 老工人收工走人（unmount），工位槽清空。
 * 老工人在岗，且老图纸与新图纸「同一类构件」（canUpdate）？
 *   └─ 是 → 老工人接过新图纸，就地微调（update → updateElement）。
 *            实体保留、内部状态（滚动 offset、pressed……）保留。
 * 其余情况（新构件类型变了 / 工位本来就空）？
 *   └─ 老工人先收工（unmount，拆实体、撕记事本），
 *      再按新图纸派新工人开工（createElement → mount）。
 * @endcode
 *
 * 这套规则是全机制的「守恒律」：
 *   - 描述树怎么变都不直接操作实体——一切经工位对照后落成
 *     「微调」或「重建」二选一；
 *   - 滚动位置保留、列表数据到达自动增删行、主题切换只改色不重建，
 *     全是这条规则的自然推论，没有任何一处是特判。
 *
 * @param child        待交接的工位槽（按引用传入：可被替换成新工位）
 * @param widget       新子图纸（unique_ptr：交接后图纸所有权归工位）
 * @param renderParent 孩子的实体应挂靠的父实体
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
 * @brief 开工入口：一张根图纸 → 一个在岗工位（mountWidget）。
 *
 * 步骤：按图纸的派遣令 createElement 派出工人 → mount 把图纸、挂靠点、
 * 导航器交到他手上并 marked 在岗 → firstMount 完成首次施工。
 * 页面根（renderParent=nullptr）建出的实体处于游离状态，由导航栈随后接管。
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
// 小写工厂：一行画一张常用图纸。
//
// 为什么有工厂还有类？类（Container/Button/……）适合直接 new 与继承扩展；
// 工厂是「build() 里的简写」——page 树要一行行读下来像设计稿，而不是
// 一摞 new 表达式。HomePage 的 build 就是全部用工厂 + widgetList 拼的。
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
