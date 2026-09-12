#include "gesture_demo_page.h"

#include <cmath>
#include <cstdio>
#include <string>

#include "screen_metrics.h"
#include "app_fonts.h"
#include "app_theme.h"
#include "evk/ui/font_engine.h"
#include "evk/ui/widgets.h"

namespace {

/// 最近一次点按类手势的种类（决定落点标记的颜色/形态）。
enum TapKind {
    kTapNone = 0,
    kTapSingle = 1,
    kTapDouble = 2,
    kTapLong = 3,
};

class GestureDemoPageState final : public evk::ui::State {
public:
    std::unique_ptr<evk::ui::Widget> build(
        evk::ui::BuildContext&) override {
        using namespace evk::ui;
        const AppTheme& theme = appTheme();

        auto title = padding(
            EdgeInsets::only(appCalcWidth(100), appCalcHeight(60),
                             appCalcWidth(100), 0),
            text("手势演示", appCalcHeight(56), theme.textPrimary,
                 appFonts::cjkBold()));

        auto subtitle = padding(
            EdgeInsets::only(appCalcWidth(100), appCalcHeight(8),
                             appCalcWidth(100), 0),
            text("点按 / 双击 / 长按，双指捏合旋转（模拟器：Ctrl+拖动）",
                 appCalcHeight(28), theme.textSecondary));

        // 状态行：手势回调里 setState 刷新（字号/宽度定值，单行确定）。
        auto status = sizedBox(
            -1.0f, appCalcHeight(120),
            padding(
                EdgeInsets::only(appCalcWidth(100), appCalcHeight(30),
                                 appCalcWidth(100), 0),
                text(status_.c_str(), appCalcHeight(30), theme.accent,
                     appFonts::cjk())));

        // ---- 手势画布：GestureDetector 包住整块卡片 ----
        // painter 按值捕获本帧状态（rebuild 后换新的 painter，无悬垂引用）。
        const TapKind kind = lastKind_;
        const float tapX = tapX_;
        const float tapY = tapY_;
        const float scale = scale_;
        const float rotation = rotation_;
        const float focalX = focalX_;
        const float focalY = focalY_;
        const int pointers = pointerCount_;

        auto card = sizedBox(
            -1.0f, appCalcHeight(1050),
            gestureDetector(
                container(theme.surface, {},
                    [kind, tapX, tapY, scale, rotation, focalX, focalY,
                     pointers](PaintContext& paint) {
                        const AppTheme& t = appTheme();
                        const Size s = paint.size();

                        // 点按落点：单击实心点 / 双击同心点 / 长按点+环。
                        if (kind != kTapNone) {
                            const uint32_t dotColor =
                                kind == kTapSingle   ? t.accent :
                                kind == kTapDouble   ? t.primary :
                                                       t.panelAccent;
                            paint.drawCircle(tapX, tapY, appCalcHeight(36),
                                             dotColor);
                            if (kind == kTapDouble) {
                                paint.drawRing(tapX, tapY, appCalcHeight(58),
                                               appCalcHeight(8), dotColor);
                            } else if (kind == kTapLong) {
                                paint.drawRing(tapX, tapY, appCalcHeight(72),
                                               appCalcHeight(10), dotColor);
                            }
                        }

                        // 缩放指示：圆心固定卡片中央，半径随累计 scale，
                        // 辐条随累计 rotation 旋转；双指中点另画一枚焦点。
                        const float cx = s.width * 0.5f;
                        const float cy = s.height * 0.55f;
                        const float maxR = std::min(s.width, s.height) * 0.42f;
                        const float r = std::max(appCalcHeight(20),
                            std::min(appCalcHeight(160) * scale, maxR));
                        paint.drawRing(cx, cy, r, appCalcHeight(10),
                                       t.panelGradient[0]);
                        paint.drawLine(cx, cy,
                                       cx + r * std::cos(rotation),
                                       cy + r * std::sin(rotation),
                                       appCalcHeight(8), t.textSecondary);
                        paint.drawCircle(cx, cy, appCalcHeight(14),
                                         t.textSecondary);
                        if (pointers > 0) {
                            paint.drawCircle(focalX, focalY, appCalcHeight(28),
                                             t.primary);
                        }
                    }),
                {
                    [this](const ClickEvent& e) {
                        setState([this, e] {
                            lastKind_ = kTapSingle;
                            tapX_ = e.x;
                            tapY_ = e.y;
                            std::snprintf(statusBuf_, sizeof(statusBuf_),
                                          "Tap（%.0f, %.0f）", e.x, e.y);
                            status_ = statusBuf_;
                        });
                    },
                    [this](const ClickEvent& e) {
                        setState([this, e] {
                            lastKind_ = kTapDouble;
                            tapX_ = e.x;
                            tapY_ = e.y;
                            std::snprintf(statusBuf_, sizeof(statusBuf_),
                                          "Double Tap（%.0f, %.0f）", e.x, e.y);
                            status_ = statusBuf_;
                        });
                    },
                    [this](const ClickEvent& e) {
                        setState([this, e] {
                            lastKind_ = kTapLong;
                            tapX_ = e.x;
                            tapY_ = e.y;
                            std::snprintf(statusBuf_, sizeof(statusBuf_),
                                          "Long Press（%.0f, %.0f）", e.x, e.y);
                            status_ = statusBuf_;
                        });
                    },
                    [this](const ScaleEvent& e) {
                        setState([this, e] {
                            if (e.state == ScaleState::Begin) {
                                baseScale_ = scale_;
                                baseRotation_ = rotation_;
                            } else {
                                scale_ = baseScale_ * e.scale;
                                rotation_ = baseRotation_ + e.rotation;
                            }
                            focalX_ = e.focalX;
                            focalY_ = e.focalY;
                            pointerCount_ = e.pointerCount;
                            const int degrees = static_cast<int>(
                                rotation_ * 180.0f / 3.14159265f);
                            std::snprintf(statusBuf_, sizeof(statusBuf_),
                                          "Scale ×%.2f · %d° · %d 指（%s）",
                                          scale_, degrees, e.pointerCount,
                                          e.state == ScaleState::Begin ? "begin" :
                                          e.state == ScaleState::Update ? "update" :
                                          e.state == ScaleState::End ? "end" :
                                          "cancel");
                            status_ = statusBuf_;
                        });
                    },
                }));

        auto cardWrap = padding(
            EdgeInsets::only(appCalcWidth(100), appCalcHeight(30),
                             appCalcWidth(100), 0),
            std::move(card));

        auto page = std::make_unique<Column>(widgetList(
            std::move(title), std::move(subtitle), std::move(status),
            std::move(cardWrap)));
        page->color = theme.windowBackground;
        return page;
    }

private:
    std::string status_ = "等待手势…";
    char statusBuf_[128] = {};
    TapKind lastKind_ = kTapNone;
    float tapX_ = 0.0f;
    float tapY_ = 0.0f;
    float scale_ = 1.0f;      ///< 累计缩放（跨会话保持）
    float rotation_ = 0.0f;   ///< 累计旋转（弧度，跨会话保持）
    float baseScale_ = 1.0f;  ///< 本会话 Begin 时的基准
    float baseRotation_ = 0.0f;
    float focalX_ = 0.0f;
    float focalY_ = 0.0f;
    int pointerCount_ = 0;
};

} // namespace

std::unique_ptr<evk::ui::State> GestureDemoPage::createState() const {
    return std::make_unique<GestureDemoPageState>();
}
