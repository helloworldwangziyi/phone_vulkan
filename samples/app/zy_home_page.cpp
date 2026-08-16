#include "zy_home_page.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "zy_screen_metrics.h"
#include "zy_app_theme.h"
#include "zy_detail_page.h"
#include "evk/zy_log.h"
#include "evk/ui/navigation/zy_navigation_stack.h"

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
