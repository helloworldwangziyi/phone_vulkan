#pragma once

/**
 * @file widget.h
 * @brief 声明式 UI 层（仿 Flutter 的 Widget/Element 极简模型）。
 *
 * 页面 = Component 的 build() 返回 Widget 描述树；reconcile 把描述差异应用到
 * retained 视图树（同类型就地更新，内部状态如滚动 offset 保留；不同类型销毁重建）；
 * setState 触发局部重建。
 *
 * 页面写法（结构即代码，所见即所得）：
 * @code{.cpp}
 * class RankPage : public evk::ui::Component {
 *     std::vector<RankItem> items_;                    // 状态：页面数据
 *     std::unique_ptr<Widget> build() override {       // 创建了什么、挂在哪，一眼可见
 *         auto topBar = Box(theme().navBar).mainSize(dp(150));
 *         auto list   = column(...数据行...).flex(1);
 *         return makeWidget(column(std::move(topBar), std::move(list)));
 *     }
 *     void onDidEnter(bool) override { requestQuote(); }   // 进入请求
 *     void onWillLeave(bool) override { cancelQuote(); }   // 离开取消（返回 false 可拦截）
 * };
 * pushPage(nav, std::make_unique<RankPage>(), true);
 * @endcode
 *
 * @note build 根 widget 类型必须稳定（Flutter 同理：根类型变化不做替换）。
 * @note 回调槽在 createView 注册一次，updateView 只覆写槽内容——同一位置 widget
 *       的回调集合（有无 onTap/onDraw）应在重建间保持稳定，新增回调类型不生效。
 * @note 视图树销毁统一由 Navigation/teardown 负责，Element 只释放回调槽。
 */

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

/**
 * @brief 布局参数（在 Flex 父容器中生效，非 Flex 父容器忽略）。
 */
struct FlexSpec {
    float main = -1.0f;           ///< 主轴固定 px；<0 由 weight 决定
    float weight = 0.0f;          ///< >0 按比例分主轴剩余空间（Expanded）
    float cross = -1.0f;          ///< 交叉轴固定 px；<0 stretch 占满
    int32_t align = 0;            ///< esx_flex_align（cross≥0 时生效）
    float marginMainBefore = 0.0f; ///< 主轴前外边距
    float marginMainAfter = 0.0f;  ///< 主轴后外边距
    float marginCross = 0.0f;      ///< 交叉轴外边距
};

/**
 * @brief 回调槽：std::function 经 trampoline 接 C ABI；指针随 Element 稳定。
 */
struct Slot {
    std::function<void(esx_view)> fn; ///< 实际回调，trampoline 判空后调用
};

/**
 * @brief 滚动回调槽，语义同 Slot；回调参数为 offsetX/offsetY。
 */
struct ScrollSlot {
    std::function<void(float, float)> fn;
};

class Widget;

/**
 * @brief 挂载记录（Flutter Element 极简版）。
 *
 * 持有最近一次描述、retained 视图句柄、子 Element 与回调槽。
 * 不负责销毁视图（见文件头说明）。
 */
struct Element {
    std::unique_ptr<Widget> widget;               ///< 最近一次 reconcile 使用的描述
    esx_view view = 0;                            ///< 描述创建出的 retained 视图句柄
    std::vector<std::unique_ptr<Element>> children;    ///< 子 Element，按下标与新描述配对
    std::vector<std::unique_ptr<Slot>> slots;          ///< 点击/绘制等回调槽
    std::vector<std::unique_ptr<ScrollSlot>> scrollSlots; ///< 滚动回调槽
};

/**
 * @brief 不可变 UI 描述。
 *
 * 每次 build() 重新创建，仅作为 reconcile 的对比基准与参数来源。
 */
class Widget {
public:
    Widget() = default;
    virtual ~Widget() = default;
    Widget(Widget&&) = default;
    Widget& operator=(Widget&&) = default;
    Widget(const Widget&) = delete;
    Widget& operator=(const Widget&) = delete;

    FlexSpec layout; ///< 在 Flex 父容器中的排布参数

    /**
     * @brief 创建 retained 视图（parent=0；挂载由 reconcile 统一处理）。
     * @param owner 挂载记录，回调槽注册到它上面
     * @return 新视图句柄；失败返回 0
     */
    virtual esx_view createView(Element& owner) const = 0;

    /**
     * @brief 同类型时就地重设参数/回调（保留视图内部状态）。
     */
    virtual void updateView(esx_view /*view*/, Element& /*owner*/) const {}

    /**
     * @brief 判断新旧描述是否同类型（默认按 typeid 比较）。
     */
    virtual bool sameKind(const Widget& other) const {
        return typeid(*this) == typeid(other);
    }

    /**
     * @brief 子描述列表（无子节点的 widget 返回静态空表）。
     */
    virtual std::vector<std::unique_ptr<Widget>>& childSpecs();

    /**
     * @brief 子节点挂载点（ScrollW 重定向到滚动 content）。
     */
    virtual esx_view childParent(esx_view view) const { return view; }

    /**
     * @brief 每个子视图 reconcile 后回调：容器写入子视图排布参数（Flex spec）。
     */
    virtual void configureChild(esx_view /*container*/, const Widget& /*child*/,
                                esx_view /*childView*/) const {}
};

/**
 * @brief 链式布局修饰基类（CRTP，rvalue 专用）。
 *
 * 用法：`Box(c).mainSize(100).flex(1)` …
 */
template <typename Derived>
class WidgetT : public Widget {
public:
    /**
     * @brief 按比例分主轴剩余空间（Expanded），并清除主轴固定尺寸。
     * @param weight 权重，>0 生效
     * @return 自身右值引用，供链式调用
     */
    Derived&& flex(float weight) && {
        layout.weight = weight;
        layout.main = -1.0f;
        return self();
    }

    /**
     * @brief 设主轴固定尺寸，并清除 weight。
     * @param px 主轴 px
     * @return 自身右值引用，供链式调用
     */
    Derived&& mainSize(float px) && {
        layout.main = px;
        layout.weight = 0.0f;
        return self();
    }

    /**
     * @brief 设交叉轴固定尺寸。
     * @param px 交叉轴 px；<0 恢复 stretch 占满
     * @return 自身右值引用，供链式调用
     */
    Derived&& crossSize(float px) && {
        layout.cross = px;
        return self();
    }

    /**
     * @brief 设交叉轴对齐（esx_flex_align，cross≥0 时生效）。
     * @return 自身右值引用，供链式调用
     */
    Derived&& crossAlign(int32_t align) && {
        layout.align = align;
        return self();
    }

    /**
     * @brief 设主轴前后外边距。
     * @return 自身右值引用，供链式调用
     */
    Derived&& marginMain(float before, float after) && {
        layout.marginMainBefore = before;
        layout.marginMainAfter = after;
        return self();
    }

    /**
     * @brief 设交叉轴外边距。
     * @return 自身右值引用，供链式调用
     */
    Derived&& marginCross(float px) && {
        layout.marginCross = px;
        return self();
    }

private:
    Derived&& self() { return std::move(static_cast<Derived&>(*this)); }
};

/**
 * @brief 背景色 + 可选点击 + 可选自定义绘制 ≈ Container+GestureDetector+CustomPaint。
 */
class Box : public WidgetT<Box> {
public:
    uint32_t color = 0;                 ///< 0xRRGGBBAA；0 = 无背景
    std::function<void()> onTap;        ///< 点击回调（为空则 Box 不成为触控目标）
    std::function<void(esx_view)> onDraw; ///< draw callback 内用 esx_draw_* 绘制

    Box() = default;
    explicit Box(uint32_t c) : color(c) {}

    esx_view createView(Element& owner) const override;
    void updateView(esx_view view, Element& owner) const override;
};

/**
 * @brief 包 Button 控件（样式 + 点击）。
 */
class ButtonW : public WidgetT<ButtonW> {
public:
    esx_button_style style{};      ///< 按钮样式（normal/pressed/disabled 颜色）
    std::function<void()> onTap;   ///< 点击回调

    ButtonW() = default;
    explicit ButtonW(const esx_button_style& s) : style(s) {}

    esx_view createView(Element& owner) const override;
    void updateView(esx_view view, Element& owner) const override;
};

/**
 * @brief Flex 容器（Column/Row）：children 按 FlexSpec 排布。
 */
class ColumnW : public WidgetT<ColumnW> {
public:
    uint32_t color = 0; ///< 容器背景；0 = 无
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

/**
 * @brief 仅主轴方向不同（createView 里 vertical=0），用 typeid 区分类型。
 */
class RowW : public ColumnW {
public:
    RowW() = default;
    explicit RowW(std::vector<std::unique_ptr<Widget>> kids)
        : ColumnW(std::move(kids)) {}
    explicit RowW(uint32_t c) : ColumnW(c) {}
    esx_view createView(Element& owner) const override;
};

/**
 * @brief 包 ScrollView；单个子视图（通常 Column）铺满内容区。
 *
 * 内容宽随 viewport，内容高 = contentHeight；滚动 offset 重建后保留。
 */
class ScrollW : public WidgetT<ScrollW> {
public:
    float contentHeight = 0.0f;                ///< 内容高度 px
    std::function<void(float, float)> onScroll; ///< 滚动回调（offsetX, offsetY）

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

/**
 * @brief 把描述树差异应用到 retained 视图树。
 *
 * 通常由 Component/pushPage 驱动，测试与高级用法可直接调用。
 * @param slot 挂载记录的持有位（可为空或指向旧树）
 * @param spec 新描述树；nullptr 等价于 teardown
 * @param parentView 挂载父视图；=0 时不挂载（根视图/页面根）
 */
void reconcile(std::unique_ptr<Element>& slot, std::unique_ptr<Widget> spec,
               esx_view parentView);

/**
 * @brief 释放回调槽并销毁整棵子树。
 * @warning pop 路径不要用——视图树由 Navigation 销毁。
 */
void teardown(std::unique_ptr<Element>& slot);

/**
 * @brief 构造 Widget 描述对象的辅助。
 */
template <typename W>
std::unique_ptr<Widget> makeWidget(W&& w) {
    return std::make_unique<std::decay_t<W>>(std::forward<W>(w));
}

/**
 * @brief Column 构建辅助（可变参数版）。
 */
template <typename... Kids>
ColumnW column(Kids&&... kids) {
    ColumnW c;
    (c.add(makeWidget(std::forward<Kids>(kids))), ...);
    return c;
}

/**
 * @brief Column 构建辅助（vector 版）。
 */
inline ColumnW column(std::vector<std::unique_ptr<Widget>> kids) {
    return ColumnW(std::move(kids));
}

/**
 * @brief Row 构建辅助（可变参数版）。
 */
template <typename... Kids>
RowW row(Kids&&... kids) {
    RowW r;
    (r.add(makeWidget(std::forward<Kids>(kids))), ...);
    return r;
}

/**
 * @brief Row 构建辅助（vector 版）。
 */
inline RowW row(std::vector<std::unique_ptr<Widget>> kids) {
    return RowW(std::move(kids));
}

/**
 * @brief 数据 → 行 widget 列表（Flutter 的 items.map(...).toList()）。
 * @param items 数据源
 * @param fn 单条数据 → widget 的映射函数
 */
template <typename T, typename F>
std::vector<std::unique_ptr<Widget>> mapWidgets(const std::vector<T>& items, F&& fn) {
    std::vector<std::unique_ptr<Widget>> out;
    out.reserve(items.size());
    for (const T& item : items) {
        out.push_back(makeWidget(fn(item)));
    }
    return out;
}

/**
 * @brief 页面基类（StatefulWidget+State 合并，成员变量即状态）。
 */
class Component {
public:
    Component() = default;
    virtual ~Component();
    Component(const Component&) = delete;
    Component& operator=(const Component&) = delete;

    /**
     * @brief 构建 UI 描述树（每次 setState 重新调用）。
     */
    virtual std::unique_ptr<Widget> build() = 0;

    /**
     * @brief 页面即将进入（Navigation 触发）。
     * @param forward true=push 前进，false=pop 返回
     * @return false 取消导航
     */
    virtual bool onWillEnter(bool /*forward*/) { return true; }

    /**
     * @brief 页面已进入（Navigation 触发）。
     * @param forward true=push 前进，false=pop 返回
     */
    virtual void onDidEnter(bool /*forward*/) {}

    /**
     * @brief 页面即将离开（Navigation 触发）。
     * @param forward true=push 前进，false=pop 返回
     * @return false 取消返回
     */
    virtual bool onWillLeave(bool /*forward*/) { return true; }

    /**
     * @brief 页面已离开（Navigation 触发）。
     * @param forward true=push 前进，false=pop 返回
     */
    virtual void onDidLeave(bool /*forward*/) {}

    /**
     * @brief 改状态并重建：mutate() → build() → reconcile 差异应用 → requestRender。
     * @param mutate 状态修改闭包；可为空（仅触发重建）
     */
    void setState(std::function<void()> mutate = nullptr);

    /**
     * @brief 页面根视图句柄（未挂载时为 0）。
     */
    esx_view view() const { return root_ ? root_->view : 0; }

    /**
     * @brief 所属 Navigation 句柄。
     */
    esx_view nav() const { return nav_; }

    /**
     * @brief 事件监听（事件总线）。
     *
     * scope 自动绑定页面根视图（页被覆盖时收不到），Component 销毁自动注销。
     * handler 永不消费事件。
     */
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

/**
 * @brief 把 Component 作为页面 push 进 Navigation。
 *
 * build → 注册生命周期转发 → push；pop 时框架自动销毁 Component
 * （视图树由 Navigation 销毁）。
 * @note Component 页面接管该 nav 的 on_pop 回调，App 不要再 esx_navigation_set_on_pop。
 */
void pushPage(esx_view nav, std::unique_ptr<Component> page, bool animated);

/**
 * @brief 销毁所有存活 Component（不销毁视图；App 整树销毁 Navigation 前调用）。
 */
void teardownAllComponents();

} // namespace evk::ui
