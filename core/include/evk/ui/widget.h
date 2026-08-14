#pragma once

// 声明式 UI 层（仿 Flutter）：页面 = Component 的 build() 返回 Widget 描述树，
// reconcile 把描述差异应用到 retained 视图树（同类型就地更新，内部状态如
// 滚动 offset 保留；不同类型销毁重建）；setState 触发局部重建。
//
// 页面写法（结构即代码，所见即所得）：
//   class RankPage : public evk::ui::Component {
//       std::vector<RankItem> items_;                    // 状态：页面数据
//       std::unique_ptr<Widget> build() override {       // 创建了什么、挂在哪，一眼可见
//           auto topBar = Box(theme().navBar).mainSize(dp(150));
//           auto list   = column(...数据行...).flex(1);
//           return makeWidget(column(std::move(topBar), std::move(list)));
//       }
//       void onDidEnter(bool) override { requestQuote(); }   // 进入请求
//       void onWillLeave(bool) override { cancelQuote(); }   // 离开取消（返回 false 可拦截）
//   };
//   pushPage(nav, std::make_unique<RankPage>(), true);
//
// 说明：
// - build 根 widget 类型必须稳定（Flutter 同理：根类型变化不做替换）；
// - 回调槽在 createView 注册一次，updateView 只覆写槽内容——同一位置 widget
//   的回调集合（有无 onTap/onDraw）应在重建间保持稳定，新增回调类型不生效；
// - 视图树销毁统一由 Navigation/teardown 负责，Element 只释放回调槽。

#include <functional>
#include <memory>
#include <typeinfo>
#include <unordered_map>
#include <utility>
#include <vector>

#include "evk/esx_view.h"
#include "evk/ui/controls/button.h"
#include "evk/ui/controls/flex.h"
#include "evk/ui/event_bus.h"

namespace evk::ui {

// ---- 布局参数（在 Flex 父容器中生效，非 Flex 父容器忽略）----
struct FlexSpec {
    float main = -1.0f;          // 主轴固定 px；<0 由 weight 决定
    float weight = 0.0f;         // >0 按比例分主轴剩余空间（Expanded）
    float cross = -1.0f;         // 交叉轴固定 px；<0 stretch 占满
    int32_t align = 0;           // esx_flex_align（cross≥0 时生效）
    float marginMainBefore = 0.0f;
    float marginMainAfter = 0.0f;
    float marginCross = 0.0f;
};

// ---- 回调槽：std::function 经 trampoline 接 C ABI；指针随 Element 稳定 ----
struct Slot {
    std::function<void(esx_view)> fn;
};
struct ScrollSlot {
    std::function<void(float, float)> fn;
};

class Widget;

// 挂载记录（Flutter Element 极简版）：持有最近一次描述、retained 视图句柄、
// 子 Element 与回调槽。不负责销毁视图（见文件头说明）。
struct Element {
    std::unique_ptr<Widget> widget;
    esx_view view = 0;
    std::vector<std::unique_ptr<Element>> children;
    std::vector<std::unique_ptr<Slot>> slots;
    std::vector<std::unique_ptr<ScrollSlot>> scrollSlots;
};

// ---- Widget：不可变 UI 描述 ----
class Widget {
public:
    Widget() = default;
    virtual ~Widget() = default;
    Widget(Widget&&) = default;
    Widget& operator=(Widget&&) = default;
    Widget(const Widget&) = delete;
    Widget& operator=(const Widget&) = delete;

    FlexSpec layout; // 在 Flex 父容器中的排布参数

    // 创建 retained 视图（parent=0；挂载由 reconcile 统一处理）。
    virtual esx_view createView(Element& owner) const = 0;
    // 同类型时就地重设参数/回调（保留视图内部状态）。
    virtual void updateView(esx_view /*view*/, Element& /*owner*/) const {}
    virtual bool sameKind(const Widget& other) const {
        return typeid(*this) == typeid(other);
    }
    virtual std::vector<std::unique_ptr<Widget>>& childSpecs();
    // 子节点挂载点（ScrollW 重定向到滚动 content）。
    virtual esx_view childParent(esx_view view) const { return view; }
    // 每个子视图 reconcile 后回调：容器写入子视图排布参数（Flex spec）。
    virtual void configureChild(esx_view /*container*/, const Widget& /*child*/,
                                esx_view /*childView*/) const {}
};

// 链式布局修饰（rvalue 专用）：Box(c).mainSize(100).flex(1) …
template <typename Derived>
class WidgetT : public Widget {
public:
    Derived&& flex(float weight) && {
        layout.weight = weight;
        layout.main = -1.0f;
        return self();
    }
    Derived&& mainSize(float px) && {
        layout.main = px;
        layout.weight = 0.0f;
        return self();
    }
    Derived&& crossSize(float px) && {
        layout.cross = px;
        return self();
    }
    Derived&& crossAlign(int32_t align) && {
        layout.align = align;
        return self();
    }
    Derived&& marginMain(float before, float after) && {
        layout.marginMainBefore = before;
        layout.marginMainAfter = after;
        return self();
    }
    Derived&& marginCross(float px) && {
        layout.marginCross = px;
        return self();
    }

private:
    Derived&& self() { return std::move(static_cast<Derived&>(*this)); }
};

// ---- 内置 widget ----

// Box：背景色 + 可选点击 + 可选自定义绘制 ≈ Container+GestureDetector+CustomPaint。
class Box : public WidgetT<Box> {
public:
    uint32_t color = 0; // 0xRRGGBBAA；0 = 无背景
    std::function<void()> onTap;
    std::function<void(esx_view)> onDraw; // draw callback 内用 esx_draw_* 绘制

    Box() = default;
    explicit Box(uint32_t c) : color(c) {}

    esx_view createView(Element& owner) const override;
    void updateView(esx_view view, Element& owner) const override;
};

// ButtonW：包 Button 控件（样式 + 点击）。
class ButtonW : public WidgetT<ButtonW> {
public:
    esx_button_style style{};
    std::function<void()> onTap;

    ButtonW() = default;
    explicit ButtonW(const esx_button_style& s) : style(s) {}

    esx_view createView(Element& owner) const override;
    void updateView(esx_view view, Element& owner) const override;
};

// Flex 容器（Column/Row）：children 按 FlexSpec 排布。
class ColumnW : public WidgetT<ColumnW> {
public:
    uint32_t color = 0; // 容器背景；0 = 无
    ColumnW() = default;
    explicit ColumnW(std::vector<std::unique_ptr<Widget>> kids)
        : kids_(std::move(kids)) {}
    explicit ColumnW(uint32_t c) : color(c) {}
    void add(std::unique_ptr<Widget> w) { kids_.push_back(std::move(w)); }

    esx_view createView(Element& owner) const override;
    void updateView(esx_view view, Element& owner) const override;
    std::vector<std::unique_ptr<Widget>>& childSpecs() override { return kids_; }
    void configureChild(esx_view container, const Widget& child,
                        esx_view childView) const override;

private:
    std::vector<std::unique_ptr<Widget>> kids_;
};

class RowW : public ColumnW {
    // 仅主轴方向不同（createView 里 vertical=0），用 typeid 区分类型。
public:
    RowW() = default;
    explicit RowW(std::vector<std::unique_ptr<Widget>> kids)
        : ColumnW(std::move(kids)) {}
    explicit RowW(uint32_t c) : ColumnW(c) {}
    esx_view createView(Element& owner) const override;
};

// ScrollW：包 ScrollView；单个子视图（通常 Column）铺满内容区，
// 内容宽随 viewport，内容高 = contentHeight；滚动 offset 重建后保留。
class ScrollW : public WidgetT<ScrollW> {
public:
    float contentHeight = 0.0f;
    std::function<void(float, float)> onScroll;

    ScrollW() = default;
    ScrollW(std::unique_ptr<Widget> child, float height) : contentHeight(height) {
        kids_.push_back(std::move(child));
    }

    esx_view createView(Element& owner) const override;
    void updateView(esx_view view, Element& owner) const override;
    std::vector<std::unique_ptr<Widget>>& childSpecs() override { return kids_; }
    esx_view childParent(esx_view view) const override;

private:
    std::vector<std::unique_ptr<Widget>> kids_;
};

// ---- 构建辅助 ----

// 把描述树差异应用到 retained 视图树；通常由 Component/pushPage 驱动，
// 测试与高级用法可直接调用。parentView=0 时不挂载（根视图/页面根）。
void reconcile(std::unique_ptr<Element>& slot, std::unique_ptr<Widget> spec,
               esx_view parentView);
// 释放回调槽并销毁整棵子树（pop 路径不要用——视图树由 Navigation 销毁）。
void teardown(std::unique_ptr<Element>& slot);

template <typename W>
std::unique_ptr<Widget> makeWidget(W&& w) {
    return std::make_unique<std::decay_t<W>>(std::forward<W>(w));
}

template <typename... Kids>
ColumnW column(Kids&&... kids) {
    ColumnW c;
    (c.add(makeWidget(std::forward<Kids>(kids))), ...);
    return c;
}
inline ColumnW column(std::vector<std::unique_ptr<Widget>> kids) {
    return ColumnW(std::move(kids));
}

template <typename... Kids>
RowW row(Kids&&... kids) {
    RowW r;
    (r.add(makeWidget(std::forward<Kids>(kids))), ...);
    return r;
}
inline RowW row(std::vector<std::unique_ptr<Widget>> kids) {
    return RowW(std::move(kids));
}

// 数据 → 行 widget 列表（Flutter 的 items.map(...).toList()）。
template <typename T, typename F>
std::vector<std::unique_ptr<Widget>> mapWidgets(const std::vector<T>& items, F&& fn) {
    std::vector<std::unique_ptr<Widget>> out;
    out.reserve(items.size());
    for (const T& item : items) {
        out.push_back(makeWidget(fn(item)));
    }
    return out;
}

// ---- Component：页面（StatefulWidget+State 合并，成员变量即状态）----
class Component {
public:
    Component() = default;
    virtual ~Component();
    Component(const Component&) = delete;
    Component& operator=(const Component&) = delete;

    virtual std::unique_ptr<Widget> build() = 0;

    // 页面生命周期（Navigation 触发；forward: true=push 前进，false=pop 返回）。
    virtual bool onWillEnter(bool /*forward*/) { return true; } // false 取消导航
    virtual void onDidEnter(bool /*forward*/) {}
    virtual bool onWillLeave(bool /*forward*/) { return true; } // false 取消返回
    virtual void onDidLeave(bool /*forward*/) {}

    // 改状态并重建：mutate() → build() → reconcile 差异应用 → requestRender。
    void setState(std::function<void()> mutate = nullptr);

    esx_view view() const { return root_ ? root_->view : 0; }
    esx_view nav() const { return nav_; }

    // 事件监听（事件总线）：scope 自动绑定页面根视图（页被覆盖时收不到），
    // Component 销毁自动注销。handler 永不消费事件。
    void listen(int32_t eventId, esx_event_priority priority,
                std::function<void(const void* data)> handler);

private:
    esx_view nav_ = 0;
    std::unique_ptr<Element> root_;

    struct Listener {
        int32_t eventId;
        esx_event_priority priority;
        std::shared_ptr<std::function<void(const void*)>> handler;
        bool registered = false;
    };
    std::vector<Listener> listeners_;

    void registerListeners();
    void unregisterListeners();

    friend void pushPage(esx_view nav, std::unique_ptr<Component> page, bool animated);
    friend void teardownAllComponents();
};

// 把 Component 作为页面 push 进 Navigation：build → 注册生命周期转发 → push；
// pop 时框架自动销毁 Component（视图树由 Navigation 销毁）。
// Component 页面接管该 nav 的 on_pop 回调，App 不要再 esx_navigation_set_on_pop。
void pushPage(esx_view nav, std::unique_ptr<Component> page, bool animated);

// 销毁所有存活 Component（不销毁视图；App 整树销毁 Navigation 前调用）。
void teardownAllComponents();

} // namespace evk::ui
