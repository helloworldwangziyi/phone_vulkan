#pragma once

/**
 * @file widget_tree.h
 * @brief 声明式 UI 的「图纸 ↔ 工地」机制：Widget 描述树、Element 挂载树与状态容器。
 *
 * @details 把整个 UI 系统想象成一个装修工地，三个角色各司其职：
 *
 *   - **Widget = 设计图纸**。设计师（build()）每次都重新画一整沓新图纸，
 *     画完旧图纸立刻作废——图纸本身是不可变的，要改只能画新的。
 *   - **Element = 工地工位**。每张图纸上的构件，在工地上都有一个对应的
 *     驻场工位；工位持有真正建成的实体（View），跨多次翻新存活。
 *   - **View = 建成的实体构件**（墙、柜子、灯具……），真正参与布局、命中
 *     与绘制。
 *
 * 一次翻新（setState）的流程：
 * @code
 *   ① 在记事本上改一笔          State::setState 的 mutate()
 *   ② 设计师按记事本重画图纸     State::build()
 *   ③ 每个工位对照新旧图纸       Element::updateChild
 *        ├─ 同类构件  → 就地微调 update → updateElement （不拆工位，状态保留）
 *        └─ 构件换类型 → 拆工位重建 unmount → createElement → mount
 * @endcode
 *
 * 图纸分四种，对应四种工位（对照 Flutter 同名类）：
 *   - StatelessWidget    → StatelessElement：自己不产实体，每次 build 画子图纸；
 *   - StatefulWidget     → StatefulElement：工位多一本记事本（State），
 *                          生命周期 hook 与 setState 都在这本记事本上；
 *   - RenderObjectWidget → RenderObjectElement：实体构件图纸，create/update
 *                          对应真实 View；
 *   - ProxyWidget        → ProxyElement：不产实体的「转接件」，只包一张子图纸，
 *                          顺手在孩子的便签上添几笔（Expanded/Center 就干这个）。
 *
 * 与 Flutter 的差异：无 key 机制（同位置同类型即复用）、无独立 RenderObject
 * 层（Element 直接持有 View）、setState 同步 rebuild（Flutter 延迟到帧）。
 *
 * @see evk::ui::View 实体构件（render_view.h）
 * @see evk::ui::FlexParentData 图纸上写给父布局的便签（layout/flex_layout.h）
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
 * @brief 工位发给设计师的「工作证」：build() 里凭它找到导航器与实体。
 *
 * 设计师在 build() 里画图纸时，手上只有这张工作证，不能直接碰工地上的
 * 其他工位。需要跳页（navigator().push）或取当前实体的信息时，都从它出发。
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
 * @brief 设计图纸基类：只描述「这里该是什么样」，不可变、用完即弃。
 *
 * 图纸上写了三件事：
 *   1. 该派哪类工人（createElement，由四种 Widget 子类各自回答）；
 *   2. 新旧图纸对照规则（canUpdate：默认同类型即同类，决定工位「微调」
 *      还是「拆了重建」）；
 *   3. 写给父布局的便签（flexParentData：我在 Flex 容器里想占多大、对齐哪边）。
 */
class Widget {
public:
    virtual ~Widget() = default;

    // 图纸不可变：禁止拷贝（要变就画一张新的），只允许把图纸「递」给工位。
    Widget(const Widget&) = delete;
    Widget& operator=(const Widget&) = delete;
    Widget(Widget&&) = default;
    Widget& operator=(Widget&&) = default;

    /// 图纸上写的工人派遣令：这张图纸该由哪类工位承接（由子类实现）。
    virtual std::unique_ptr<Element> createElement() const = 0;

    /// 新旧图纸对照：默认「同一类型」就算同一构件，工位可复用。
    virtual bool canUpdate(const Widget& other) const {
        return typeid(*this) == typeid(other);
    }

    /**
     * @brief 图纸上写给父布局的便签（Flutter 的 parent data）。
     *
     * 只在父容器是 Flex（Column/Row）时被读取：主轴尺寸、弹性系数、
     * 交叉轴尺寸与对齐、间距。普通父容器根本不看这张便签。
     */
    virtual FlexParentData flexParentData(Axis axis) const;

protected:
    Widget() = default;
};

/**
 * @brief 分包图纸：自己不直接对应实体，每次 build() 画一张子图纸交差。
 *
 * 典型用途：把一段固定的子树（标题栏、徽章行）包成一个可复用组件——
 * 状态不变时每次重建它只是「重画子图纸」，工位不会动。
 */
class StatelessWidget : public Widget {
public:
    std::unique_ptr<Element> createElement() const override;

    /// 画子图纸。context 是工位发的工作证（拿导航器/实体用）。
    virtual std::unique_ptr<Widget> build(BuildContext& context) const = 0;
};

/**
 * @brief 带记事本的图纸：工位会为它专门配一本 State 记事本。
 *
 * Widget 本身依旧不可变；可变的东西（页码、勾选、请求状态……）全部记在
 * State 里。同一位置的新旧 StatefulWidget 只要类型相同，工位与记事本都会
 * 复用——这就是「重建后状态还在」的根基。
 */
class StatefulWidget : public Widget {
public:
    std::unique_ptr<Element> createElement() const override;

    /// 为新工位配一本新记事本。
    virtual std::unique_ptr<State> createState() const = 0;
};

/**
 * @brief 工位的记事本：页面唯一允许「可变」的地方。
 *
 * 分工铁律：Widget 描述不可变，State 可变。改界面的正确姿势永远是
 * 「记事本上改一笔 → setState → 设计师重画图纸 → 工地 diff」，
 * 而不是直接去搬工地上的实体。
 *
 * 生命周期（对照 Flutter 的 State）：
 * @code
 *   attach → initState → build → didMount
 *   （每次 setState：mutate → build → reconcile → requestRender）
 *   unmount → dispose → detach
 * @endcode
 * 导航四段旅程（onWillEnter/onDidEnter/onWillLeave/onDidLeave）由
 * StatefulElement 在 RouteEvent 到达时转发过来。
 */
class State {
public:
    virtual ~State() = default;

    /// 记事本刚挂上工位（attach 后、首次 build 前）——一次性的初始化。
    virtual void initState() {}

    /// 首次 build 完成、实体已挂上工地之后——注册事件监听的时机。
    virtual void didMount() {}

    /// 工位拆除前——释放外部资源、取消在飞请求的最后机会。
    virtual void dispose() {}

    /// 按记事本画图纸（唯一必写的方法）。
    virtual std::unique_ptr<Widget> build(BuildContext& context) = 0;

    /// 导航 hook：返回 false 可拦截本次进入。
    virtual bool onWillEnter(bool) { return true; }
    /// 导航 hook：已进入台前（发起请求/订阅数据的时机）。
    virtual void onDidEnter(bool) {}
    /// 导航 hook：返回 false 可拦截本次离开（如未保存表单）。
    virtual bool onWillLeave(bool) { return true; }
    /// 导航 hook：已离开台前。
    virtual void onDidLeave(bool) {}

    /// 记事本是否还挂在工位上（未挂上的 setState 会被忽略）。
    bool mounted() const;

    /// 工作证（build 之外也能用它拿导航器）。
    BuildContext& context() const;

    /// 本工位当前承接的图纸（StatefulWidget 配置）。
    const StatefulWidget& widget() const;

    /// 把图纸动态转成具体类型（如 widgetAs<HomePage>() 读配置字段）。
    template <typename T>
    const T& widgetAs() const {
        return dynamic_cast<const T&>(widget());
    }

    /**
     * @brief 翻新入口：改一笔记事本，然后重画图纸并 diff 到工地。
     *
     * @param mutate 先执行的记事本修改（可空：状态已在别处改好，只触发重建）。
     *
     * @note 重建是同步立即完成的（与 Flutter 延迟到帧不同），
     *       build() 必须便宜：别发请求、别重计算。
     */
    void setState(std::function<void()> mutate = {});

    /**
     * @brief 订阅业务事件（主题切换、行情到达……）。
     *
     * @param eventId   事件 id（App 自定义，从 1 起）
     * @param priority  派发优先级
     * @param handler   回调；scope 自动绑定本工位的实体视图——
     *                  页面被导航覆盖时收不到事件（隐式 pause）
     */
    void listen(
        int32_t eventId,
        EventPriority priority,
        std::function<void(const void* data)> handler);

    /// 挂上工位（框架内部调用）。
    void attach(Element* element);

    /// 离开工位（框架内部调用）。
    void detach();

private:
    Element* element_ = nullptr;
    std::vector<EventSubscription> subscriptions_;
};

/**
 * @brief 实体构件图纸：这张图纸上的构件，会在工地上建成一个真实的 View。
 *
 * 与 Flutter 的 RenderObjectWidget 同名同构，只是「渲染对象」简化为
 * retained 的 View 树节点。四个问题由子类回答：
 *   - createRenderObject：怎么造实体；
 *   - updateRenderObject：同类型图纸微调时怎么改实体；
 *   - children/childParent：孩子图纸在哪、建成后挂到哪个实体上；
 *   - configureChild：孩子落位后，给它写排布参数（Flex 靠这个工作）。
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
 * @brief 转接件图纸：工地上不产实体，只包一张子图纸。
 *
 * 它存在的意义是「在孩子的便签上添几笔」：Expanded 把 flex 写进便签，
 * Center 把交叉轴对齐改成正中——自己不出实体，孩子的实体还是孩子的。
 * 对照 Flutter 的 ProxyWidget（Expanded/Center 都是它）。
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

/**
 * @brief 四边内边距（Padding 的图纸参数）。
 */
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
 * @brief 万能砖块：背景色 + 可选描边/圆角 + 点击 + 自绘。
 *
 * 一个类顶 Flutter 的 Container + GestureDetector + CustomPaint。
 * 有 onTap 才注册点击回调（无 onTap 的 Container 不成为触控目标）；
 * painter 在帧构建期间被调用，只能用 PaintContext 画，不能改视图树。
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

/**
 * @brief 按钮图纸：包 Button 控件（自带 pressed 状态机与禁用态）。
 */
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
 * @brief Flex 线性容器图纸（Column/Row 的公共基类）。
 *
 * 工地上的实体是 FlexView：先扣固定项与间距，剩余主轴空间按 flex 系数
 * 分给弹性项（Expanded）。子视图的排布参数全部来自孩子图纸上的
 * flexParentData 便签——所以「App 不写 layout 函数」，尺寸一变
 * FlexView 自动级联重排。
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
 * @brief 可滚动容器图纸：包 ScrollView 控件（拖动/惯性/回弹）。
 *
 * 单子节点：孩子建成后挂到滚动的 content 实体上（childParent 重定向）。
 * contentHeight 由 App 显式给出（v1 无测量、无懒构建）；滚动 offset 是
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
 * @brief 转接件：把孩子的 flex 便签改成「吃满剩余主轴空间」。
 *
 * 对照 Flutter 的 Expanded。flex 参数 = 弹性系数（多个 Expanded 按比例分）。
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
 * 工地上对应的 SizedBoxView 会在自身尺寸变化时把孩子塞满。
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
 * @brief 内边距盒子：孩子的实体缩进 insets 后塞进剩余空间。
 *
 * 便签上会把 insets 折算进自己对外报的尺寸（Flex 父容器看到的尺寸
 * = 孩子尺寸 + 内边距）。
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
 * @brief 居中转接件：把孩子的交叉轴对齐改成正中。
 *
 * 对照 Flutter 的 Center（本质是 Align）。注意它不产实体：需要背景色时
 * 外面再包一层 Container。
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
 * @brief 工地工位基类：Widget 描述树的「驻场工人」，持有真实实体。
 *
 * 一棵 Widget 描述树 mount 后，就变成一棵一一对应的 Element 树——
 * 图纸是每次重建画新的，工位是跨重建存活的。工位的四项基本功：
 *
 *   - mount：拿到新图纸与挂靠位置，firstMount 首次施工；
 *   - update：换上新图纸，updateElement 微调实体（不拆工位）；
 *   - unmount：拆除——先拆孩子与实体，再撕记事本；
 *   - updateChild（交接算法）：对照新旧子图纸决定每个孩子「微调」还是
 *     「换工人」，见实现文件里的大段注释。
 */
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

    /// 换图纸微调（前提：canUpdate 判定同类型）。
    void update(std::unique_ptr<Widget> widget);

    /// 重画（各类工位各干各的：stateless 重 build、stateful 问记事本、render 微调实体）。
    virtual void rebuild() = 0;

    /// 拆除工位：拆孩子 → 拆实体 → 撕记事本，再清空自身。
    virtual void unmount();

    /// 导航旅程转发：StatefulElement 转发给 State 的 hook，其余沿子树下沉。
    virtual bool dispatchRouteEvent(RouteEvent event, bool forward);

protected:
    Element() = default;

    /// 首次施工（mount 时调用一次）。
    virtual void firstMount() = 0;

    /// 换图纸时的微调动作。
    virtual void updateElement() = 0;

    /**
     * @brief 交接算法核心：把一张新子图纸交给一个工位槽。
     *
     * @param child        待交接的工位槽（可空）
     * @param widget       新子图纸（空 = 撤掉这个孩子）
     * @param renderParent 孩子实体应挂靠的实体父节点
     */
    void updateChild(
        std::unique_ptr<Element>& child,
        std::unique_ptr<Widget> widget,
        View* renderParent);

    std::unique_ptr<Widget> widget_;   ///< 当前承接的图纸（对照用）
    Element* parent_ = nullptr;        ///< 工位树上的父工位
    View* renderParent_ = nullptr;     ///< 实体应挂靠的实体父节点
    Navigator* navigator_ = nullptr;   ///< 导航器（BuildContext 用）
    bool mounted_ = false;             ///< 是否在岗
};

/**
 * @brief 开工入口：一张图纸 → 一个在岗工位。
 *
 * @param widget       根图纸
 * @param renderParent 根实体挂靠点（页面根传 nullptr 表示游离，随后被导航接管）
 * @param navigator    导航器
 * @param parent       工位树父工位（根传 nullptr）
 * @return 建成的工位；图纸为空返回 nullptr
 */
std::unique_ptr<Element> mountWidget(
    std::unique_ptr<Widget> widget,
    View* renderParent,
    Navigator* navigator,
    Element* parent = nullptr);

/// 建造辅助：Args 完美转发构造一张图纸（makeWidget<Container>(color)）。
template <typename W, typename... Args>
std::unique_ptr<Widget> makeWidget(Args&&... args) {
    return std::make_unique<W>(std::forward<Args>(args)...);
}

/// 建造辅助：多张图纸装进一个列表（column/row 的参数收集）。
template <typename... Widgets>
std::vector<std::unique_ptr<Widget>> widgetList(Widgets&&... widgets) {
    std::vector<std::unique_ptr<Widget>> result;
    result.reserve(sizeof...(widgets));
    (result.push_back(std::forward<Widgets>(widgets)), ...);
    return result;
}

/// 建造辅助：纵向 Flex 图纸。
template <typename... Widgets>
std::unique_ptr<Widget> column(Widgets&&... widgets) {
    return makeWidget<Column>(widgetList(std::forward<Widgets>(widgets)...));
}

inline std::unique_ptr<Widget> column(std::vector<std::unique_ptr<Widget>> widgets) {
    return makeWidget<Column>(std::move(widgets));
}

/// 建造辅助：横向 Flex 图纸。
template <typename... Widgets>
std::unique_ptr<Widget> row(Widgets&&... widgets) {
    return makeWidget<Row>(widgetList(std::forward<Widgets>(widgets)...));
}

inline std::unique_ptr<Widget> row(std::vector<std::unique_ptr<Widget>> widgets) {
    return makeWidget<Row>(std::move(widgets));
}

// ---- 小写工厂：一行画一张常用图纸（HomePage 的 build 全靠它们）----

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
