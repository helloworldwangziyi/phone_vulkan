# 声明式 UI 入门

> 本文讲解 core 的声明式 UI 层（`evk/ui/widget.h` 一族）：为什么这样设计、
> 三个核心概念、`widget.h` 逐段精读、一次状态变更的完整旅程，以及与 Flutter
> 的对照。面向「会写 App 页面、还不熟声明式机制」的读者。
>
> 阅读前提：先读「示例应用导读」第 4 节（HomePage 的写法），再回来看机制；
> 本文所有机制最终都落到 `samples/app/main_page.cpp` 上。

## 1. 为什么是声明式

传统命令式 UI 是「告诉电脑怎么做」：自己 create 视图、自己 layout、
数据到了自己删旧行建新行。**状态和界面是两份数据，必须手动保证同步**，
页面越复杂，漏同步的 bug 越多。

声明式 UI 的核心思想只有一句话：

> **UI = f(state)**：界面永远是「当前状态」的函数。

App 只维护状态；状态一变，就把整个界面**重新描述**一遍；框架对比新旧
描述，把差异（diff）应用到真实视图上。

```
你的代码                        框架替你做的
──────────                    ──────────────────
成员变量 = 状态（唯一真相）
build() = 由状态生成描述树      reconcile：diff 新旧描述
setState() = "状态变了"         → 最小视图改动（增/删/改）
                               → 排布布局
                               → requestRender → 渲染
```

## 2. 三个核心概念（对照 Flutter）

整套机制只有三个概念，本项目与 Flutter 一一对应：

| 概念 | 本项目 | Flutter | 本质 | 生命周期 |
|---|---|---|---|---|
| **描述** | `evk::ui::Widget` | `Widget` | 「应该长什么样」的不可变描述（购物清单） | 每次 build 都是新的，用完即弃 |
| **挂载** | `evk::ui::Element` | `Element` | 描述与真实视图之间的挂载记录，持有真实视图 | 跨多次 build 存活 |
| **状态** | `evk::ui::Component` | `StatefulWidget`+`State` | 可变状态 + build() + setState() | 随页面存活 |

关键差异：**本项目没有独立的 RenderObject 层**。Flutter 是
Widget → Element → RenderObject 三层；这里 Element 直接持有一个 retained
视图句柄（`esx_view`），渲染即遍历这棵 retained 视图树。所以本项目的
reconcile = Flutter 的「Element diff + RenderObject update」合并。

## 3. `widget.h` 逐段精读

抽象感来自两点：这是纯框架文件（没有业务逻辑）；以及「描述树与真实视图
是两棵树」。读法：每段都问「**谁会在什么时候调用它？**」

### 3.1 `FlexSpec`：寄存在描述上的快递单

```cpp
struct FlexSpec {
    float main = -1.0f;          // 主轴固定 px；<0 由 weight 决定
    float weight = 0.0f;         // >0 按比例分主轴剩余空间（Expanded）
    float cross = -1.0f;         // 交叉轴固定 px；<0 stretch 占满
    int32_t align = 0;           // esx_flex_align（cross≥0 时生效）
    float marginMainBefore = 0.0f;
    float marginMainAfter = 0.0f;
    float marginCross = 0.0f;
};
```

它本身没有任何作用——只是每个 Widget 随身携带的快递单，等**父容器
（Flex）来读**：`ColumnW::configureChild` 在孩子挂载后把这张单子翻译成
`esx_flex_set_child` 的参数。所以 `layout` 只在 Flex 父容器里生效，
挂在别处（如 Navigation 内容区）就没人读（头注释「非 Flex 父容器忽略」）。

### 3.2 `Slot` / `ScrollSlot`：回调的信箱

```cpp
struct Slot { std::function<void(esx_view)> fn; };
struct ScrollSlot { std::function<void(float, float)> fn; };
```

控件 C ABI 的回调是「函数指针 + `void* user_data`」，而 App 的 lambda 是
`std::function`，两者对不上。解法：

- 回调的 `std::function` 放进**堆上的信箱**（Slot）；
- 视图注册的回调指向**静态跳板函数**（trampoline），`user_data` 指向信箱；
- 事件来了 → 跳板取出信箱里的 `fn` → 调用。

关键收益：**重建时只换信箱里的信，不换信箱地址** → 视图不用重新注册
回调（`updateView` 只覆写槽内容）。Box 约定 `slots[0]=点击、slots[1]=绘制`
（widget.cpp `Box::createView`），且**只有 onTap 非空才注册点击回调**——
这就是「无 onTap 的 Box 不成为触控目标」的出处。

### 3.3 `Element`：最抽象的一环

```cpp
struct Element {
    std::unique_ptr<Widget> widget;              // 最近一次描述（供下次 diff）
    esx_view view = 0;                           // 真实视图句柄（跨重建不变）
    std::vector<std::unique_ptr<Element>> children;
    std::vector<std::unique_ptr<Slot>> slots;
    std::vector<std::unique_ptr<ScrollSlot>> scrollSlots;
};
```

一句话：**Widget 树是图纸，Element 树是已经建成的大楼**。每次 `setState`
画一张新图纸，reconcile 对照旧图纸（`Element.widget`）找出哪面墙要刷、
哪间房要拆，改的是大楼（`Element.view`）。

「滚动位置保留」不是特意做的功能，而是「没动的东西就不动」这个 diff
原则的自然结果：pop 回来时 `list` 节点没换，`list->view` 还是那个
ScrollView 句柄，内部状态自然还在。

### 3.4 `Widget` 基类：六个被框架调用的方法

```cpp
virtual esx_view createView(Element& owner) const = 0;      // 怎么建真实视图
virtual void updateView(esx_view, Element&) const {}        // 同类型怎么就地改
virtual bool sameKind(const Widget& other) const {
    return typeid(*this) == typeid(other);                  // 类型相同即同类
}
virtual std::vector<std::unique_ptr<Widget>>& childSpecs(); // 孩子描述在哪
virtual esx_view childParent(esx_view view) const { return view; } // 孩子挂哪
virtual void configureChild(esx_view, const Widget&, esx_view) const {} // 孩子排布
```

这些方法几乎不被直接调用，全部由 `reconcile` 调用（**控制反转**）：

| 方法 | 谁调用 | 何时 | 本项目里谁重写 |
|---|---|---|---|
| `createView` | reconcile | 类型变了 / 新孩子 | Box、ButtonW、ColumnW、RowW、ScrollW |
| `updateView` | reconcile | 同位置同类型 | Box、ButtonW、ColumnW、ScrollW |
| `sameKind` | reconcile | 决定「改」还是「重建」 | （默认 typeid 足够） |
| `childSpecs` | reconcileChildren | 递归孩子时 | ColumnW（返回 kids_） |
| `childParent` | reconcileChildren | 决定孩子挂到哪个视图 | **ScrollW**（重定向到滚动 content） |
| `configureChild` | reconcileChildren | 孩子 reconcile 完后 | **ColumnW**（FlexSpec 写入 Flex 容器） |

`childParent` 的可重写性是嵌套规则自治的关键：`ScrollW` 说「我的孩子挂到
滚动 content 上」，reconcile 就不关心内容细节。

### 3.5 `WidgetT`：链式修饰的魔法

```cpp
template <typename Derived>
class WidgetT : public Widget {
public:
    Derived&& flex(float weight) && { layout.weight = weight; ...; return self(); }
    ...
private:
    Derived&& self() { return std::move(static_cast<Derived&>(*this)); }
};
```

两个 C++ 技巧解决两个问题：

- **`&&`（rvalue 限定）**：这些方法只能对**临时对象**调。`Box(c).mainSize(100)`
  合法；存进变量再调就编译不过。这是纪律：描述应一次性写完，不可反复修改。
- **CRTP（`Derived` 模板参数）**：`self()` 把 `this` 转成具体类型，链式调用
  返回的仍是 `Box&&` 而非 `Widget&&`——类型信息不丢失（`typeid`/`sameKind`
  靠它），后续链式调用也还在。

### 3.6 四个内置 Widget：四张视图说明书

| Widget | 包什么 | 重写的关键方法 |
|---|---|---|
| `Box` | 普通 `esx_view`（背景 + 点击 + 自定义绘制），≈ Container+GestureDetector+CustomPaint | `createView`：按 onTap 有无决定是否注册点击回调 |
| `ButtonW` | `esx_button` 控件（带 pressed 状态机） | `createView`/`updateView`：样式 + 回调槽 |
| `ColumnW`/`RowW` | `esx_flex` 容器 | `configureChild`：把孩子的 FlexSpec 翻译成 `esx_flex_child` |
| `ScrollW` | `ScrollFollow` 宿主 + `ScrollView` | `childParent`：孩子挂到滚动 content |

注意 **RowW 继承 ColumnW、只差构造时的 vertical 参数**：行为全同，靠
`typeid` 区分类型——同一位置 Column 换 Row 会触发销毁重建（合理：主轴
方向都变了）。

ScrollW 最复杂（widget.cpp `ScrollW::createView`）：先建一个 `ScrollFollow`
宿主视图（`handleBoundsChanged` 时同步 viewport/content/内容子视图的
bounds），再在里面建真正的 ScrollView。

### 3.7 构建辅助：让嵌套语法成立

```cpp
template <typename W> std::unique_ptr<Widget> makeWidget(W&& w);
template <typename... Kids> ColumnW column(Kids&&... kids);
template <typename... Kids> RowW row(Kids&&... kids);
template <typename T, typename F> std::vector<std::unique_ptr<Widget>> mapWidgets(...);
```

作用：把各种具体类型**擦成统一的 `unique_ptr<Widget>`**，让
`column(panel, button, list)` 这种异构参数能收进同一个 vector。
`mapWidgets` 即 Flutter 的 `items.map(...).toList()`。

### 3.8 `Component`：页面的基类

```cpp
class Component {
public:
    virtual std::unique_ptr<Widget> build() = 0;         // 状态 → 描述（唯一必写）
    virtual bool onWillEnter(bool) { return true; }      // false 取消导航
    virtual void onDidEnter(bool) {}
    virtual bool onWillLeave(bool) { return true; }      // false 拦截返回
    virtual void onDidLeave(bool) {}
    void setState(std::function<void()> mutate = nullptr);
    void listen(int32_t eventId, esx_event_priority, std::function<void(const void*)>);
    esx_view view() const { return root_ ? root_->view : 0; }
    esx_view nav() const { return nav_; }
private:
    esx_view nav_ = 0;
    std::unique_ptr<Element> root_;   // ← 页面的 Element 树根
    std::vector<Listener> listeners_;
};
```

= Flutter 的 `StatefulWidget + State` 合并：成员变量即状态，`build()` 即
build，`setState` 同名同义。`setState` 内部流程（widget.cpp
`Component::setState`）：

```cpp
if (mutate) mutate();                 // ① 改状态
if (!root_) return;
spec = build();                       // ② 重新描述
if (!root_->widget->sameKind(*spec)) { warn; return; }  // ③ 根类型必须稳定
reconcile(root_, std::move(spec), 0); // ④ diff 应用（parent=0：页面根自持）
```

第 ③ 步就是头注释规则「build 根 widget 类型必须稳定」的实现。

`listen` 的 scope 是页面根视图：**页面被 Navigation 覆盖时事件总线不派给
它**（隐式 pause）；Component 析构自动注销。`friend pushPage /
teardownAllComponents` 表明页面的生死只由框架管理。

### 3.9 `pushPage` / `teardownAllComponents`：页面的生死簿

```
pushPage(nav, make_unique<DetailPage>(3), true)
  ├─ build() → reconcile 挂载（此时是 parent=0 的游离树）
  ├─ 注册 nav 钩子（Navigation 8 态 → 转发成 Component 虚函数）
  ├─ 注册事件监听
  └─ esx_navigation_push → 转场 → 进栈
pop 转场结束 → componentPopHook：delete Component（析构注销监听、清回调槽）
            → Navigation 随后销毁视图树
```

注意：**Component 页面接管该 nav 的 `on_pop` 回调**，App 不要再调
`esx_navigation_set_on_pop`（写了会被覆盖）。

## 4. 一次 setState 的完整旅程

以首页「点击面板切换强调色」为例：

```cpp
panel.onTap = [this] { setState([this] { panelAccent_ = !panelAccent_; }); };
```

1. **改状态**：mutate 执行，`panelAccent_` false → true。这是唯一允许改状态
   的地方。
2. **重新描述**：`build()` 用新状态生成一棵**全新的** Widget 树。纯函数：
   同样状态永远生成同样描述。
3. **reconcile（diff 应用）**：逐节点对比新旧描述。本例所有节点类型都没变，
   全部走 `updateView` 就地更新（面板 Box 的 onDraw 槽换成捕获新
   `panelAccent_` 的 lambda）。**没有创建/销毁任何视图对象**——声明式重建
   便宜的原因：重建的是描述（轻量），不是视图（重量）。
4. **requestRender**：每次视图属性修改只置 dirty 标志。
5. **VSync → 渲染**：`beginFrame` 看 dirty → 帧构建（背景 → draw 回调 →
   子节点）→ Vulkan 上屏。

> ⚠️ 与 Flutter 的差异：本项目 `setState` 里 rebuild **同步立即**完成；
> Flutter 是标脏后延迟到下一帧 build 阶段。本项目渲染仍是 VSync 延迟的，
> 只是描述重建提前做了——所以 `build()` 必须便宜（别做重计算、别发请求）。

## 5. reconcile 规则细节（对齐 Flutter 无 key 语义）

**判据：同位置 + 同类型（`typeid`）**。本项目没有 key 机制。

行情列表数据到达（`quoteLoaded_` false → true，rows 从 [占位Box] 变
[Box ×20]）的 diff：

- 位置 0：旧占位 Box vs 新 Box → 同类型 → `updateView` 就地改色改尺寸
  （**这个视图原地复用**）；
- 位置 1~19：旧的没有 → `createView` 新建挂载；
- 位置 20+：旧的多出来 → teardown 销毁。

配套规则：

- **内部状态保留**是「同类型就地更新」的副产品：ScrollW 的 `updateView`
  只更新 contentHeight 与回调槽，滚动 offset 是 ScrollView 的内部状态，
  reconcile 不碰。
- **回调槽**：createView 注册一次，updateView 只覆写槽内容（user_data
  指针不变）。副作用是一条约定：同一位置 widget 的回调集合（有无
  onTap/onDraw）应在重建间保持稳定，新增回调类型不生效。
- **根类型稳定**：`setState` 检查 `sameKind`，根类型变化告警并忽略。

## 6. 布局：Flex 容器

`column()`/`row()` 包 `esx_flex` 容器（flex.cpp）。每个子节点的
`FlexSpec` 相当于 Flutter 的 parent data：

| FlexSpec 字段 | 语义 | Flutter 对照 |
|---|---|---|
| `main` | 主轴固定 px | `SizedBox(height:)`（Column 里） |
| `weight` | >0 按比例瓜分剩余主轴空间 | `Expanded(flex:)` |
| `cross` | 交叉轴固定 px | `SizedBox(width:)` |
| `align` | cross≥0 时交叉轴对齐 | `Align` / crossAxisAlignment |
| `margin*` | 间距 | `Padding` / `margin` |

排布算法（flex.cpp `layoutChildren`）：先扣固定项（main≥0）与全部 margin，
剩余空间按 weight 比例分给弹性项——首页列表 `list.layout.weight = 1`
「吃掉剩余空间」的原理。

两个省心机制：

- **App 不写 layout 函数**：挂载 / spec 变更 / 尺寸变化都触发
  `layoutChildren`。旋转屏幕 → SurfaceChanged 只改根视图 bounds →
  Flex 的 `handleBoundsChanged` 钩子级联重排整棵子树。
- **v1 规则**：主轴方向每个子视图必须 main≥0（固定）或 weight>0（弹性）
  二选一；不测量子视图固有尺寸。

## 7. 页面与导航：Component 的一生

生命周期钩子的典型用法（首页全部演示）：

- `onDidEnter`：发起请求 / 订阅数据；
- `onWillLeave`：取消请求 / 保存状态，**返回 false 可拦截导航**（如未保存
  表单）；
- `WILL` 与 `DID` 严格配对；转场被取消（左滑回弹、尺寸吸附）时按最终
  归属收尾。

## 8. 三条数据通道

| 通道 | 入口 | 线程/时机 | 用途 |
|---|---|---|---|
| 交互回调 | onTap/onDraw/onScroll | 同步、UI 线程 | 用户交互 |
| 事件总线 | `listen` / `esx_event_emit` | 同步、UI 线程 | App 内广播（换肤） |
| 跨线程 | `evk::ui::postUi` | 任意线程投递，beginFrame 开头执行 | 后台数据回 UI 线程 |

- `onDraw` 只在帧构建期间被调，只能用 `esx_draw_*` 写当前帧画布，**不能改
  视图树**（告警忽略）。
- `listen` scope 自动绑页面根视图：被覆盖收不到事件（隐式 pause），销毁
  自动注销。
- `postUi` 是唯一合法跨线程入口；配「取消标志」模式处理页面已销毁的迟到
  数据（首页 `quoteCancel_`）。

## 9. 全景图：HomePage 运行时内存里长什么样

```
你的代码（描述层，每次 setState 全新）        框架（挂载层，持续存活）
─────────────────────────────              ────────────────────────────
Component (HomePage)
  └─ build() → Widget 树                         Element 树（root_）
       ColumnW(page)  ──────── reconcile ────→  esx_view 1001 → FlexView
        ├─ Box(panel)                           ├─ 1002 → View (bg+draw回调)
        ├─ ButtonW(detail)                      ├─ 1003 → Button (pressed状态机)
        ├─ ButtonW(theme)                       ├─ 1004 → Button
        └─ ScrollW(list)                        └─ 1005 → ScrollFollow
             └─ ColumnW(rows)                        ├─ 1006 → ScrollView (offset!)
               ├─ Box(row0) ...                      └─ 1007 → content 视图
                                                         └─ 1008... 行视图
```

一次 `setState`（行情数据到达）：

1. 新描述树：`list` 节点类型没变 → `updateView`（改 contentHeight 与回调槽）
   ——1005/1006/1007 原地保留，滚动位置不动；
2. 行节点：位置 0 同类型 → 就地改色改高（1008 复用）；位置 1~19 新建挂载；
   位置 20+ 销毁；
3. 每次修改内部 requestRender → VSync 一次性上屏。

## 10. 与 Flutter 对照速查表

| Flutter | 本项目 | 差异/备注 |
|---|---|---|
| `Widget` | `evk::ui::Widget` | 无 key 机制，按类型+位置匹配 |
| `Element` | `Element` | 直接持视图句柄，无 RenderObject 层 |
| `StatefulWidget`/`State` | `Component` | 合并为一个类 |
| `build(BuildContext)` | `build()` | 根类型必须稳定 |
| `setState(fn)` | `setState(mutate)` | 同步 rebuild（Flutter 延迟到帧） |
| `Column`/`Row`/`Expanded` | `column()`/`row()`/`.flex(1)` | FlexSpec 即 parent data |
| `Container` | `Box` | 合并了 GestureDetector + CustomPaint |
| `ListView` | `ScrollW` | 单子节点、手动 contentHeight，无懒构建 |
| `Navigator.push` | `pushPage` | 页面销毁由导航栈管理 |
| `Theme`/`InheritedWidget` | `AppTheme` + 事件总线 | 广播通知，无依赖订阅 |
| `initState`/`dispose` | 构造/析构 + onDidEnter/onWillLeave | 请求放 DidEnter，取消放 WillLeave |
| `didUpdateWidget` | `Widget::updateView` | 就地更新的落点 |
| `RenderFlex` | `esx_flex` | v1 简化：不测子视图固有尺寸 |

## 11. 学习路线

1. `core/include/evk/ui/widget.h` 顶部大注释（设计文档就在代码里）→ 认识三
   概念与约定；
2. `core/src/ui/widget.cpp` 的 `reconcile` / `pushPage` / `setState`（核心
   约 100 行）→ 搞懂 diff 规则；
3. `samples/app/main_page.cpp` 对照 → 每个概念在真实页面里的用法；
4. `core/src/ui/controls/flex.cpp` 的 `layoutChildren` → 布局算法；
5. `tests/ui_runtime_test.cpp` → 每个行为都有断言，跑一遍验证理解。

## 12. 读框架文件的三条心法

1. **控制反转**：框架文件里没有入口函数，全是钩子。读任何方法先问
   「谁在什么时候调用它」。
2. **描述 vs 挂载是两棵树**：状态/滚动位置/视图句柄活在 Element 侧；
   颜色/尺寸/回调是描述侧每次重建的新值。
3. **拿测试当规格**：tests 里每个断言都是框架行为的可执行定义。

## 相关文件

| 文件 | 说明 |
|---|---|
| `core/include/evk/ui/widget.h` | 声明式层全部声明（本文精读对象） |
| `core/src/ui/widget.cpp` | reconcile / pushPage / setState / 回调槽实现 |
| `core/include/evk/ui/controls/flex.h` | Flex 容器 C ABI |
| `core/src/ui/controls/flex.cpp` | Flex 排布算法 |
| `core/include/evk/ui/event_bus.h` | 业务事件总线 + `esx_post_ui` |
| `samples/app/main_page.cpp` | 本文所有机制的应用范本 |
| `tests/ui_runtime_test.cpp` | 机制行为的可执行规格 |
