# 声明式 UI 入门

当前 SDK 采用 Flutter 的核心分层，并且是纯 C++ API。App 不接触 View 指针、
句柄、函数指针回调或 user_data。

## 1. Flutter 实际解决了什么

Flutter 不是把所有功能都做成控件，而是把职责拆成三棵树：

| 层 | 本 SDK | 职责 |
|---|---|---|
| 不可变描述 | Widget | 表达“这一刻 UI 应该是什么样” |
| 挂载身份 | Element | 决定复用或替换、持有 BuildContext 和 State |
| 渲染对象 | View | 布局、绘制、命中测试、滚动与转场 |

Widget 可以频繁重建，因为它不保存滚动位置、按下状态等运行时数据。Element
按“同位置、同类型”更新 Widget；对应的 View 对象不变，因此内部状态自然保留。

## 2. StatefulWidget 与 State

页面配置和可变状态分开：

~~~cpp
class HomePage final : public evk::ui::StatefulWidget {
public:
    std::unique_ptr<evk::ui::State> createState() const override;
};

class HomeState final : public evk::ui::State {
public:
    std::unique_ptr<evk::ui::Widget> build(
        evk::ui::BuildContext& context) override {
        using namespace evk::ui;
        return column(
            expanded(container(active_ ? 0x22C55EFF : 0x334155FF)),
            center(sizedBox(
                320.0f,
                96.0f,
                button({}, [this] {
                    setState([this] { active_ = !active_; });
                }))));
    }

private:
    bool active_ = false;
};
~~~

setState 只做三件事：

1. 执行状态修改闭包；
2. 重新调用当前 State 的 build；
3. 由 Element 对新旧 Widget 做同类型就地更新。

App 不调用 reconcile，也不管理 View 的创建和销毁。

## 3. 布局是 Widget 组合

App 不再填写仅在某种父容器下才生效的 layout 字段。尺寸、留白、对齐和弹性
由独立 Widget 表达：

~~~cpp
return column(
    padding(
        EdgeInsets::symmetric(32.0f, 16.0f),
        sizedBox(-1.0f, 180.0f, container(theme.surface))),
    expanded(
        padding(
            EdgeInsets::all(24.0f),
            scrollView(buildRows(), contentHeight))),
    center(
        sizedBox(320.0f, 96.0f, button(style, onPressed))));
~~~

对应关系：

| Flutter | 本 SDK |
|---|---|
| Column / Row | column / row |
| Expanded | expanded |
| SizedBox | sizedBox |
| Padding | padding + EdgeInsets |
| Center | center |
| Container | container |
| ScrollView | scrollView |

Flex 的 parent data 仍存在于渲染层，但只由 Expanded、SizedBox、Padding 和
Center 生成，App 不需要理解它的存储和下发规则。

## 4. 绘制

自定义绘制直接接收局部 PaintContext：

~~~cpp
container(
    theme.surface,
    {},
    [](PaintContext& paint) {
        const Size size = paint.size();
        paint.drawTriangle(
            size.width * 0.5f, 0.0f,
            size.width, size.height,
            0.0f, size.height,
            0x6366F1FF, 0x22D3EEFF, 0xA78BFAFF);
    })
~~~

PaintContext 自动处理局部坐标、父级偏移和裁剪，不需要查询 View，也不存在
只能在回调期间调用的全局绘制函数。

## 5. 导航

Navigator 从 BuildContext 获取：

~~~cpp
Navigator::of(context).push(makeWidget<DetailPage>(42), true);
Navigator::of(context).pop(true);
~~~

Navigator 持有每条 Route 的 Element 树。pop 转场完成后，Route 先 unmount，
再析构 State 和 View；App 不写清理表，也不接收裸页面句柄。

State 可按需覆写：

- onWillEnter：返回 false 可拒绝进入；
- onDidEnter：页面进入台前；
- onWillLeave：返回 false 可拦截离开；
- onDidLeave：页面离开台前；
- dispose：释放页面资源。

## 6. 事件与异步

State::listen 创建 RAII 订阅，State 卸载时自动取消。带页面 scope 的订阅只在
页面可见时接收事件。

后台线程通过 postUi 投递闭包：

~~~cpp
postUi([this] {
    if (!mounted()) {
        return;
    }
    setState([this] { loaded_ = true; });
});
~~~

## 7. 根应用

平台上报尺寸后设置 viewport，在渲染器就绪时运行首页：

~~~cpp
setViewportSize(width, height);
runApp(
    makeWidget<HomePage>(),
    {navigationBarHeight, navigationStyle});
~~~

Surface 销毁时调用 shutdownApp。它会取消输入和动画、卸载 Route、释放
Element/State/View，并清空事件订阅。

## 8. 阅读顺序

1. core/include/evk/ui/widget_tree.h
2. core/src/ui/widget_tree.cpp
3. core/include/evk/ui/navigation/navigation_stack.h
4. samples/app/home_page.cpp
5. tests/ui_runtime_test.cpp
