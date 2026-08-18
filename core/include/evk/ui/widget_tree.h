#pragma once

/**
 * @file widget_tree.h
 * @brief 声明式 UI 核心：Widget（界面描述）→ Element（常驻协调节点）→ View（真实渲染节点）。
 *
 * @details 框架里有三棵树，职责分开：
 *
 *   - **Widget 树**：不可变的「界面描述」。每次 build() 都返回一棵全新的
 *     Widget 树，回答「界面现在应该是什么样」。Widget 不持有任何运行时
 *     状态，用完即弃——要改界面只能造新 Widget。
 *   - **Element 树**：常驻的协调节点树，与 Widget 树位置一一对应。每个
 *     Element 持有当前 Widget 的副本，并（直接或间接）持有一个 View；
 *     它跨多次重建存活，负责把新旧两份描述逐个位置对比，决定复用还是
 *     重建。
 *   - **View 树**：真正干活的节点——布局（handleBoundsChanged 级联）、
 *     绘制（paint → Canvas）、命中测试（hitTest）。由 Element 创建并
 *     持有，App 不直接操作。
 *
 * 一次 setState 的完整流程：
 * @code
 *   ① 改状态         State::setState 的 mutate()
 *   ② 生成新描述     State::build() 返回一棵全新 Widget 树
 *   ③ 逐位置对比     Element::updateChild：
 *       ├─ 同位置同类型 → Element::update：换掉 Widget 副本，就地微调
 *       │                 View（View 与其内部状态——滚动偏移、按压态——保留）
 *       ├─ 类型变了     → 旧子树 unmount 销毁，新子树 mount 重建
 *       └─ 新描述为空   → 旧子树 unmount 销毁
 *   ④ 标记重绘       requestRender()，下一个 VSync 才真正构建并提交帧
 * @endcode
 *
 * 四类 Widget 与四类 Element 一一对应（命名对照 Flutter）：
 *   - StatelessWidget    → StatelessElement：不产生 View；rebuild = 重新
 *                          build 出子 Widget 后继续对比下去；
 *   - StatefulWidget     → StatefulElement：挂一个 State 对象，页面状态、
 *                          生命周期 hook、setState 都在 State 上；
 *   - RenderObjectWidget → RenderObjectElement：唯一直接创建并持有 View
 *                          的 Widget/Element（Container/Text/ScrollView…）；
 *   - ProxyWidget        → ProxyElement：不产生 View，只在传递中修改子
 *                          Widget 的排布参数（Expanded/Center）。
 *
 * 与 Flutter 的差异：无 key（同位置同类型即复用）、无独立 RenderObject
 * 层（Element 直接持有 View）、setState 同步完成重建（Flutter 延迟到帧）。
 *
 * @see evk::ui::View 渲染节点（render_view.h）
 * @see evk::ui::FlexParentData Widget 向 Column/Row 声明排布参数的结构
 * （layout/flex_layout.h）
 */

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

/**
 * @brief build() 里能拿到的框架句柄，由 Element 自己实现。
 *
 * build() 里访问不到其他 Element，只能通过它：navigator() 用于跳页，
 * renderObject() 取本节点对应的 View（读取信息用，不要直接改）。
 */
class BuildContext {
public:
    virtual ~BuildContext() = default;
    virtual Navigator& navigator() const = 0;
    virtual View* renderObject() const = 0;
};

/**
 * @brief 页面在导航栈中的四段旅程（前进/返回由 @c forward 区分）。
 *
 * 由 Navigator 在转场前后依次下发，Will* 返回 false 可拦截本次导航
 * （如「表单还没保存」）；每个 Will 严格配对一次 Did，转场被取消时按
 * 最终归属收尾。详情见 navigation_stack.h。
 */
enum class RouteEvent {
    WillEnter,  ///< 即将进入台前（转场开始前）
    DidEnter,   ///< 已进入台前（转场结束后）
    WillLeave,  ///< 即将离开台前（被覆盖或将 pop 销毁）
    DidLeave,   ///< 已离开台前（pop 方向时页面随后销毁）
};

/**
 * @brief 界面描述基类：不可变，build() 每次造新的，用完即弃。
 *
 * 框架对一张 Widget 只做三件事：
 *   1. 它首次出现（或同位置类型变化重建）时，调 createElement() 造一个
 *      常驻 Element；
 *   2. 重建对比时调 canUpdate()：返回 true → 旧 Element 就地复用（微调），
 *      false → 销毁旧子树、重建新子树；
 *   3. 父容器是 Column/Row 时读 flexParentData()，取它的排布参数。
 */
class Widget {
public:
    virtual ~Widget() = default;

    // 不可变：禁止拷贝（要变就造新的），只允许移动（所有权交给 Element 持有）。
    Widget(const Widget&) = delete;
    Widget& operator=(const Widget&) = delete;
    Widget(Widget&&) = default;
    Widget& operator=(Widget&&) = default;

    /// 造本 Widget 对应的常驻 Element。仅在首次出现或类型变化重建时被调一次。
    virtual std::unique_ptr<Element> createElement() const = 0;

    /// 同位置的新旧 Widget 是否算「同一个东西」：默认同类型即可复用 Element。
    virtual bool canUpdate(const Widget& other) const {
        return typeid(*this) == typeid(other);
    }

    /**
     * @brief 向 Column/Row 父容器声明排布参数（主轴尺寸、flex 系数、
     *        交叉轴约束与对齐）。父容器不是 Flex 时不会被读取。
     */
    virtual FlexParentData flexParentData(Axis axis) const;

protected:
    Widget() = default;
};

/**
 * @brief 无状态组件：不产生 View，rebuild 时重新 build 出子 Widget 树。
 *
 * 用途：把一段子树封装成可复用组件。重建时它只是再 build 一遍，
 * 下方子树仍走正常的对比复用流程。
 */
class StatelessWidget : public Widget {
public:
    std::unique_ptr<Element> createElement() const override;

    /// 生成子描述树。context 由本组件的 Element 提供（拿导航器/View 用）。
    virtual std::unique_ptr<Widget> build(BuildContext& context) const = 0;
};

/**
 * @brief 有状态组件：对应的 Element 会持有一个 State 对象。
 *
 * Widget 本身依然不可变；所有可变数据（页码、勾选、加载状态……）放在
 * State 里。同位置同类型重建时 Element 与 State 都保留——这就是
 * 「界面重建了，但状态还在」的原因。
 */
class StatefulWidget : public Widget {
public:
    std::unique_ptr<Element> createElement() const override;

    /// 为本组件的 Element 创建 State。仅在 Element 首次 mount 时调用一次。
    virtual std::unique_ptr<State> createState() const = 0;
};

/**
 * @brief 页面的可变状态与生命周期容器（对应 Flutter 的 State）。
 *
 * 分工铁律：Widget 不可变、State 可变。改界面的唯一路径是
 * 「改 State 字段 → setState() → build() 生成新描述 → 框架对比重建」，
 * 不要直接改 View。
 *
 * 生命周期（由 StatefulElement 驱动，顺序固定）：
 * @code
 *   mount 时：attach → initState → 首次 build → didMount
 *   setState：mutate → build → 子树对比重建 → requestRender
 *   销毁时 ：dispose → detach
 * @endcode
 * 导航旅程 hook（onWillEnter 等）由导航栈经 StatefulElement 转发，
 * Will* 返回 false 可否决本次导航（见 RouteEvent）。
 */
class State {
public:
    virtual ~State() = default;

    /// 首次 build 之前调用一次：一次性初始化。
    virtual void initState() {}

    /// 首次 build 完成、View 已挂树之后调用一次：注册事件监听的时机。
    virtual void didMount() {}

    /// 子树销毁前调用：释放外部资源、取消在飞请求的最后时机。
    virtual void dispose() {}

    /// 生成界面描述（唯一必须实现的方法）。会被频繁调用，必须便宜。
    virtual std::unique_ptr<Widget> build(BuildContext& context) = 0;

    /// 导航 hook：返回 false 可否决本次进入。
    virtual bool onWillEnter(bool) { return true; }
    /// 导航 hook：已进入台前（发起请求/订阅数据的时机）。
    virtual void onDidEnter(bool) {}
    /// 导航 hook：返回 false 可否决本次离开（如未保存表单）。
    virtual bool onWillLeave(bool) { return true; }
    /// 导航 hook：已离开台前。
    virtual void onDidLeave(bool) {}

    /// State 是否仍挂在 Element 上。false 时 setState 会被忽略（页面已 pop）。
    bool mounted() const;

    /// 框架句柄：build 之外也能拿导航器。
    BuildContext& context() const;

    /// 本 Element 当前持有的 Widget 配置（重建后指向新 Widget）。
    const StatefulWidget& widget() const;

    /// 把 Widget 配置转成具体类型（如 widgetAs<HomePage>() 读配置字段）。
    template <typename T>
    const T& widgetAs() const {
        return dynamic_cast<const T&>(widget());
    }

    /**
     * @brief 触发界面重建：先执行 mutate 改状态，再 build 出新描述，
     *        同步完成子树对比重建，最后 requestRender 标记重绘。
     *
     * @param mutate 状态修改（可空：状态已在别处改好，只触发重建）。
     *
     * @note 重建是同步完成的（与 Flutter 延迟到帧不同），build() 必须
     *       便宜：别发请求、别重计算。mounted() 为 false 时调用无效。
     */
    void setState(std::function<void()> mutate = {});

    /**
     * @brief 订阅业务事件（主题切换、行情到达……）。
     *
     * @param eventId   事件 id（App 自定义，从 1 起）
     * @param priority  派发优先级
     * @param handler   回调；自动绑定本节点的 View——页面被导航覆盖时
     *                  收不到事件（隐式 pause）
     */
    void listen(
        int32_t eventId,
        EventPriority priority,
        std::function<void(const void* data)> handler);

    /// 绑定到 Element（框架内部调用，mount 路径）。
    void attach(Element* element);

    /// 与 Element 解绑（框架内部调用，unmount 路径）。
    void detach();

private:
    Element* element_ = nullptr;
    std::vector<EventSubscription> subscriptions_;
};

/**
 * @brief 直接对应一个 View 的 Widget（Container/Text/ScrollView…）。
 *
 * 对应 Flutter 的 RenderObjectWidget，只是渲染对象简化为常驻 View。
 * 框架在不同阶段问子类四个问题：
 *   - createRenderObject：首次 mount 时——怎么造 View（造完挂到父 View 下）；
 *   - updateRenderObject：同类型重建时——怎么把新参数应用到已存在的 View；
 *   - children / childParent：子 Widget 有哪些、它们的 View 挂到哪个
 *     View 下（ScrollViewWidget 借此把孩子挂进滚动 content）；
 *   - configureChild：子 View 落位后——写排布参数（Flex 靠它布局）。
 */
class RenderObjectWidget : public Widget {
public:
    std::unique_ptr<Element> createElement() const override;

    virtual std::unique_ptr<View> createRenderObject() const = 0;
    virtual void updateRenderObject(View&) const {}
    virtual std::vector<std::unique_ptr<Widget>>& children();
    virtual View* childParent(View& view) const { return &view; }
    virtual void configureChild(View&, const Widget&, View&) const {}
};

/**
 * @brief 不产生 View 的转接 Widget：只为修改子 Widget 的排布参数。
 *
 * Expanded 把 flex 系数写进参数，Center 把交叉轴对齐改成居中；
 * 最终显示的 View 仍是子 Widget 造的那个。对应 Flutter 的 ProxyWidget。
 */
class ProxyWidget : public Widget {
public:
    explicit ProxyWidget(std::unique_ptr<Widget> child);
    std::unique_ptr<Element> createElement() const override;

    std::unique_ptr<Widget>& child() { return child_; }
    const Widget& childWidget() const { return *child_; }

protected:
    std::unique_ptr<Widget> child_;
};

/// 四边内边距（Padding 的参数）。
struct EdgeInsets {
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;

    static EdgeInsets all(float value);
    static EdgeInsets symmetric(float horizontal, float vertical);
    static EdgeInsets only(float left, float top, float right, float bottom);
};

/**
 * @brief 通用块级组件：背景色 + 可选描边/圆角 + 点击 + 自绘。
 *
 * 一个类顶 Flutter 的 Container + GestureDetector + CustomPaint。
 * 传了 onTap 才成为触控目标；painter 在帧构建期间被调用，只能经
 * PaintContext 绘制，不能改视图树。
 */
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
 * @brief 位图组件：把 TextureStore 纹理拉伸铺满自身边界。
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

/// 按钮组件：包 Button 控件（自带 pressed 状态机与禁用态）。
class Button final : public RenderObjectWidget {
public:
    ButtonStyle style;
    std::function<void()> onPressed;
    bool enabled = true;

    Button(ButtonStyle style, std::function<void()> onPressed);
    std::unique_ptr<View> createRenderObject() const override;
    void updateRenderObject(View& view) const override;
};

/**
 * @brief 线性容器（Column/Row 的公共基类）。
 *
 * 对应的 View 是 FlexView：主轴空间先扣固定项与间距，剩余按 flex 系数
 * 分给弹性项（Expanded）。每个孩子的排布参数来自其 Widget 的
 * flexParentData()——所以 App 不写布局函数：尺寸变化时 FlexView 经
 * handleBoundsChanged 自动级联重排。
 */
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

/// 纵向 Flex：孩子自上而下排布。
class Column final : public Flex {
public:
    explicit Column(std::vector<std::unique_ptr<Widget>> children);
};

/// 横向 Flex：孩子自左而右排布。
class Row final : public Flex {
public:
    explicit Row(std::vector<std::unique_ptr<Widget>> children);
};

/**
 * @brief 可滚动容器（拖动 / 惯性 / 回弹）。
 *
 * 单子组件：子 View 挂到滚动 content 实体下（childParent 重定向）。
 * contentHeight 由 App 显式给出（v1 无测量、无懒构建）；滚动偏移是
 * 控件内部状态，重建后保留。
 */
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

/**
 * @brief 定高行列表：双轴可滚（上下翻行；contentWidth 加宽后左右看更多列；
 *        单段手势按主方向锁轴，横竖互斥）。
 *
 * 对照 Flutter 的 ListView(itemExtent: ...)：行高固定免测量，第 i 行位置
 * = (0, i × itemHeight)。v1 为全量构建（对应 Flutter 的
 * ListView(children: [...]) 默认构造器，无懒加载/回收），行数大时请
 * 控制规模。滚动状态（偏移、惯性）存在 View 上，重建后保留；删除一行
 * 后其余行自动上移。
 */
class ListView final : public RenderObjectWidget {
public:
    float contentWidth = -1.0f;  ///< 内容宽度；-1 = 视口宽（横向不可滚）
    std::function<void(float, float)> onScroll;

    ListView(float itemHeight, std::vector<std::unique_ptr<Widget>> items);

    std::unique_ptr<View> createRenderObject() const override;
    void updateRenderObject(View& view) const override;
    std::vector<std::unique_ptr<Widget>>& children() override { return children_; }
    View* childParent(View& view) const override;
    void configureChild(View& parent, const Widget& child, View& childView) const override;

private:
    float itemHeight_;
    std::vector<std::unique_ptr<Widget>> children_;
};

/**
 * @brief 转接组件：让孩子在 Column/Row 里按 flex 系数瓜分剩余主轴空间。
 *
 * 对应 Flutter 的 Expanded。多个 Expanded 按 flex 比例分配。
 */
class Expanded final : public ProxyWidget {
public:
    Expanded(std::unique_ptr<Widget> child, float flex = 1.0f);
    FlexParentData flexParentData(Axis axis) const override;

private:
    float flex_;
    FlexParentData horizontalData_;
    FlexParentData verticalData_;
};

/**
 * @brief 固定尺寸盒子：给孩子一个明确的宽高。
 *
 * 宽或高传 -1 表示该方向不约束（如 SizedBox(-1, 540, child) 只定高度）。
 * 对应的 SizedBoxView 会在自身尺寸变化时把孩子塞满。
 */
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

/**
 * @brief 内边距盒子：孩子的 View 缩进 insets 后占据剩余空间。
 *
 * 对 Column/Row 父容器上报的排布尺寸 = 孩子尺寸 + 内边距。
 */
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

/**
 * @brief 居中转接组件：把孩子的交叉轴对齐改成居中。
 *
 * 对应 Flutter 的 Center（本质是 Align）。注意它不产生 View：需要
 * 背景色时外面再包一层 Container。
 */
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

/**
 * @brief 常驻协调节点：持有一份 Widget 副本，并（直接或间接）持有一个
 *        View，跨多次重建存活。
 *
 * 框架对 Element 的四个操作，调用者都是框架本身：
 *
 *   - mount：子树「上岗」。两种时机——该位置首次出现（mountWidget），
 *     或同位置 Widget 类型变化、旧子树销毁后的重建（updateChild）。
 *     做的事：保存 Widget 副本、父 Element、View 挂靠点与导航器，
 *     标记 mounted，然后 firstMount() 一次性构建（造 View / 配 State /
 *     建孩子）。从此在岗，直到 unmount；
 *   - update：同位置、同类型的新 Widget 到达时就地微调——换掉 Widget
 *     副本，updateElement() 把新参数应用到现有结构上。View 与其内部
 *     状态（滚动偏移、按压态……）全部保留；
 *   - rebuild：让子树与当前 Widget 重新同步。各子类做法不同：
 *     Stateless 重新 build、Stateful 调 State::build、RenderObject
 *     微调 View 并逐子对比；
 *   - unmount：销毁整棵子树——先拆孩子与 View，再销毁 State。
 *
 * updateChild() 是唯一的「新旧对比」规则（相当于 Flutter 的
 * Element::updateChild），实现见 widget_tree.cpp。
 */
class Element : public BuildContext {
public:
    virtual ~Element();

    Element(const Element&) = delete;
    Element& operator=(const Element&) = delete;

    Navigator& navigator() const override;
    const Widget& widget() const { return *widget_; }
    bool mounted() const { return mounted_; }

    /**
     * @brief 子树上岗：绑定新 Widget 与挂靠信息，然后 firstMount()
     *        一次性构建（造 View / 配 State / 建孩子）。框架内部调用。
     */
    void mount(
        std::unique_ptr<Widget> widget,
        Element* parent,
        View* renderParent,
        Navigator* navigator);

    /// 就地微调：换成同类型的新 Widget，updateElement() 应用新参数。
    void update(std::unique_ptr<Widget> widget);

    /// 让子树与当前 Widget 重新同步（各子类实现不同，见类注释）。
    virtual void rebuild() = 0;

    /// 销毁子树：先拆孩子与 View，再清空自身（StatefulElement 还会销毁 State）。
    virtual void unmount();

    /// 导航事件下沉转发：StatefulElement 先问 State 的 hook，再传给子树。
    virtual bool dispatchRouteEvent(RouteEvent event, bool forward);

protected:
    Element() = default;

    /// 首次构建（mount 时恰好调用一次）。
    virtual void firstMount() = 0;

    /// update 时把新 Widget 的参数应用到现有结构。
    virtual void updateElement() = 0;

    /**
     * @brief 唯一的对比规则：把一个子位置的新 Widget 与旧 Element 对齐
     *        ——复用微调、或销毁重建（规则见 widget_tree.cpp）。
     *
     * @param child        该位置的旧 Element 槽（可被替换成新 Element）
     * @param widget       新 Widget（空 = 该位置撤销，销毁旧子树）
     * @param renderParent 孩子的 View 应挂到的父 View
     */
    void updateChild(
        std::unique_ptr<Element>& child,
        std::unique_ptr<Widget> widget,
        View* renderParent);

    std::unique_ptr<Widget> widget_;   ///< 当前持有的 Widget 副本（对比基准）
    Element* parent_ = nullptr;        ///< Element 树上的父节点
    View* renderParent_ = nullptr;     ///< 子 View 应挂到的父 View
    Navigator* navigator_ = nullptr;   ///< 导航器（BuildContext::navigator 用）
    bool mounted_ = false;             ///< 是否在岗
};

/**
 * @brief 顶层入口：把一张根 Widget 变成一棵在岗的 Element 子树。
 *
 * 流程：widget->createElement() 造根 Element → element->mount(...) 绑定
 * 挂靠信息并 firstMount()；firstMount 内部经 updateChild 递归，整棵
 * 子树一次建好。
 *
 * @param widget       根 Widget
 * @param renderParent 根 View 的挂靠点；页面根传 nullptr（先游离，
 *                     随后由导航栈接管）
 * @param navigator    导航器
 * @param parent       Element 树父节点（根传 nullptr）
 * @return 建好的 Element 根；widget 为空返回 nullptr
 */
std::unique_ptr<Element> mountWidget(
    std::unique_ptr<Widget> widget,
    View* renderParent,
    Navigator* navigator,
    Element* parent = nullptr);

/// 构造辅助：完美转发造一个 Widget（makeWidget<Container>(color)）。
template <typename W, typename... Args>
std::unique_ptr<Widget> makeWidget(Args&&... args) {
    return std::make_unique<W>(std::forward<Args>(args)...);
}

/// 构造辅助：把多个 Widget 收进一个列表（column/row 的参数收集）。
template <typename... Widgets>
std::vector<std::unique_ptr<Widget>> widgetList(Widgets&&... widgets) {
    std::vector<std::unique_ptr<Widget>> result;
    result.reserve(sizeof...(widgets));
    (result.push_back(std::forward<Widgets>(widgets)), ...);
    return result;
}

/// 构造辅助：纵向 Flex 描述。
template <typename... Widgets>
std::unique_ptr<Widget> column(Widgets&&... widgets) {
    return makeWidget<Column>(widgetList(std::forward<Widgets>(widgets)...));
}

inline std::unique_ptr<Widget> column(std::vector<std::unique_ptr<Widget>> widgets) {
    return makeWidget<Column>(std::move(widgets));
}

/// 构造辅助：横向 Flex 描述。
template <typename... Widgets>
std::unique_ptr<Widget> row(Widgets&&... widgets) {
    return makeWidget<Row>(widgetList(std::forward<Widgets>(widgets)...));
}

inline std::unique_ptr<Widget> row(std::vector<std::unique_ptr<Widget>> widgets) {
    return makeWidget<Row>(std::move(widgets));
}

// ---- 小写工厂：build() 里的简写，一行造一个常用组件（HomePage 的 build 全靠它们）----

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
/// contentWidth 传 -1 = 视口宽（横向不可滚）；加宽后横向纵向同时可滚。
std::unique_ptr<Widget> listView(
    float itemHeight,
    std::vector<std::unique_ptr<Widget>> items,
    float contentWidth = -1.0f);

} // namespace evk::ui
