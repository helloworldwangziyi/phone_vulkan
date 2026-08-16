#pragma once

/**
 * @file app_fonts.h
 * @brief App 字体表：把内嵌字体资产注册进 FontEngine，并给页面发语义化句柄。
 *
 * 注册顺序即回退顺序（FontEngine 按注册序逐字体找字）：
 * Latin 在前（英文/数字用它，字形精细）、CJK 在后补全汉字——
 * 所以 kFontAny 排版混排文本时会自动分派。
 */
#include "evk/ui/font_engine.h"

namespace appFonts {

/// Roboto Regular：英文/数字正文字体。
evk::ui::FontId latin();
/// Noto Sans SC Regular：中文字体（混排时自动补全 Roboto 缺的汉字）。
evk::ui::FontId cjk();
/// Noto Sans SC Bold：中文标题字体。
evk::ui::FontId cjkBold();

/// 注册全部内嵌字体；App 在 EngineReady 后调用一次（重复调用幂等跳过）。
void registerFonts();

} // namespace appFonts
