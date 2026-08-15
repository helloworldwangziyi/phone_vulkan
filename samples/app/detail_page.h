#pragma once

/// @file detail_page.h
/// ============================================================================
/// 详情页（声明式 Component）：渐变主视觉 + 2×2 色卡 + 继续 push 按钮。
///
/// 比首页更纯粹的「结构即代码」示范：页面没有异步数据、没有生命周期钩子，
/// 实例状态只有一个 variant_（层序号，构造传入），build() 用
/// column/row/Box/ButtonW 直接拼出整棵视图树。
///
/// 演示点：
///   - 无限嵌套导航：按钮 onTap 里 pushPage 一个新的 DetailPage(variant+1)，
///     连续 push 时每层配色不同，转场/返回有感知；
///   - Row 等宽分栏：row() 内两个 .flex(1) 的 Box 平分主轴空间（Flex 布局）；
///   - 页面销毁：pop（返回按钮或左缘右滑）转场结束后，Navigation 回调
///     on_pop → 框架自动 delete 本 Component，App 无需写任何清理代码。
/// ============================================================================

#include <memory>

#include "evk/ui/widget.h"

class DetailPage : public evk::ui::Component {
public:
    explicit DetailPage(int variant) : variant_(variant) {}

    std::unique_ptr<evk::ui::Widget> build() override;

private:
    int variant_ = 0; ///< 层序号：决定本层渐变/色卡配色
};
