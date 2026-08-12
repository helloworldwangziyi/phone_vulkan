// 详情页：演示导航返回（导航栏返回按钮 / 左缘滑动返回）与多级 push。
// 可同时存在多个实例：每个实例的句柄记录存入 g_pages，
// pop 销毁前由 Navigation 的 on_pop 回调通知清理（detailPageOnPopped），
// 整树重建（切换主题）时由 detailPagesClear 一次性清空。
// 层序号 variant 让每层主视觉/色卡颜色不同，连续 push 时跳转有感知。

#include "detail_page.h"

#include <vector>

#include "app_metrics.h"
#include "app_theme.h"
#include "evk/esx_view.h"
#include "evk/log.h"
#include "evk/ui/controls/button.h"
#include "evk/ui/controls/navigation.h"

namespace {

struct DetailPage {
    esx_view page = 0;
    esx_view nav = 0;
    esx_view hero = 0;
    esx_view cards[4] = {};
    esx_view pushButton = 0;
};

std::vector<DetailPage> g_pages;

void drawHero(esx_view view, void* userData) {
    const AppTheme& theme = appTheme();
    // userData 是创建时传入的层序号（值，不是指针），让每层详情页主视觉不同。
    const int variant = static_cast<int>(reinterpret_cast<intptr_t>(userData));
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

void onPushAgainClick(esx_view button, void* /*userData*/) {
    for (const DetailPage& record : g_pages) {
        if (record.pushButton == button) {
            EVK_LOGI("detail page pushes another detail page");
            esx_navigation_push(record.nav, detailPageCreate(record.nav), 1);
            return;
        }
    }
}

void layoutDetailPage(DetailPage& record) {
    const float contentWidth = appCalcWidth(880.0f);
    const float contentX = (g_screenWidth - contentWidth) * 0.5f;
    esx_view_set_bounds(record.hero, contentX, appCalcHeight(60.0f),
                        contentWidth, appCalcHeight(400.0f));

    const float cardWidth = appCalcWidth(420.0f);
    const float cardHeight = appCalcHeight(170.0f);
    for (int i = 0; i < 4; ++i) {
        const float x = contentX + static_cast<float>(i % 2) * appCalcWidth(460.0f);
        const float y = appCalcHeight(520.0f + static_cast<float>(i / 2) * 210.0f);
        esx_view_set_bounds(record.cards[i], x, y, cardWidth, cardHeight);
    }

    const float buttonWidth = appCalcWidth(400.0f);
    esx_view_set_bounds(record.pushButton, (g_screenWidth - buttonWidth) * 0.5f,
                        appCalcHeight(1010.0f), buttonWidth, appCalcHeight(140.0f));
}

} // namespace

esx_view detailPageCreate(esx_view nav) {
    const AppTheme& theme = appTheme();
    // 层序号：当前栈里的详情页数量，用来让每层颜色不同，push 跳转有感知。
    const int variant = static_cast<int>(g_pages.size());

    DetailPage record;
    record.nav = nav;
    record.page = esx_create_view(0, 0, 0, 0, 0);
    esx_view_set_background(record.page, theme.windowBackground);

    record.hero = esx_create_view(0, 0, 0, 0, record.page);
    esx_view_set_background(record.hero, theme.surface);
    esx_view_set_draw_callback(record.hero, drawHero,
                               reinterpret_cast<void*>(static_cast<intptr_t>(variant)));

    for (int i = 0; i < 4; ++i) {
        record.cards[i] = esx_create_view(0, 0, 0, 0, record.page);
        esx_view_set_background(record.cards[i], theme.scrollItems[(variant * 3 + i) % 8]);
    }

    const esx_button_style buttonStyle{theme.primary, theme.primaryPressed,
                                       theme.primaryDisabled};
    record.pushButton = esx_button_create(0, 0, 0, 0, record.page, &buttonStyle,
                                          onPushAgainClick, nullptr);

    g_pages.push_back(record);
    layoutDetailPage(g_pages.back());
    EVK_LOGI("detail page created: page={} variant={} count={}",
             record.page, variant, g_pages.size());
    return record.page;
}

void detailPagesLayout() {
    for (DetailPage& record : g_pages) {
        layoutDetailPage(record);
    }
}

void detailPageOnPopped(esx_view page) {
    for (auto it = g_pages.begin(); it != g_pages.end(); ++it) {
        if (it->page == page) {
            g_pages.erase(it);
            return;
        }
    }
}

void detailPagesClear() {
    g_pages.clear();
}
