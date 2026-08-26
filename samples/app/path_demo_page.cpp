#include "path_demo_page.h"

#include <cmath>

#include "screen_metrics.h"
#include "app_fonts.h"
#include "app_theme.h"
#include "evk/ui/font_engine.h"
#include "evk/ui/path.h"
#include "evk/ui/widgets.h"

namespace {

constexpr float kPi = 3.14159265358979323846f;

/// 在归一化 100×100 空间内构建心形路径（三次贝塞尔）。
evk::ui::Path makeHeart() {
    evk::ui::Path p;
    p.moveTo(50.0f, 15.0f)
        .cubicTo(50.0f, 0.0f, 10.0f, 0.0f, 10.0f, 30.0f)
        .cubicTo(10.0f, 55.0f, 50.0f, 75.0f, 50.0f, 90.0f)
        .cubicTo(50.0f, 75.0f, 90.0f, 55.0f, 90.0f, 30.0f)
        .cubicTo(90.0f, 0.0f, 50.0f, 0.0f, 50.0f, 15.0f)
        .close();
    return p;
}

/// 构建五角星路径（凹多边形，10 个顶点，用于验证 ear clipping）。
evk::ui::Path makeStar(float cx, float cy, float outerR, float innerR) {
    evk::ui::Path p;
    for (int i = 0; i < 10; ++i) {
        const float angle = (-90.0f + i * 36.0f) * kPi / 180.0f;
        const float r = (i % 2 == 0) ? outerR : innerR;
        const float x = cx + r * std::cos(angle);
        const float y = cy + r * std::sin(angle);
        if (i == 0) {
            p.moveTo(x, y);
        } else {
            p.lineTo(x, y);
        }
    }
    p.close();
    return p;
}

/// 在指定区域居中绘制文字。
void drawCenteredText(evk::ui::PaintContext& paint, const char* text,
                      float cx, float cy, float fontSize, uint32_t color,
                      int32_t font) {
    float tw = 0.0f, th = 0.0f;
    evk::ui::FontEngine::instance().measureText(text, fontSize, font, &tw, &th);
    paint.drawText(text, font, cx - tw * 0.5f, cy - th * 0.5f, fontSize, color);
}

} // namespace

std::unique_ptr<evk::ui::Widget> PathDemoPage::build(
    evk::ui::BuildContext&) const {
    using namespace evk::ui;
    const AppTheme& theme = appTheme();

    // ---- 标题 ----
    auto title = padding(
        EdgeInsets::only(appCalcWidth(100), appCalcHeight(60), appCalcWidth(100), 0),
        text("矢量路径演示", appCalcHeight(56), theme.textPrimary,
             appFonts::cjkBold()));

    auto subtitle = padding(
        EdgeInsets::only(appCalcWidth(100), appCalcHeight(8), appCalcWidth(100), 0),
        text("Path · 贝塞尔曲线 / 凹多边形三角化", appCalcHeight(28),
             theme.textSecondary));

    // ---- 主展示卡片 ----
    auto canvas = sizedBox(
        -1.0f, appCalcHeight(900),
        container(theme.surface, {}, [](PaintContext& paint) {
            const AppTheme& t = appTheme();
            const Size s = paint.size();
            const float padX = s.width * 0.06f;
            const float halfW = s.width * 0.5f;
            const float topH = s.height * 0.52f;
            const float iconSize = std::min(halfW - padX * 2.0f, topH * 0.7f);
            const float labelY = topH + appCalcHeight(30);
            const float labelFont = appCalcHeight(26);

            // --- 左上：心形（三次贝塞尔填充）---
            {
                const float cx = halfW * 0.5f;
                const float cy = topH * 0.42f;
                const float sc = iconSize / 100.0f;
                paint.drawPath(
                    makeHeart().scaled(sc, sc).translated(cx - iconSize * 0.5f,
                                                          cy - iconSize * 0.45f),
                    0xFFE74C3C);
                drawCenteredText(paint, "cubicTo 填充", cx, labelY, labelFont,
                                 t.textSecondary, appFonts::cjk());
            }

            // --- 右上：五角星（凹多边形填充，ear clipping）---
            {
                const float cx = halfW + halfW * 0.5f;
                const float cy = topH * 0.42f;
                const float r = iconSize * 0.42f;
                paint.drawPath(makeStar(cx, cy, r, r * 0.42f), 0xFFF1C40F);
                drawCenteredText(paint, "凹多边形三角化", cx, labelY, labelFont,
                                 t.textSecondary, appFonts::cjk());
            }

            // --- 下半部分：两条贝塞尔曲线描边 ---
            const float curveTop = s.height * 0.62f;
            const float curveH = s.height * 0.28f;

            // 二次贝塞尔（绿色）
            {
                const float x0 = padX;
                const float x1 = s.width - padX;
                const float yBase = curveTop;
                Path q;
                q.moveTo(x0, yBase)
                    .quadTo((x0 + x1) * 0.5f, yBase - curveH, x1, yBase);
                paint.strokePath(q, appCalcHeight(6), 0xFF2ECC71);
                drawCenteredText(paint, "quadTo 描边", s.width * 0.5f,
                                 curveTop + curveH + appCalcHeight(40),
                                 labelFont, t.textSecondary, appFonts::cjk());
            }

            // 三次贝塞尔（蓝色，S 形）
            {
                const float x0 = padX;
                const float x1 = s.width - padX;
                const float yBase = curveTop + curveH + appCalcHeight(80);
                Path c;
                c.moveTo(x0, yBase)
                    .cubicTo(x0 + (x1 - x0) * 0.25f, yBase - curveH * 0.8f,
                             x0 + (x1 - x0) * 0.75f, yBase + curveH * 0.8f,
                             x1, yBase);
                paint.strokePath(c, appCalcHeight(6), 0xFF3498DB);
            }
        }));

    auto canvasCard = padding(
        EdgeInsets::only(appCalcWidth(100), appCalcHeight(40), appCalcWidth(100), 0),
        std::move(canvas));

    // ---- 底部说明 ----
    auto note = padding(
        EdgeInsets::only(appCalcWidth(100), appCalcHeight(30), appCalcWidth(100),
                         appCalcHeight(60)),
        text("心形由 4 段三次贝塞尔组成；五角星是凹多边形，由 ear clipping "
             "算法三角化；曲线按曲率自适应细分，平直段少分、弯曲段多分。",
             appCalcHeight(26), theme.textSecondary, appFonts::cjk()));

    auto page = std::make_unique<Column>(widgetList(
        std::move(title), std::move(subtitle), std::move(canvasCard),
        std::move(note)));
    page->color = theme.windowBackground;
    return page;
}
