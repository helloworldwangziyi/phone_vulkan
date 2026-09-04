/// @file app_fonts.cpp
#include "app_fonts.h"

#include "screen_metrics.h"
#include "evk/assets/font_assets.h"
#include "evk/log.h"

namespace {

/// 注册结果缓存；kInvalidFont 表示尚未注册。
evk::ui::FontId gLatin = evk::ui::kInvalidFont;
evk::ui::FontId gCjk = evk::ui::kInvalidFont;
evk::ui::FontId gCjkBold = evk::ui::kInvalidFont;

/// 行情页图纸像素 → sample 设计像素（与 watchlist_page.cpp 的 dp 一致）。
float watchDp(float v) { return appCalcWidth(v * (1080.0f / 375.0f)); }

void prewarmText(const char* utf8, float sizePx, evk::ui::FontId font) {
    evk::ui::FontEngine::instance().prewarm(utf8, sizePx, font);
}

} // namespace

namespace appFonts {

evk::ui::FontId latin() {
    return gLatin;
}

evk::ui::FontId cjk() {
    return gCjk;
}

evk::ui::FontId cjkBold() {
    return gCjkBold;
}

void registerFonts() {
    auto& engine = evk::ui::FontEngine::instance();
    if (engine.fontCount() > 0) {
        return; ///< 已注册（surface 重建时 EngineReady 可能再来一次）
    }
    gLatin = engine.addFont(evk::assets::kFontRobotoRegular,
                            sizeof(evk::assets::kFontRobotoRegular));
    gCjk = engine.addFont(evk::assets::kFontNotoSansScRegular,
                          sizeof(evk::assets::kFontNotoSansScRegular));
    gCjkBold = engine.addFont(evk::assets::kFontNotoSansScBold,
                              sizeof(evk::assets::kFontNotoSansScBold));
    EVK_LOGI("fonts registered: latin={} cjk={} cjkBold={}",
             static_cast<int>(gLatin), static_cast<int>(gCjk),
             static_cast<int>(gCjkBold));
}

void prewarm() {
    if (g_screenWidth <= 0.0f) {
        // SurfaceChanged 还没来：appCalc 换算结果是 0，预热无意义。
        EVK_LOGI("font prewarm skipped: screen size not ready");
        return;
    }
    using evk::ui::kFontAny;

    // ---- 首页/详情页/路径演示页（字号走 appCalcHeight）----
    // 标题类（粗体）：首页「行情速览」、路径演示页「矢量路径演示」。
    prewarmText("行情速览矢量路径演示", appCalcHeight(56.0f), cjkBold());
    // 副标题（text() 默认 kFontAny：latin 优先、汉字回退 CJK）。
    prewarmText("Market Overview · 沪深300 实时数据", appCalcHeight(28.0f),
                kFontAny);
    prewarmText("Path · 贝塞尔曲线 / 凹多边形三角化", appCalcHeight(28.0f),
                kFontAny);
    // 首页两个入口按钮（cjk 排版：括号内的 latin 也解析进 CJK 字体）。
    prewarmText("自选行情（蓝湖复刻）矢量路径演示（Path）",
                appCalcHeight(36.0f), cjk());
    // 详情页 caption：「Layer #N · 渲染层详情」，N 是运行期数字，digits 全量预热。
    prewarmText("Layer #0123456789 · 渲染层详情", appCalcHeight(30.0f), cjk());
    // 路径演示页：画布标签 + 三段说明长文（含换行截断的省略号）。
    prewarmText("cubicTo quadTo 填充凹多边形三角化描边…",
                appCalcHeight(26.0f), cjk());
    prewarmText("心形由 4 段三次贝塞尔组成；五角星是凹多边形，由 ear clipping "
                "算法三角化；曲线按曲率自适应细分，平直段少分、弯曲段多分 "
                "是的发生的发烧地方撒旦法水电费阿斯蒂芬是的发生的发烧地方。",
                appCalcHeight(26.0f), cjk());
    prewarmText("居中对齐 TextAlign::kCenter：每一行独立按容器宽度居中，"
                "短的一行会明显收在中间。",
                appCalcHeight(26.0f), cjk());
    prewarmText("右对齐 TextAlign::kRight 且 maxLines = 2：这一段说明文字"
                "故意写得非常非常长，长到无论多宽的屏幕都肯定要折成三行"
                "以上——超过两行的部分会被整体丢弃，第二行的末尾按剩余宽"
                "度削掉几个字、自动补上省略号，列表摘要、卡片简介这类"
                "场景就是这么用的，再也不用担心文本把布局撑爆。",
                appCalcHeight(26.0f), cjk());

    // ---- 行情页（字号走 watchDp，与 watchlist_page.cpp 的 dp 一致）----
    // 板块 Tab + 表头。
    prewarmText("默认板块12345名称最新涨跌幅成交持仓日增仓",
                watchDp(14.0f), cjk());
    // 合约名（kFontAny：汉字走 CJK、数字走 Roboto）。
    prewarmText("花生201豆粕405棕榈209豆油301螺纹410铁矿501"
                "白糖309棉花501橡胶307甲醇505纯碱601玻璃505",
                watchDp(15.0f), kFontAny);
    // 合约代码 + 主力徽章 M。
    prewarmText("SV201M405P209Y301RB410I501SR309CF501RU307MA505SA601FG505",
                watchDp(11.0f), latin());
    prewarmText("M", watchDp(9.0f), latin());
    // 六列数字（最新/涨跌/涨跌幅/成交/持仓/日增仓：数字与 .+%- 符号）。
    prewarmText("0123456789.+%-", watchDp(15.0f), latin());
    // Toast「xx已从自选删除」用 cjk 排版：数字解析进 CJK 字体（缓存键不同）。
    prewarmText("花生201豆粕405棕榈209豆油301螺纹410铁矿501"
                "白糖309棉花501橡胶307甲醇505纯碱601玻璃505已从自选删除",
                watchDp(14.0f), cjk());
    // 底部 Tab 标签。
    prewarmText("自选行情交易资讯", watchDp(9.5f), cjk());

    EVK_LOGI("font prewarm done: atlas pages={}",
             evk::ui::FontEngine::instance().pageCount());
}

} // namespace appFonts
