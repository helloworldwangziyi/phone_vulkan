// 详情页实现：布局全部声明在 build() 里（Column/Row 嵌套即视图树）。
// 实例状态只有 variant_；页面销毁由 Navigation/框架负责，无需 records 清理。

#include "detail_page.h"

#include <vector>

#include "app_metrics.h"
#include "app_theme.h"
#include "evk/log.h"

namespace {

// 主视觉渐变三角：每层详情页按 variant 轮换配色（同旧 drawHero）。
void drawHeroGradient(esx_view view, int variant) {
    const AppTheme& theme = appTheme();
    uint32_t colors[3];
    switch (variant % 3) {
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
    esx_draw_triangle(view,
                      appCalcWidth(440.0f), appCalcHeight(30.0f),
                      appCalcWidth(800.0f), appCalcHeight(330.0f),
                      appCalcWidth(80.0f), appCalcHeight(330.0f),
                      colors[0], colors[1], colors[2]);
}

} // namespace

std::unique_ptr<evk::ui::Widget> DetailPage::build() {
    using namespace evk::ui;
    const AppTheme& theme = appTheme();

    // 顶部主视觉：渐变三角。
    auto hero = Box(theme.surface);
    hero.onDraw = [variant = variant_](esx_view view) {
        drawHeroGradient(view, variant);
    };
    hero.layout.main = appCalcHeight(400.0f);
    hero.layout.marginMainBefore = appCalcHeight(60.0f);
    hero.layout.marginCross = appCalcWidth(100.0f);

    // 2×2 色卡：两行 Row，每行两个等宽 Box，卡间 40px 间距。
    auto card = [&](int i) {
        return Box(theme.scrollItems[(variant_ * 3 + i) % 8]).flex(1.0f);
    };
    auto cards01 = row(card(0).marginMain(0.0f, appCalcWidth(40.0f)), card(1))
                       .mainSize(appCalcHeight(170.0f))
                       .marginCross(appCalcWidth(100.0f))
                       .marginMain(appCalcHeight(40.0f), 0.0f);
    auto cards23 = row(card(2).marginMain(0.0f, appCalcWidth(40.0f)), card(3))
                       .mainSize(appCalcHeight(170.0f))
                       .marginCross(appCalcWidth(100.0f))
                       .marginMain(appCalcHeight(40.0f), 0.0f);

    // 继续 push：层序号 +1。
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

    auto page = column(std::move(hero), std::move(cards01), std::move(cards23),
                       std::move(pushButton));
    page.color = theme.windowBackground;
    return makeWidget(std::move(page));
}
