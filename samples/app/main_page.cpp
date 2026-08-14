// 首页实现：build() 即页面结构（嵌套即父子），回调内联。
// 对照 estarx App 页面 create 函数的可读性，但消灭 layout/destroy 样板：
// 布局由 Column/Flex 容器自动完成，resize 由容器级联重排。

#include "main_page.h"

#include <chrono>
#include <thread>

#include "app_metrics.h"
#include "app_theme.h"
#include "detail_page.h"
#include "evk/log.h"
#include "evk/ui/controls/navigation.h"

namespace {

// 面板渐变三角（局部坐标，同旧 drawPanel）。
void drawPanelGradient(esx_view view, bool accent) {
    const AppTheme& theme = appTheme();
    const uint32_t left = accent ? theme.panelAccent : theme.panelGradient[0];
    esx_draw_triangle(view, appCalcWidth(440.0f), appCalcHeight(50.0f),
                      appCalcWidth(790.0f), appCalcHeight(460.0f),
                      appCalcWidth(90.0f), appCalcHeight(460.0f),
                      left, theme.panelGradient[1], theme.panelGradient[2]);
}

} // namespace

HomePage::HomePage() {
    // 主题切换广播 → 重建换肤（build 重读 token，diff 就地更新）。
    listen(kEventThemeChanged, ESX_PRI_NORMAL, [this](const void*) { setState(); });
}

HomePage::~HomePage() {
    if (quoteCancel_) {
        quoteCancel_->store(true);
    }
}

std::unique_ptr<evk::ui::Widget> HomePage::build() {
    using namespace evk::ui;
    const AppTheme& theme = appTheme();

    // 顶部渐变面板：点击切换强调色。
    auto panel = Box(theme.surface);
    panel.onTap = [this] {
        setState([this] { panelAccent_ = !panelAccent_; });
    };
    panel.onDraw = [this](esx_view view) { drawPanelGradient(view, panelAccent_); };
    panel.layout.main = appCalcHeight(540.0f);
    panel.layout.marginMainBefore = appCalcHeight(60.0f);
    panel.layout.marginCross = appCalcWidth(100.0f);

    // 主按钮：push 详情页（层序号递增，详情页颜色随层变化）。
    const esx_button_style primaryStyle{theme.primary, theme.primaryPressed,
                                        theme.primaryDisabled};
    auto detailButton = ButtonW(primaryStyle);
    detailButton.onTap = [this] {
        pushPage(nav(), std::make_unique<DetailPage>(detailCount_++), true);
    };
    detailButton.layout.main = appCalcHeight(140.0f);
    detailButton.layout.cross = appCalcWidth(400.0f);
    detailButton.layout.align = ESX_FLEX_ALIGN_CENTER;
    detailButton.layout.marginMainBefore = appCalcHeight(60.0f);

    // 次按钮：切换主题并广播（导航栏样式同步更换）。
    const esx_button_style secondaryStyle{theme.secondary, theme.secondaryPressed,
                                          theme.primaryDisabled};
    auto themeButton = ButtonW(secondaryStyle);
    themeButton.onTap = [this] {
        appThemeToggle();
        const AppTheme& next = appTheme();
        const esx_navigation_style navStyle{next.navBar, next.navBarLine,
                                            next.surfaceRaised, next.surface,
                                            next.backArrow};
        esx_navigation_set_style(nav(), &navStyle);
        esx_event_emit(kEventThemeChanged, nullptr);
    };
    themeButton.layout = detailButton.layout;

    // 模拟行情列表：数据到达前占位行，到达后按数据重建（diff 自动增删行）。
    std::vector<std::unique_ptr<Widget>> rows;
    float contentHeight = 0.0f;
    if (!quoteLoaded_) {
        rows.push_back(makeWidget(Box(theme.surfaceRaised)
                                      .mainSize(appCalcHeight(400.0f))));
        contentHeight = appCalcHeight(400.0f);
    } else {
        rows = mapWidgets(quotes_, [](uint32_t color) {
            return Box(color)
                .mainSize(appCalcHeight(122.0f))
                .marginMain(0.0f, appCalcHeight(30.0f));
        });
        contentHeight = appCalcHeight(24.0f + 152.0f * quotes_.size());
    }
    auto list = ScrollW(makeWidget(column(std::move(rows))), contentHeight);
    list.onScroll = [](float x, float y) {
        EVK_LOGI("quote list offset=({:.1f}, {:.1f})", x, y);
    };
    list.layout.weight = 1;
    list.layout.marginMainBefore = appCalcHeight(40.0f);
    list.layout.marginMainAfter = appCalcHeight(24.0f);
    list.layout.marginCross = appCalcWidth(100.0f);

    auto page = column(std::move(panel), std::move(detailButton),
                       std::move(themeButton), std::move(list));
    page.color = theme.windowBackground;
    return makeWidget(std::move(page));
}

void HomePage::onDidEnter(bool /*forward*/) {
    if (quoteLoaded_ || quotePending_) {
        return;
    }
    // 模拟网络请求：后台线程 600ms 后经 esx_post_ui 回 UI 线程"数据返回"。
    quotePending_ = true;
    quoteCancel_ = std::make_shared<std::atomic_bool>(false);
    const auto cancel = quoteCancel_;
    std::thread([this, cancel] {
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        evk::ui::postUi([this, cancel] {
            if (cancel->load()) {
                return; // 已离开/销毁：丢弃迟到数据
            }
            const AppTheme& theme = appTheme();
            quotes_.clear();
            for (int i = 0; i < 20; ++i) {
                quotes_.push_back(theme.scrollItems[i % 8]);
            }
            quoteLoaded_ = true;
            quotePending_ = false;
            EVK_LOGI("quotes arrived, render {} rows", quotes_.size());
            setState(); // 数据返回 → 重建列表
        });
    }).detach();
}

bool HomePage::onWillLeave(bool /*forward*/) {
    // 离开（被覆盖或将 pop）：取消进行中的请求，迟到数据由取消标志拦截。
    if (quoteCancel_) {
        quoteCancel_->store(true);
    }
    quotePending_ = false;
    return true;
}
