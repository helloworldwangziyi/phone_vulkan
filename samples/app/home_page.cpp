#include "home_page.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "screen_metrics.h"
#include "app_fonts.h"
#include "app_images.h"
#include "app_theme.h"
#include "detail_page.h"
#include "watchlist_page.h"
#include "evk/log.h"
#include "evk/ui/font_engine.h"
#include "evk/ui/navigation/navigation_stack.h"

namespace {

evk::ui::NavigationStyle navigationStyle(const AppTheme& theme) {
    return {
        theme.navBar,
        theme.navBarLine,
        theme.surfaceRaised,
        theme.surface,
        theme.backArrow,
    };
}

class HomePageState final : public evk::ui::State {
public:
    ~HomePageState() override {
        cancelRequest();
    }

    void didMount() override {
        listen(
            kEventThemeChanged,
            evk::ui::EventPriority::Normal,
            [this](const void*) { setState(); });
    }

    std::unique_ptr<evk::ui::Widget> build(
        evk::ui::BuildContext&) override {
        using namespace evk::ui;
        const AppTheme& theme = appTheme();

        auto panel = padding(
            EdgeInsets::only(
                appCalcWidth(100.0f),
                appCalcHeight(60.0f),
                appCalcWidth(100.0f),
                0.0f),
            sizedBox(
                -1.0f,
                appCalcHeight(540.0f),
                container(
                    theme.surface,
                    [this] {
                        setState([this] { panelAccent_ = !panelAccent_; });
                    },
                    [accent = panelAccent_](PaintContext& paint) {
                        const AppTheme& currentTheme = appTheme();
                        const Size size = paint.size();
                        const float centerX = size.width * 0.5f;
                        const float halfBase = size.width * 0.4f;
                        paint.drawTriangle(
                            centerX, size.height * 0.12f,
                            centerX + halfBase, size.height * 0.88f,
                            centerX - halfBase, size.height * 0.88f,
                            accent ? currentTheme.panelAccent
                                   : currentTheme.panelGradient[0],
                            currentTheme.panelGradient[1],
                            currentTheme.panelGradient[2]);
                    })));

        auto detailButton = padding(
            EdgeInsets::only(0.0f, appCalcHeight(60.0f), 0.0f, 0.0f),
            center(sizedBox(
                appCalcWidth(400.0f),
                appCalcHeight(140.0f),
                button(
                    {theme.primary, theme.primaryPressed, theme.primaryDisabled},
                    [this] {
                        context().navigator().push(
                            makeWidget<DetailPage>(detailCount_++), true);
                    }))));

        auto themeButton = padding(
            EdgeInsets::only(0.0f, appCalcHeight(60.0f), 0.0f, 0.0f),
            center(sizedBox(
                appCalcWidth(400.0f),
                appCalcHeight(140.0f),
                button(
                    {theme.secondary, theme.secondaryPressed, theme.primaryDisabled},
                    [this] {
                        appThemeToggle();
                        context().navigator().setStyle(navigationStyle(appTheme()));
                        EventBus::instance().emit(kEventThemeChanged);
                    }))));

        /// 蓝湖「自选」图纸复刻页入口（带文字标签，区别于上面两个色块按钮）。
        auto watchlistEntry = padding(
            EdgeInsets::only(0.0f, appCalcHeight(60.0f), 0.0f, 0.0f),
            center(sizedBox(
                appCalcWidth(400.0f),
                appCalcHeight(140.0f),
                container(
                    theme.accent,
                    [this] {
                        context().navigator().push(
                            makeWidget<WatchlistPage>(), true);
                    },
                    [](PaintContext& paint) {
                        const Size size = paint.size();
                        const char* label = "自选行情（蓝湖复刻）";
                        const float fontSize = appCalcHeight(36.0f);
                        float textWidth = 0.0f;
                        float textHeight = 0.0f;
                        evk::ui::FontEngine::instance().measureText(
                            label, fontSize, appFonts::cjk(), &textWidth,
                            &textHeight);
                        paint.drawText(label, appFonts::cjk(),
                                       (size.width - textWidth) * 0.5f,
                                       (size.height - textHeight) * 0.5f,
                                       fontSize, 0xFFFFFFFF);
                    }))));

        std::vector<std::unique_ptr<Widget>> rows;
        float contentHeight = 0.0f;
        if (!quoteLoaded_) {
            rows.push_back(sizedBox(
                -1.0f,
                appCalcHeight(400.0f),
                container(theme.surfaceRaised)));
            contentHeight = appCalcHeight(400.0f);
        } else {
            rows.reserve(quotes_.size());
            for (uint32_t color : quotes_) {
                rows.push_back(padding(
                    EdgeInsets::only(0.0f, 0.0f, 0.0f, appCalcHeight(30.0f)),
                    sizedBox(
                        -1.0f,
                        appCalcHeight(122.0f),
                        container(color))));
            }
            contentHeight = appCalcHeight(152.0f * quotes_.size());
        }

        /// ---- 2D 图元演示条：一个 painter 画完一行 ----
        /// 实心圆 / 圆环 / 圆弧 / 线段 / 渐变 / 圆角描边 / 位图徽章，
        /// 覆盖 Canvas 新增的全部图元类目（凹多边形/子区域贴图见测试）。
        auto shapeStrip = sizedBox(
            -1.0f, appCalcHeight(200.0f),
            container(0, {}, [](PaintContext& paint) {
                const AppTheme& theme = appTheme();
                const Size size = paint.size();
                const float h = size.height;
                const float step = size.width / 7.0f;
                paint.drawCircle(step * 0.5f, h * 0.5f, h * 0.32f, theme.accent);
                paint.drawRing(step * 1.5f, h * 0.5f, h * 0.3f, h * 0.07f,
                               theme.panelGradient[0]);
                paint.drawArc(step * 2.5f, h * 0.5f, h * 0.3f, h * 0.07f,
                              -2.2f, 3.6f, theme.panelAccent);
                paint.drawLine(step * 3.05f, h * 0.25f, step * 3.95f, h * 0.75f,
                               h * 0.06f, theme.textSecondary);
                paint.drawRectGradient({step * 4.1f, h * 0.2f, step * 0.8f, h * 0.6f},
                                       theme.panelGradient[0], theme.panelGradient[2],
                                       true);
                paint.strokeRoundRect({step * 5.1f, h * 0.2f, step * 0.8f, h * 0.6f},
                                      h * 0.12f, h * 0.045f, theme.accent);
                if (appImages::badge() != evk::ui::kInvalidTexture) {
                    paint.drawImage(appImages::badge(),
                                    {step * 6.05f, h * 0.2f, h * 0.6f, h * 0.6f});
                }
            }));
        auto shapeRow = padding(
            EdgeInsets::only(appCalcWidth(100.0f), appCalcHeight(20.0f),
                             appCalcWidth(100.0f), 0.0f),
            std::move(shapeStrip));

        /// ---- 文字标题区：中文粗体标题 + 中英混排副标题 ----
        /// 副标题用 latin 字体排版：汉字在 Roboto 里缺失，FontEngine 自动
        /// 沿注册顺序回退到 NotoSansSC——一行内两种字体无感混排。
        auto titleBlock = column(widgetList(
            padding(
                EdgeInsets::only(appCalcWidth(100.0f), appCalcHeight(40.0f),
                                  appCalcWidth(100.0f), 0.0f),
                text("行情速览", appCalcHeight(56.0f), theme.textPrimary,
                     appFonts::cjkBold())),
            padding(
                EdgeInsets::only(appCalcWidth(100.0f), appCalcHeight(8.0f),
                                 appCalcWidth(100.0f), 0.0f),
                text("Market Overview · 沪深300 实时数据",
                     appCalcHeight(28.0f), theme.textSecondary))));

        auto list = expanded(
            padding(
                EdgeInsets::only(
                    appCalcWidth(100.0f),
                    appCalcHeight(40.0f),
                    appCalcWidth(100.0f),
                    appCalcHeight(24.0f)),
                scrollView(
                    column(std::move(rows)),
                    contentHeight,
                    [](float x, float y) {
                        EVK_LOGI("quote list offset=({:.1f}, {:.1f})", x, y);
                    })),
            1.0f);

        auto page = std::make_unique<Column>(widgetList(
            std::move(panel),
            std::move(detailButton),
            std::move(themeButton),
            std::move(watchlistEntry),
            std::move(shapeRow),
            std::move(titleBlock),
            std::move(list)));
        page->color = theme.windowBackground;
        return page;
    }

    void onDidEnter(bool) override {
        if (quoteLoaded_ || quotePending_) {
            return;
        }
        quotePending_ = true;
        quoteCancel_ = std::make_shared<std::atomic_bool>(false);
        const auto cancel = quoteCancel_;
        std::thread([this, cancel] {
            std::this_thread::sleep_for(std::chrono::milliseconds(600));
            evk::ui::postUi([this, cancel] {
                if (cancel->load() || !mounted()) {
                    return;
                }
                const AppTheme& theme = appTheme();
                quotes_.clear();
                for (int i = 0; i < 20; ++i) {
                    quotes_.push_back(theme.scrollItems[i % 8]);
                }
                quoteLoaded_ = true;
                quotePending_ = false;
                setState();
            });
        }).detach();
    }

    bool onWillLeave(bool) override {
        cancelRequest();
        quotePending_ = false;
        return true;
    }

    void dispose() override {
        cancelRequest();
    }

private:
    void cancelRequest() {
        if (quoteCancel_) {
            quoteCancel_->store(true);
        }
    }

    bool panelAccent_ = false;
    int detailCount_ = 0;
    bool quoteLoaded_ = false;
    bool quotePending_ = false;
    std::vector<uint32_t> quotes_;
    std::shared_ptr<std::atomic_bool> quoteCancel_;
};

} // namespace

std::unique_ptr<evk::ui::State> HomePage::createState() const {
    return std::make_unique<HomePageState>();
}
