/// @file app_fonts.cpp
#include "app_fonts.h"

#include "evk/assets/font_assets.h"
#include "evk/log.h"

namespace {

/// 注册结果缓存；kInvalidFont 表示尚未注册。
evk::ui::FontId gLatin = evk::ui::kInvalidFont;
evk::ui::FontId gCjk = evk::ui::kInvalidFont;
evk::ui::FontId gCjkBold = evk::ui::kInvalidFont;

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

} // namespace appFonts
