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

#include <cassert>
#include <utility>

#include "evk/frame_scheduler.h"

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
 * （颜色、painter、样式……）→ updateChildren 逐子对比 → 标脏并从根
 * 重排（约束协议：父下行约束、子上行尺寸，FlexView 靠它布局）。
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
        // 重建完成：标脏（冒泡至根）并立即从根重排——子树尺寸变化经
        // 约束协议自然上行（如文字变长撑高行、撑开后续兄弟）。
        view_->markNeedsLayout();
        view_->flushLayout();
    }

    View* view_ = nullptr;
    std::unique_ptr<View> detachedView_;
    std::vector<std::unique_ptr<Element>> children_;
};

/**
 * @brief ProxyWidget 对应的 Element：不产生 View，只把子 Widget 转交给
 *        下一个 Element。
 *
 * 对应 ProxyWidget（Expanded）。它唯一的作用是在转交之前改一改
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

} // namespace evk::ui
