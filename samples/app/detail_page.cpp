/// @file detail_page.cpp
/// ============================================================================
/// 详情页实现：布局全部声明在 build() 里（Column/Row 嵌套即视图树）。
///
/// 实例状态只有 variant_；页面销毁由 Navigation/框架负责，无需 records 清理。
/// 一个 DetailPage 实例对应导航栈里的一层页面：push 时 build 一次挂载，
/// pop 转场结束后框架销毁视图树 + Component 本身。
/// ============================================================================

#include "detail_page.h"

#include <vector>

#include "app_metrics.h"
#include "app_theme.h"
#include "evk/log.h"

namespace {

/// 主视觉渐变三角：每层详情页按 variant 轮换配色（同旧 drawHero）。
/// draw callback 内只能 esx_draw_*（写当前帧 Canvas），不能改视图树。
void drawHeroGradient(esx_view view, int variant) {
    const AppTheme& theme = appTheme();
    uint32_t colors[3];
    switch (variant % 3) { ///< 三层一轮换：连续 push 时每层主视觉不同
        case 0:
            colors[0] = theme.panelGradient[2];
            colors[1] = theme.panelGradient[1];
            colors[2] = theme.accent;
            break;
        case 1:
            colors[0] = theme.accent;
            colors[1] = theme.panelGradient[0];
            colors[2] = theme.panelAccent;
            break;
        default:
            colors[0] = theme.panelGradient[1];
            colors[1] = theme.panelAccent;
            colors[2] = theme.panelGradient[0];
            break;
    }
    /// 三顶点不同色，GPU 插值出渐变。
    esx_draw_triangle(view,
                      appCalcWidth(440.0f), appCalcHeight(30.0f),
                      appCalcWidth(800.0f), appCalcHeight(330.0f),
                      appCalcWidth(80.0f), appCalcHeight(330.0f),
                      colors[0], colors[1], colors[2]);
}

} ///< namespace

std::unique_ptr<evk::ui::Widget> DetailPage::build() {
    using namespace evk::ui;
    const AppTheme& theme = appTheme();

    /// ---- ① 顶部主视觉：渐变三角 ----
    auto hero = Box(theme.surface);
    /// 捕获 variant 副本：draw callback 的生命周期跨越多次 rebuild，
    /// 不能捕获 this（页面可能已被销毁），捕获值最安全。
    hero.onDraw = [variant = variant_](esx_view view) {
        drawHeroGradient(view, variant);
    };
    hero.layout.main = appCalcHeight(400.0f);
    hero.layout.marginMainBefore = appCalcHeight(60.0f);
    hero.layout.marginCross = appCalcWidth(100.0f);

    /// ---- ② 2×2 色卡：两行 Row，每行两个等宽 Box，卡间 40px 间距 ----
    /// card(i)：一个色卡 Box。.flex(1) = weight=1：在横向 Row 里吃掉
    /// 剩余主轴空间的一半（两个 flex(1) 平分）。
    auto card = [&](int i) {
        return Box(theme.scrollItems[(variant_ * 3 + i) % 8]).flex(1.0f);
    };
    /// 第一行：card0 与 card1。card(0) 额外声明了主轴后间距 40px（卡间缝）。
    /// 行自身：固定高度 170px + 左右边距 100px + 与上方内容间距 40px。
    /// 注意链式修饰是 rvalue 专用（WidgetT）：Box(...).flex(1) 返回右值引用。
    auto cards01 = row(card(0).marginMain(0.0f, appCalcWidth(40.0f)), card(1))
                       .mainSize(appCalcHeight(170.0f))
                       .marginCross(appCalcWidth(100.0f))
                       .marginMain(appCalcHeight(40.0f), 0.0f);
    /// 第二行：card2 与 card3，样式与第一行相同。
    auto cards23 = row(card(2).marginMain(0.0f, appCalcWidth(40.0f)), card(3))
                       .mainSize(appCalcHeight(170.0f))
                       .marginCross(appCalcWidth(100.0f))
                       .marginMain(appCalcHeight(40.0f), 0.0f);

    /// ---- ③ 继续 push：层序号 +1 ----
    /// 每次点按都压入一个新的详情页实例（variant+1），可无限嵌套；
    /// 返回用导航栏返回按钮或左缘 48px 内右滑（Navigation 自带）。
    const esx_button_style buttonStyle{theme.primary, theme.primaryPressed,
                                       theme.primaryDisabled};
    auto pushButton = ButtonW(buttonStyle);
    pushButton.onTap = [this] {
        EVK_LOGI("detail page pushes another detail page");
        pushPage(nav(), std::make_unique<DetailPage>(variant_ + 1), true);
    };
    pushButton.layout.main = appCalcHeight(140.0f);
    pushButton.layout.cross = appCalcWidth(400.0f);
    pushButton.layout.align = ESX_FLEX_ALIGN_CENTER;
    pushButton.layout.marginMainBefore = appCalcHeight(40.0f);

    /// ---- ④ 组装：外层纵向 Column 从上到下排 hero → cards01 → cards23 → 按钮 ----
    auto page = column(std::move(hero), std::move(cards01), std::move(cards23),
                       std::move(pushButton));
    page.color = theme.windowBackground;
    return makeWidget(std::move(page));
}
