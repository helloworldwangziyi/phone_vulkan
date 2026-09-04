#include "detail_page.h"

#include "screen_metrics.h"
#include "app_fonts.h"
#include "app_theme.h"
#include "evk/log.h"
#include "evk/ui/navigation/navigation_stack.h"
#include "evk/ui/widgets.h"

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

    /// 层级说明卡：圆角矩形 + 描边 + 居中文字（编号数字来自 Roboto，
    /// 中文来自 NotoSansSC——一行内回退混排）。
    const float captionFontSize = appCalcHeight(30.0f);
    const float captionRadius = appCalcHeight(24.0f);
    std::string captionText =
        std::string("Layer #") + std::to_string(variant) + " · 渲染层详情";
    auto caption = padding(
        EdgeInsets::only(appCalcWidth(100.0f), appCalcHeight(20.0f),
                         appCalcWidth(100.0f), 0.0f),
        sizedBox(
            -1.0f, appCalcHeight(110.0f),
            container(0, {},
                      [text = std::move(captionText), fill = theme.surface,
                       border = theme.accent, color = theme.textPrimary,
                       fontSize = captionFontSize, radius = captionRadius,
                       font = appFonts::cjk()](PaintContext& paint) {
                          const Size size = paint.size();
                          const Rect bounds = {0.0f, 0.0f, size.width, size.height};
                          paint.drawRoundRect(bounds, radius, fill);
                          paint.strokeRoundRect(bounds, radius, 3.0f, border);
                          float textWidth = 0.0f;
                          float textHeight = 0.0f;
                          evk::ui::FontEngine::instance().measureText(
                              text.c_str(), fontSize, font, &textWidth, &textHeight);
                          paint.drawText(text.c_str(), font,
                                         (size.width - textWidth) * 0.5f,
                                         (size.height - textHeight) * 0.5f,
                                         fontSize, color);
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
                            0.0f, 0.0f, appCalcWidth(20.0f), 0.0f),
                        card(first))),
                    expanded(padding(
                        EdgeInsets::only(
                            appCalcWidth(20.0f), 0.0f, 0.0f, 0.0f),
                        card(first + 1))))));
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
        std::move(caption),
        cardRow(0),
        cardRow(2),
        std::move(pushButton)));
    page->color = theme.windowBackground;
    return page;
}
