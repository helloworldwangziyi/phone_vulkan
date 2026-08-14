#pragma once

// ============================================================================
// 首页（声明式 Component）：渐变面板 + 详情页/主题按钮 + 模拟行情列表。
//
// 页面 = Component 子类：成员变量即页面状态，build() 返回 Widget 描述树。
// 框架把「描述树 → 视图树」的差异应用（reconcile）藏了起来，所以页面里
// 没有 create/layout/destroy 样板：改了状态调 setState()，框架自动 diff
// 出最小视图改动并请求重绘。
//
// 本页演示的四个模式（对应框架的四个能力）：
//   1. 布局交给容器     —— column() 是纵向 Flex，子节点用 layout 参数声明
//                          排布（main/weight/cross/align/margin），App 不写
//                          layout 函数；resize 由 Flex 级联重排。
//   2. 换肤走事件总线   —— 主题切换广播 kEventThemeChanged，页面 listen 到
//                          就 setState() 重跑 build()（重读 token），
//                          reconcile 就地更新颜色。
//   3. 异步数据 + 取消  —— onDidEnter 发起模拟网络请求（后台线程 600ms），
//                          数据经 postUi 回 UI 线程 setState()；onWillLeave
//                          置取消标志，迟到数据丢弃。这是「后台数据源 →
//                          UI 线程」的标准写法。
//   4. 导航             —— pushPage(nav(), ...) 压入详情页；pop 后本页的
//                          Component 由框架自动销毁，无需清理 records。
// ============================================================================

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "evk/ui/widget.h"

class HomePage : public evk::ui::Component {
public:
    HomePage();
    ~HomePage() override;

    // 声明式核心：整棵页面的 UI 描述。每次 setState() 都会重跑，
    // 返回值交给框架 reconcile（同位置同类型就地更新，不同类型销毁重建）。
    std::unique_ptr<evk::ui::Widget> build() override;

    // ---- 页面生命周期（由 Navigation 在 push/pop/左滑返回时触发）----
    // onDidEnter：进入台前（push 转场结束后）——发起数据请求的时机。
    void onDidEnter(bool forward) override;
    // onWillLeave：将离开台前（被覆盖或将 pop）——取消请求的时机；
    // 返回 false 可拦截本次导航（如未保存表单）。
    bool onWillLeave(bool forward) override;

private:
    // 页面状态。build() 里只读这些成员，改它们必须走 setState()。
    bool panelAccent_ = false;             // 顶部面板强调色开关（点击切换）
    int detailCount_ = 0;                  // push 过的详情页数量（层序号）
    bool quoteLoaded_ = false;             // 模拟行情数据是否已到达
    bool quotePending_ = false;            // 请求是否进行中（防重复发起）
    std::vector<uint32_t> quotes_;         // 模拟行情数据（颜色行）
    std::shared_ptr<std::atomic_bool> quoteCancel_; // 请求取消标志（跨线程共享）
};
