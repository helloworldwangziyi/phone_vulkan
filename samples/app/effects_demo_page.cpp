#include "effects_demo_page.h"

#include <vector>

#include "screen_metrics.h"
#include "app_fonts.h"
#include "app_theme.h"
#include "evk/ui/font_engine.h"
#include "evk/ui/widgets.h"

namespace {

/// 在卡片中央画一行标签文字。
void drawLabel(evk::ui::PaintContext& paint, const char* label,
               float fontSize, uint32_t color, int32_t font) {
    const evk::ui::Size size = paint.size();
    float tw = 0.0f, th = 0.0f;
    evk::ui::FontEngine::instance().measureText(label, fontSize, font, &tw, &th);
    paint.drawText(label, font, (size.width - tw) * 0.5f,
                   (size.height - th) * 0.5f, fontSize, color);
}

/// 两个 0xRRGGBBAA 颜色按 t∈[0,1] 逐通道插值（连续渐变底料用）。
uint32_t lerpColor(uint32_t a, uint32_t b, float t) {
    const auto ch = [a, b, t](int shift) -> uint32_t {
        const float va = static_cast<float>((a >> shift) & 0xFF);
        const float vb = static_cast<float>((b >> shift) & 0xFF);
        return static_cast<uint32_t>(va + (vb - va) * t);
    };
    return (ch(24) << 24) | (ch(16) << 16) | (ch(8) << 8) | ch(0);
}

/// 造一张带阴影的圆角卡片（Container 直造，shadow 字段赋值）。
std::unique_ptr<evk::ui::Widget> shadowCard(const AppTheme& theme,
                                            const char* label, float dy,
                                            float blur) {
    auto card = evk::ui::makeWidget<evk::ui::Container>(theme.surface);
    auto* config = static_cast<evk::ui::Container*>(card.get());
    config->cornerRadius = appCalcHeight(24);
    config->shadow = evk::ui::BoxShadow{0x00000099, 0.0f, appCalcHeight(dy),
                                        appCalcHeight(blur), -1.0f};
    config->painter = [label, theme](evk::ui::PaintContext& paint) {
        drawLabel(paint, label, appCalcHeight(28), theme.textSecondary,
                  appFonts::cjk());
    };
    return evk::ui::sizedBox(appCalcWidth(380), appCalcHeight(180),
                             std::move(card));
}

class EffectsDemoPageState final : public evk::ui::State {
public:
    std::unique_ptr<evk::ui::Widget> build(evk::ui::BuildContext&) override {
        using namespace evk::ui;
        const AppTheme& theme = appTheme();

        auto title = padding(
            EdgeInsets::only(appCalcWidth(100), appCalcHeight(60),
                             appCalcWidth(100), 0),
            text("渲染特效", appCalcHeight(56), theme.textPrimary,
                 appFonts::cjkBold()));
        auto subtitle = padding(
            EdgeInsets::only(appCalcWidth(100), appCalcHeight(8),
                             appCalcWidth(100), 0),
            text("阴影 · 圆角裁剪 · 背景模糊（SDF 管线 + 离屏通道）",
                 appCalcHeight(28), theme.textSecondary));

        // ---- 阴影：两种羽化半径对比 ----
        auto shadowRow = padding(
            EdgeInsets::only(appCalcWidth(100), appCalcHeight(50),
                             appCalcWidth(100), 0),
            row(widgetList(
                shadowCard(theme, "elevation 低", 6.0f, 10.0f),
                sizedBox(appCalcWidth(60), -1.0f, nullptr),
                shadowCard(theme, "elevation 高", 14.0f, 30.0f))));

        // ---- 圆角裁剪：渐变 + 出界圆被 ClipRRect 削角 ----
        auto clipDemo = padding(
            EdgeInsets::only(appCalcWidth(100), appCalcHeight(50),
                             appCalcWidth(100), 0),
            clipRRect(
                appCalcHeight(32),
                sizedBox(-1.0f, appCalcHeight(260),
                         container(0, {},
                                   [](PaintContext& paint) {
                                       const Size s = paint.size();
                                       paint.drawRectGradient(
                                           {0, 0, s.width, s.height},
                                           0xFF5856D6FF, 0xFFFF375FFF, true);
                                       // 四角的圆故意探出边界：被圆角裁掉才直观。
                                       paint.drawCircle(0, 0, appCalcHeight(90),
                                                        0xFF00C7BEFF);
                                       paint.drawCircle(s.width, s.height,
                                                        appCalcHeight(110),
                                                        0xFFFFD60AFF);
                                   }))));

        // ---- 背景模糊：连续渐变滚动带（无硬接缝）+ 悬浮毛玻璃卡片 ----
        // 注意：内容若有高对比硬边，扫过卡片时会被模糊涂抹成显眼的条带
        // （背景模糊的正确行为），所以滚动底料特意做成无接缝连续渐变。
        auto stripes = column(widgetList(
            sizedBox(-1.0f, appCalcHeight(1400),
                     container(0, {},
                               [](PaintContext& paint) {
                                   const Size s = paint.size();
                                   // 连续色相渐变：按行插值多个色站，全程无硬边。
                                   static const uint32_t stops[] = {
                                       0x5856D6FF, 0xAF52DEFF, 0xFF375FFF,
                                       0xFF9F0AFF, 0xFFD60AFF, 0x30D158FF,
                                       0x00C7BEFF, 0x0A84FFFF, 0x5856D6FF,
                                   };
                                   constexpr int kStops =
                                       sizeof(stops) / sizeof(stops[0]) - 1;
                                   const float bandH = s.height / 64.0f;
                                   for (int i = 0; i < 64; ++i) {
                                       const float f0 =
                                           static_cast<float>(i) / 64.0f * kStops;
                                       const float f1 =
                                           static_cast<float>(i + 1) / 64.0f * kStops;
                                       const int i0 = static_cast<int>(f0);
                                       const int i1 = static_cast<int>(f1);
                                       const uint32_t c0 = lerpColor(
                                           stops[i0], stops[i0 + 1], f0 - i0);
                                       const uint32_t c1 = lerpColor(
                                           stops[i1], stops[i1 + 1], f1 - i1);
                                       paint.drawRectGradient(
                                           {0.0f, bandH * i, s.width, bandH + 1.0f},
                                           c0, c1, false);
                                   }
                                   // 两枚柔和光斑，让模糊效果更直观。
                                   paint.drawCircle(s.width * 0.2f, s.height * 0.3f,
                                                    appCalcHeight(160), 0xFFFFFF66);
                                   paint.drawCircle(s.width * 0.8f, s.height * 0.7f,
                                                    appCalcHeight(200), 0xFFFFFF55);
                               }))));

        auto blurCard = center(backdropBlur(
            18.0f, appCalcHeight(28),
            sizedBox(appCalcWidth(560), appCalcHeight(200),
                     container(0xFFFFFF40, {},
                               [theme](PaintContext& paint) {
                                   drawLabel(paint, "毛玻璃 BackdropBlur",
                                             appCalcHeight(32),
                                             theme.textPrimary,
                                             appFonts::cjkBold());
                               }))));

        auto blurDemo = padding(
            EdgeInsets::only(appCalcWidth(100), appCalcHeight(50),
                             appCalcWidth(100), appCalcHeight(60)),
            sizedBox(-1.0f, appCalcHeight(700),
                     stack(widgetList(
                         scrollView(column(std::move(stripes)),
                                    appCalcHeight(1400),
                                    [](float, float) {}),
                         std::move(blurCard)))));

        auto page = std::make_unique<Column>(widgetList(
            std::move(title), std::move(subtitle), std::move(shadowRow),
            std::move(clipDemo), std::move(blurDemo)));
        page->color = theme.windowBackground;
        return page;
    }
};

} // namespace

std::unique_ptr<evk::ui::State> EffectsDemoPage::createState() const {
    return std::make_unique<EffectsDemoPageState>();
}
