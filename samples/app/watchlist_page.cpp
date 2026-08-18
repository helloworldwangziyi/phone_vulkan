/// @file watchlist_page.cpp
/// 蓝湖图纸「2-1-750-自选-黑」复刻实现。图纸坐标均为 375 逻辑宽下的像素，
/// ×2.88 换算到 sample 的 1080 设计基线（dp），再经 appCalc 映射真机像素。
#include "watchlist_page.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "app_fonts.h"
#include "screen_metrics.h"
#include "evk/kv_store.h"
#include "evk/ui/event_bus.h"
#include "evk/ui/font_engine.h"

namespace {

/// 图纸像素 → sample 设计像素（1080/375 = 2.88）。
float dp(float v) { return appCalcWidth(v * (1080.0f / 375.0f)); }

// ---- 图纸取色（0xRRGGBBAA）----
constexpr uint32_t kColorPageBg = 0x141914FF;   ///< 页面/行情行底色（图纸行矩形取色）
constexpr uint32_t kColorBarBg = 0x1C211CFF;    ///< 板块 Tab 条、底部 Tab 条底色
constexpr uint32_t kColorAccent = 0xD43B48FF;   ///< 选中红（Tab、M 徽章、下划线）
constexpr uint32_t kColorUp = 0xFF0000FF;       ///< 涨（红）
constexpr uint32_t kColorDown = 0x00B578FF;     ///< 跌（绿；图纸未给出，按行业惯例补）
constexpr uint32_t kColorName = 0xFFB60DFF;     ///< 合约名黄
constexpr uint32_t kColorGray = 0xAFAFAFFF;     ///< 次级文字灰
constexpr uint32_t kColorWhite = 0xFFFFFFFF;    ///< 成交白
constexpr uint32_t kColorDivider = 0xFFFFFF1A;  ///< 行间 0.5px 分隔线
constexpr uint32_t kColorToastBg = 0xF7EAC8FF;  ///< Toast 米色气泡
constexpr uint32_t kColorToastText = 0x232227FF;

/// 数字列右边缘（图纸 x 右缘）：最新 / 涨跌 / 涨跌幅 / 成交 / 持仓 / 日增仓。
/// 后两列超出 375 图纸宽——列表横向加宽后左右滑动查看（双轴列表演示）。
constexpr float kColumnRight[] = {158.0f, 228.0f, 298.0f, 368.0f, 446.0f, 520.0f};

/// 列表内容宽（图纸 px）：最后一列右缘 + 12 边距。大于视口才横向可滚。
constexpr float kContentWidth = 532.0f;

/// 底部 Tab 选中项的持久化 key（KeyValueStore），跨启动保持。
constexpr const char* kKeyBottomNav = "watchlist.bottomNav";
constexpr int kBottomNavCount = 4;

/// 行情行数据：数值字段供 tick 更新，文本字段是重建时排版好的展示串。
struct QuoteRow {
    std::string name;
    std::string code;
    double preClose = 0.0;
    double price = 0.0;
    double change = 0.0;
    double pct = 0.0;
    double volume = 0.0;
    double position = 0.0;       ///< 持仓量
    double positionDelta = 0.0;  ///< 日增仓（可负）
    bool main = true;  ///< 是否带「M」主力徽章
    // ---- 展示串（syncTexts 刷新）----
    std::string priceText;
    std::string changeText;
    std::string pctText;
    std::string volumeText;
    std::string positionText;
    std::string deltaText;
    bool up = true;
};

std::string format0(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.0f", v);
    return buf;
}

std::string format2(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", v);
    return buf;
}

/// 带符号整数（日增仓：+123 / -45）。
std::string formatSigned(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%+.0f", v);
    return buf;
}

void syncTexts(QuoteRow& row) {
    row.change = row.price - row.preClose;
    row.pct = row.preClose > 0.0 ? row.change / row.preClose * 100.0 : 0.0;
    row.up = row.change >= 0.0;
    row.priceText = format0(row.price);
    row.changeText = format2(row.change);
    row.pctText = format2(row.pct) + "%";
    row.volumeText = format0(row.volume);
    row.positionText = format0(row.position);
    row.deltaText = formatSigned(row.positionDelta);
}

/// 文本绘制小助手：measureText 量出行盒后按对齐方式落 drawText。
/// y 对齐统一用「给定垂直中心」——行盒高 = ascent + descent，随字号变化。
void drawTextCenteredY(evk::ui::PaintContext& paint, const char* s, float xLeft,
                       float centerY, float sizePx, uint32_t color,
                       evk::ui::FontId font) {
    float w = 0.0f, h = 0.0f;
    evk::ui::FontEngine::instance().measureText(s, sizePx, font, &w, &h);
    paint.drawText(s, font, xLeft, centerY - h * 0.5f, sizePx, color);
}

void drawTextRight(evk::ui::PaintContext& paint, const char* s, float xRight,
                   float centerY, float sizePx, uint32_t color,
                   evk::ui::FontId font) {
    float w = 0.0f, h = 0.0f;
    evk::ui::FontEngine::instance().measureText(s, sizePx, font, &w, &h);
    paint.drawText(s, font, xRight - w, centerY - h * 0.5f, sizePx, color);
}

void drawTextCentered(evk::ui::PaintContext& paint, const char* s, float centerX,
                      float centerY, float sizePx, uint32_t color,
                      evk::ui::FontId font) {
    float w = 0.0f, h = 0.0f;
    evk::ui::FontEngine::instance().measureText(s, sizePx, font, &w, &h);
    paint.drawText(s, font, centerX - w * 0.5f, centerY - h * 0.5f, sizePx, color);
}

class WatchlistPageState final : public evk::ui::State {
public:
    WatchlistPageState() {
        // 初始自选股：名称/代码/昨结/最新/成交量/持仓量/日增仓/主力。
        // 涨跌由昨结算出。
        struct Seed {
            const char* name;
            const char* code;
            double preClose;
            double price;
            double volume;
            double position;
            double positionDelta;
            bool main;
        };
        const Seed seeds[] = {
            {"花生201", "SV201", 8520.0, 8524.0, 36523.0, 98654.0, 1230.0, true},
            {"豆粕405", "M405", 3048.0, 3052.0, 281130.0, 1250340.0, 18520.0, true},
            {"棕榈209", "P209", 7860.0, 7842.0, 152340.0, 452310.0, -8320.0, true},
            {"豆油301", "Y301", 7978.0, 7996.0, 98450.0, 385420.0, 5210.0, false},
            {"螺纹410", "RB410", 3538.0, 3542.0, 402110.0, 1820450.0, 24310.0, true},
            {"铁矿501", "I501", 780.0, 776.0, 210340.0, 965230.0, -12680.0, true},
            {"白糖309", "SR309", 5688.0, 5698.0, 88720.0, 352140.0, 4320.0, false},
            {"棉花501", "CF501", 13420.0, 13455.0, 45210.0, 218760.0, 2140.0, true},
            {"橡胶307", "RU307", 14105.0, 14130.0, 62180.0, 198650.0, -1560.0, false},
            {"甲醇505", "MA505", 2480.0, 2476.0, 150020.0, 685240.0, 9840.0, true},
            {"纯碱601", "SA601", 1522.0, 1531.0, 198760.0, 756320.0, 15230.0, true},
            {"玻璃505", "FG505", 1080.0, 1086.0, 234560.0, 524180.0, -6210.0, false},
        };
        rows_.reserve(sizeof(seeds) / sizeof(seeds[0]));
        for (const Seed& seed : seeds) {
            QuoteRow row;
            row.name = seed.name;
            row.code = seed.code;
            row.preClose = seed.preClose;
            row.price = seed.price;
            row.volume = seed.volume;
            row.position = seed.position;
            row.positionDelta = seed.positionDelta;
            row.main = seed.main;
            syncTexts(row);
            rows_.push_back(std::move(row));
        }
        // 读回上次的底部 Tab 选中项（如「交易」），未存过/越界回落到 0（自选）。
        // 此时 KeyValueStore 已由平台壳在 EngineReady 前初始化完毕。
        nav_ = std::clamp(evk::KeyValueStore::instance().getInt(kKeyBottomNav, 0),
                          0, kBottomNavCount - 1);
    }

    ~WatchlistPageState() override { cancelThreads(); }

    void didMount() override {
        // 行情推送模拟：每秒一次随机游走价格，postUi 回 UI 线程 setState。
        // 正是「列表随行情通知变化」的场景——文字全部命中字形缓存，每帧
        // 只是重建顶点，不会有 atlas 重传尖峰。
        tickCancel_ = std::make_shared<std::atomic_bool>(false);
        const auto cancel = tickCancel_;
        std::thread([this, cancel] {
            while (!cancel->load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                if (cancel->load()) {
                    return;
                }
                evk::ui::postUi([this, cancel] {
                    if (cancel->load() || !mounted()) {
                        return;
                    }
                    setState([this] { tickPrices(); });
                });
            }
        }).detach();
    }

    bool onWillLeave(bool) override {
        cancelThreads();
        return true;
    }

    void dispose() override { cancelThreads(); }

    std::unique_ptr<evk::ui::Widget> build(evk::ui::BuildContext&) override {
        using namespace evk::ui;
        auto page = std::make_unique<Column>(widgetList(
            buildTabStrip(),
            expanded(buildList(), 1.0f),
            buildToastSlot(),
            buildBottomBar()));
        page->color = kColorPageBg;
        return page;
    }

private:
    // ---- 板块 Tab 条（默认 | 板块1..5，高 40.5，选中红色 + 下划线）----
    std::unique_ptr<evk::ui::Widget> buildTabStrip() {
        using namespace evk::ui;
        static const char* kTabs[] = {"默认", "板块1", "板块2",
                                      "板块3", "板块4", "板块5"};
        std::vector<std::unique_ptr<Widget>> tabs;
        for (int i = 0; i < 6; ++i) {
            const char* label = kTabs[i];
            const bool selected = tab_ == i;
            tabs.push_back(sizedBox(
                dp(62.5f), dp(40.5f),
                container(
                    0,
                    [this, i] { setState([this, i] { tab_ = i; }); },
                    [label, selected](PaintContext& paint) {
                        const Size size = paint.size();
                        drawTextCentered(paint, label, size.width * 0.5f,
                                         size.height * 0.5f, dp(14.0f),
                                         selected ? kColorAccent : kColorGray,
                                         appFonts::cjk());
                        if (selected) {
                            // 选中下划线：图纸是 14.5x3 的红色小圆角条。
                            const float uw = dp(14.5f);
                            const float uh = std::max(1.5f, dp(3.0f));
                            paint.drawRoundRect(
                                {(size.width - uw) * 0.5f,
                                 size.height - dp(4.5f) - uh, uw, uh},
                                uh * 0.5f, kColorAccent);
                        }
                    })));
        }
        auto strip = std::make_unique<Row>(std::move(tabs));
        strip->color = kColorBarBg;
        return sizedBox(-1.0f, dp(40.5f), std::move(strip));
    }

    // ---- 表头行（名称 | 最新↓ | 涨跌 | 涨跌幅 | 成交 | 持仓 | 日增仓）----
    // 作为列表第 0 行，随行内容一起横向滚动（行高由 ListView 的 itemExtent 给）。
    std::unique_ptr<evk::ui::Widget> buildHeader() {
        using namespace evk::ui;
        return container(0, {}, [](PaintContext& paint) {
            const Size size = paint.size();
            const float centerY = size.height * 0.5f;
            const float fs = dp(14.0f);
            drawTextCenteredY(paint, "名称", dp(12.5f), centerY, fs,
                              kColorGray, appFonts::cjk());
            // 「最新」带降序箭头：文字右缘让出箭头位，实心小三角朝下。
            const float arrowHalf = dp(3.75f);
            drawTextRight(paint, "最新", dp(kColumnRight[0]) - dp(11.0f),
                          centerY, fs, kColorGray, appFonts::cjk());
            const float ax = dp(kColumnRight[0]) - arrowHalf * 2.0f;
            paint.drawTriangle(ax, centerY - arrowHalf * 0.7f,
                               ax + arrowHalf * 2.0f, centerY - arrowHalf * 0.7f,
                               ax + arrowHalf, centerY + arrowHalf * 0.9f,
                               kColorGray, kColorGray, kColorGray);
            drawTextRight(paint, "涨跌", dp(kColumnRight[1]), centerY, fs,
                          kColorGray, appFonts::cjk());
            drawTextRight(paint, "涨跌幅", dp(kColumnRight[2]), centerY, fs,
                          kColorGray, appFonts::cjk());
            drawTextRight(paint, "成交", dp(kColumnRight[3]), centerY, fs,
                          kColorGray, appFonts::cjk());
            drawTextRight(paint, "持仓", dp(kColumnRight[4]), centerY, fs,
                          kColorGray, appFonts::cjk());
            drawTextRight(paint, "日增仓", dp(kColumnRight[5]), centerY, fs,
                          kColorGray, appFonts::cjk());
        });
    }

    // ---- 行情行：左 名称/代码+M，右六列数字右对齐，底部分隔线 ----
    // 行矩形由 ListView 按 itemExtent 直接写入（无需 sizedBox 包装）。
    std::unique_ptr<evk::ui::Widget> buildRow(const QuoteRow& row, int index) {
        using namespace evk::ui;
        return container(
            0,
            [this, index] { removeRow(index); },
            [row](PaintContext& paint) {
                const Size size = paint.size();
                const float centerY = size.height * 0.5f;
                const uint32_t numColor = row.up ? kColorUp : kColorDown;
                const float numFs = dp(15.0f);

                // 左列：合约名（黄 15）在上，代码（灰 11）在下。
                drawTextCenteredY(paint, row.name.c_str(), dp(12.5f),
                                  dp(15.0f), dp(15.0f), kColorName,
                                  kFontAny);
                float codeW = 0.0f, codeH = 0.0f;
                FontEngine::instance().measureText(
                    row.code.c_str(), dp(11.0f), appFonts::latin(), &codeW,
                    &codeH);
                paint.drawText(row.code.c_str(), appFonts::latin(),
                               dp(12.5f), dp(31.0f) - codeH * 0.5f,
                               dp(11.0f), kColorGray);
                if (row.main) {
                    // 主力徽章：红色圆角描边小方块 + 居中 M。
                    const Rect badge{dp(12.5f) + codeW + dp(4.0f),
                                     dp(31.0f) - dp(6.0f), dp(12.0f),
                                     dp(12.0f)};
                    paint.strokeRoundRect(badge, dp(2.0f),
                                          std::max(1.0f, dp(0.8f)),
                                          kColorAccent);
                    drawTextCentered(paint, "M", badge.x + badge.w * 0.5f,
                                     badge.y + badge.h * 0.5f, dp(9.0f),
                                     kColorAccent, appFonts::latin());
                }

                // 右六列：最新 / 涨跌 / 涨跌幅（红涨绿跌）、成交与持仓（白）、
                // 日增仓（正红负绿）。后两列宽出视口，左右滑动查看。
                drawTextRight(paint, row.priceText.c_str(),
                              dp(kColumnRight[0]), centerY, numFs, numColor,
                              appFonts::latin());
                drawTextRight(paint, row.changeText.c_str(),
                              dp(kColumnRight[1]), centerY, numFs, numColor,
                              appFonts::latin());
                drawTextRight(paint, row.pctText.c_str(),
                              dp(kColumnRight[2]), centerY, numFs, numColor,
                              appFonts::latin());
                drawTextRight(paint, row.volumeText.c_str(),
                              dp(kColumnRight[3]), centerY, numFs,
                              kColorWhite, appFonts::latin());
                drawTextRight(paint, row.positionText.c_str(),
                              dp(kColumnRight[4]), centerY, numFs,
                              kColorWhite, appFonts::latin());
                const uint32_t deltaColor =
                    row.positionDelta >= 0.0 ? kColorUp : kColorDown;
                drawTextRight(paint, row.deltaText.c_str(),
                              dp(kColumnRight[5]), centerY, numFs,
                              deltaColor, appFonts::latin());

                // 底部分隔线：图纸 x=8、高 0.5，随内容宽贯穿整行。
                const float lh = std::max(1.0f, dp(0.5f));
                paint.drawRect({dp(8.0f), size.height - lh,
                                size.width - dp(16.0f), lh},
                               kColorDivider);
            });
    }

    std::unique_ptr<evk::ui::Widget> buildList() {
        using namespace evk::ui;
        // 表头作为第 0 行进入列表：列加宽后表头随行一起横向滚动。
        // v1 不做固定表头（跨滚动区域的手势与联动 v1 不支持）。
        std::vector<std::unique_ptr<Widget>> rows;
        rows.reserve(rows_.size() + 1);
        rows.push_back(buildHeader());
        for (size_t i = 0; i < rows_.size(); ++i) {
            rows.push_back(buildRow(rows_[i], static_cast<int>(i)));
        }
        return listView(dp(50.0f), std::move(rows), dp(kContentWidth));
    }

    // ---- Toast 槽（固定高 50）：有内容时画米色圆角气泡，不占位跳动 ----
    std::unique_ptr<evk::ui::Widget> buildToastSlot() {
        using namespace evk::ui;
        const std::string toast = toast_;
        return sizedBox(
            -1.0f, dp(50.0f),
            container(0, {}, [toast](PaintContext& paint) {
                if (toast.empty()) {
                    return;
                }
                const Size size = paint.size();
                const float bw = dp(159.5f);
                const float bh = dp(35.0f);
                paint.drawRoundRect({(size.width - bw) * 0.5f,
                                     (size.height - bh) * 0.5f, bw, bh},
                                    dp(6.0f), kColorToastBg);
                drawTextCentered(paint, toast.c_str(), size.width * 0.5f,
                                 size.height * 0.5f, dp(14.0f), kColorToastText,
                                 appFonts::cjk());
            }));
    }

    // ---- 底部 Tab 条（高 74：自选/行情/交易/资讯，图标 + 9.5 标签）----
    std::unique_ptr<evk::ui::Widget> buildBottomBar() {
        using namespace evk::ui;
        static const char* kLabels[] = {"自选", "行情", "交易", "资讯"};
        std::vector<std::unique_ptr<Widget>> tabs;
        for (int i = 0; i < kBottomNavCount; ++i) {
            const char* label = kLabels[i];
            const bool selected = nav_ == i;
            tabs.push_back(expanded(container(
                0,
                // 点按即落盘：下次冷启动 State 构造时读回（对照
                // Flutter shared_preferences 的设置项保持）。
                [this, i] {
                    evk::KeyValueStore::instance().putInt(kKeyBottomNav, i);
                    setState([this, i] { nav_ = i; });
                },
                [i, label, selected](PaintContext& paint) {
                    const Size size = paint.size();
                    const uint32_t color =
                        selected ? kColorAccent : kColorGray;
                    // 条顶分隔线（四个相邻 tab 各画一段，拼成整条）。
                    paint.drawRect({0.0f, 0.0f, size.width,
                                    std::max(1.0f, dp(0.5f))},
                                   kColorDivider);
                    const float cx = size.width * 0.5f;
                    const float top = dp(10.0f);
                    const float t = std::max(1.0f, dp(2.0f)); ///< 图标线宽
                    switch (i) {
                        case 0: { ///< 自选：人头 + 肩弧
                            paint.drawCircle(cx, top + dp(4.5f), dp(4.0f),
                                             color);
                            paint.drawArc(cx, top + dp(16.5f), dp(7.5f), t,
                                          3.14159265f, 3.14159265f, color);
                            break;
                        }
                        case 1: { ///< 行情：折线上攻 + 箭头
                            const float x0 = cx - dp(8.0f);
                            const float y0 = top + dp(15.0f);
                            paint.drawLine(x0, y0, cx - dp(3.0f),
                                           top + dp(9.0f), t, color);
                            paint.drawLine(cx - dp(3.0f), top + dp(9.0f),
                                           cx + dp(1.0f), top + dp(12.5f), t,
                                           color);
                            paint.drawLine(cx + dp(1.0f), top + dp(12.5f),
                                           cx + dp(7.0f), top + dp(4.0f), t,
                                           color);
                            const float arrow[] = {cx + dp(9.5f), top + dp(2.0f),
                                                   cx + dp(2.0f), top + dp(2.5f),
                                                   cx + dp(8.0f), top + dp(9.5f)};
                            paint.drawConvexPolygon(arrow, 3, color);
                            break;
                        }
                        case 2: { ///< 交易：双向箭头 ⇄
                            paint.drawLine(cx - dp(8.0f), top + dp(5.5f),
                                           cx + dp(6.0f), top + dp(5.5f), t,
                                           color);
                            const float right[] = {cx + dp(5.0f), top + dp(1.5f),
                                                   cx + dp(5.0f), top + dp(9.5f),
                                                   cx + dp(10.0f), top + dp(5.5f)};
                            paint.drawConvexPolygon(right, 3, color);
                            paint.drawLine(cx - dp(6.0f), top + dp(15.5f),
                                           cx + dp(8.0f), top + dp(15.5f), t,
                                           color);
                            const float left[] = {cx - dp(5.0f), top + dp(11.5f),
                                                  cx - dp(5.0f), top + dp(19.5f),
                                                  cx - dp(10.0f), top + dp(15.5f)};
                            paint.drawConvexPolygon(left, 3, color);
                            break;
                        }
                        default: { ///< 资讯：文档轮廓 + 两行
                            paint.strokeRoundRect(
                                {cx - dp(7.0f), top + dp(1.5f), dp(14.0f),
                                 dp(18.0f)},
                                dp(2.0f), t, color);
                            paint.drawLine(cx - dp(4.0f), top + dp(7.0f),
                                           cx + dp(4.0f), top + dp(7.0f), t,
                                           color);
                            paint.drawLine(cx - dp(4.0f), top + dp(12.5f),
                                           cx + dp(1.5f), top + dp(12.5f), t,
                                           color);
                            break;
                        }
                    }
                    drawTextCentered(paint, label, cx, dp(44.0f), dp(9.5f),
                                     color, appFonts::cjk());
                })));
        }
        auto bar = std::make_unique<Row>(std::move(tabs));
        bar->color = kColorBarBg;
        return sizedBox(-1.0f, dp(74.0f), std::move(bar));
    }

    /// 点按行情行 = 移出自选（对照图纸 Toast：「花生201已从自选删除」）。
    void removeRow(int index) {
        if (index < 0 || index >= static_cast<int>(rows_.size())) {
            return;
        }
        std::string name = rows_[static_cast<size_t>(index)].name;
        setState([this, index, name] {
            rows_.erase(rows_.begin() + index);
            toast_ = name + "已从自选删除";
        });
        // 1.6s 后自动消失；代数戳防连续点按时旧定时器误清新 Toast。
        const uint32_t gen = ++toastGen_;
        const auto cancel = tickCancel_;
        std::thread([this, gen, cancel] {
            std::this_thread::sleep_for(std::chrono::milliseconds(1600));
            evk::ui::postUi([this, gen, cancel] {
                if ((cancel && cancel->load()) || !mounted() ||
                    gen != toastGen_) {
                    return;
                }
                setState([this] { toast_.clear(); });
            });
        }).detach();
    }

    /// 每秒行情：价格随机游走 ±0.2%，成交量微增，展示串全部重算。
    void tickPrices() {
        for (QuoteRow& row : rows_) {
            const float r = nextRandom();
            const double delta = row.price * 0.002 * (static_cast<double>(r) - 0.5) * 2.0;
            row.price = std::max(1.0, row.price + delta);
            row.volume += nextRandom() * 80.0;
            syncTexts(row);
        }
    }

    /// xorshift32：行情模拟够用的轻量随机源（UI 线程独占，无锁）。
    float nextRandom() {
        rngState_ ^= rngState_ << 13;
        rngState_ ^= rngState_ >> 17;
        rngState_ ^= rngState_ << 5;
        return static_cast<float>(rngState_ & 0xFFFFFFu) /
               static_cast<float>(0x1000000u);
    }

    void cancelThreads() {
        if (tickCancel_) {
            tickCancel_->store(true);
        }
    }

    std::vector<QuoteRow> rows_;
    int tab_ = 0;       ///< 板块 Tab 选中项
    int nav_ = 0;       ///< 底部 Tab 选中项（KeyValueStore 持久化，跨启动保持）
    std::string toast_; ///< 空 = 隐藏
    uint32_t toastGen_ = 0;
    uint32_t rngState_ = 0x2F6E2B1u;
    std::shared_ptr<std::atomic_bool> tickCancel_;
};

} // namespace

std::unique_ptr<evk::ui::State> WatchlistPage::createState() const {
    return std::make_unique<WatchlistPageState>();
}
