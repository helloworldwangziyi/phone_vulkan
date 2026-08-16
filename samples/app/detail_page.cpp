#include "detail_page.h"

#include "screen_metrics.h"
#include "app_theme.h"
#include "evk/log.h"
#include "evk/ui/navigation/navigation_stack.h"

std::unique_ptr<evk::ui::Widget> DetailPage::build(
    evk::ui::BuildContext& context) const {
    using namespace evk::ui;
    const AppTheme& theme = appTheme();
    const int variant = variant_;

    auto hero = padding(
        EdgeInsets::only(
            appCalcWidth(100.0f),
            appCalcHeight(60.0f),
            appCalcWidth(100.0f),
            0.0f),
        sizedBox(
            -1.0f,
            appCalcHeight(400.0f),
            container(
                theme.surface,
                {},
                [variant](PaintContext& paint) {
                    const AppTheme& currentTheme = appTheme();
                    uint32_t colors[3];
                    switch (variant % 3) {
                        case 0:
                            colors[0] = currentTheme.panelGradient[2];
                            colors[1] = currentTheme.panelGradient[1];
                            colors[2] = currentTheme.accent;
                            break;
                        case 1:
                            colors[0] = currentTheme.accent;
                            colors[1] = currentTheme.panelGradient[0];
                            colors[2] = currentTheme.panelAccent;
                            break;
                        default:
                            colors[0] = currentTheme.panelGradient[1];
                            colors[1] = currentTheme.panelAccent;
                            colors[2] = currentTheme.panelGradient[0];
                            break;
                    }
                    const Size size = paint.size();
                    paint.drawTriangle(
                        size.width * 0.5f, size.height * 0.08f,
                        size.width * 0.91f, size.height * 0.83f,
                        size.width * 0.09f, size.height * 0.83f,
                        colors[0], colors[1], colors[2]);
                })));

    auto card = [&](int index) {
        return container(theme.scrollItems[(variant * 3 + index) % 8]);
    };

    auto cardRow = [&](int first) {
        return padding(
            EdgeInsets::only(
                appCalcWidth(100.0f),
                appCalcHeight(40.0f),
                appCalcWidth(100.0f),
                0.0f),
            sizedBox(
                -1.0f,
                appCalcHeight(170.0f),
                row(
                    expanded(padding(
                        EdgeInsets::only(
                            0.0f, 0.0f, appCalcWidth(40.0f), 0.0f),
                        card(first))),
                    expanded(card(first + 1)))));
    };

    BuildContext* routeContext = &context;
    auto pushButton = padding(
        EdgeInsets::only(0.0f, appCalcHeight(40.0f), 0.0f, 0.0f),
        center(sizedBox(
            appCalcWidth(400.0f),
            appCalcHeight(140.0f),
            button(
                {theme.primary, theme.primaryPressed, theme.primaryDisabled},
                [routeContext, variant] {
                    EVK_LOGI("detail page pushes another detail page");
                    Navigator::of(*routeContext).push(
                        makeWidget<DetailPage>(variant + 1), true);
                }))));

    auto page = std::make_unique<Column>(widgetList(
        std::move(hero),
        cardRow(0),
        cardRow(2),
        std::move(pushButton)));
    page->color = theme.windowBackground;
    return page;
}
