/**
 * @file text_layout.cpp
 * @brief 多行文本排版实现：逐码点宽度累加 + 从简断行规则。
 */
#include "evk/ui/text_layout.h"

#include <utility>

namespace evk::ui {

namespace {

/// 换行规则覆盖的 CJK 区块（简繁同在这些区间；含全角标点与表意扩展 B）。
bool isCjk(uint32_t cp) {
    return (cp >= 0x2E80 && cp <= 0x9FFF) || (cp >= 0x3000 && cp <= 0x303F) ||
           (cp >= 0xFF00 && cp <= 0xFFEF) || (cp >= 0x20000 && cp <= 0x2A6DF);
}

/// 词内不断、词间断行里的"词间空白"：只认 ASCII 空格（规则从简）。
bool isSpace(uint32_t cp) { return cp == 0x20; }

} // namespace

void TextLayout::layout(const char* utf8, float sizePx, FontId preferred,
                        float maxWidth) {
    const char* safe = utf8 ? utf8 : "";
    if (valid_ && text_ == safe && sizePx_ == sizePx && font_ == preferred &&
        maxWidth_ == maxWidth) {
        return; // 输入未变：复用缓存，不重排。
    }
    text_ = safe;
    sizePx_ = sizePx;
    font_ = preferred;
    maxWidth_ = maxWidth;
    lines_.clear();
    valid_ = true;
    ++revision_;

    FontEngine& fonts = FontEngine::instance();
    // 行高不依赖文本内容：空串测量拿到的就是 ascent+descent。
    fonts.measureText("", sizePx, preferred, nullptr, &lineHeight_);
    if (text_.empty()) {
        return;
    }

    const std::vector<GlyphAdvance> glyphs =
        fonts.measureGlyphs(text_.c_str(), sizePx, preferred);
    const size_t n = glyphs.size();
    // 推进宽度前缀和：任意字形区间宽度 O(1) 可得（回退断点重算行宽用）。
    std::vector<float> prefix(n + 1, 0.0f);
    for (size_t i = 0; i < n; ++i) {
        prefix[i + 1] = prefix[i] + glyphs[i].advance;
    }

    const bool softWrap = maxWidth > 0.0f;
    size_t lineStart = 0;               // 当前行首（字形下标）
    size_t lastSpace = n;               // 行内最近一个空格（n = 无）
    size_t i = 0;
    while (i < n) {
        const uint32_t cp = glyphs[i].codepoint;
        if (cp == '\n') { // 强制断行：'\n' 本身不进任何一行。
            pushLine(glyphs, prefix, lineStart, i);
            lineStart = i + 1;
            lastSpace = n;
            ++i;
            continue;
        }
        if (isSpace(cp) && i == lineStart) { // 行首空白跳过。
            lineStart = i + 1;
            ++i;
            continue;
        }
        if (isSpace(cp)) {
            lastSpace = i;
        }
        if (softWrap && i > lineStart &&
            prefix[i + 1] - prefix[lineStart] > maxWidth) {
            // 超宽选断点：CJK 前后任意断 > 回退到行内最近空格 > 硬断。
            // 断在空格时空格丢弃（成行时统一修剪行尾空白）。
            size_t breakAt = i;
            size_t lineEnd = i;
            const bool cjkBreak = isCjk(cp) || isCjk(glyphs[i - 1].codepoint);
            if (!cjkBreak && lastSpace != n && lastSpace > lineStart) {
                lineEnd = lastSpace;
                breakAt = lastSpace + 1;
            }
            pushLine(glyphs, prefix, lineStart, lineEnd);
            lineStart = breakAt;
            while (lineStart < n && isSpace(glyphs[lineStart].codepoint)) {
                ++lineStart; // 断点后连续空白同样跳过。
            }
            // 重扫新行已越过的部分，重建"最近空格"候选。
            lastSpace = n;
            for (size_t j = lineStart; j < i; ++j) {
                if (isSpace(glyphs[j].codepoint)) {
                    lastSpace = j;
                }
            }
            continue; // 不推进 i：让溢出字形在新行重新评估。
        }
        ++i;
    }
    if (lineStart < n) {
        pushLine(glyphs, prefix, lineStart, n);
    } else if (n > 0 && glyphs[n - 1].codepoint == '\n') {
        // 文本以 '\n' 结尾：尾部还有一个空行。
        pushLine(glyphs, prefix, n, n);
    }
}

std::string TextLayout::lineText(int index) const {
    if (index < 0 || index >= static_cast<int>(lines_.size())) {
        return {};
    }
    const Line& line = lines_[static_cast<size_t>(index)];
    return text_.substr(line.begin, line.end - line.begin);
}

void TextLayout::paint(Canvas& canvas, float x, float y, Color color,
                       const Rect& clip) const {
    for (size_t i = 0; i < lines_.size(); ++i) {
        const float top = y + static_cast<float>(i) * lineHeight_;
        // 行 y 区间与 clip 不相交：跳过（不生成任何顶点）。
        if (top + lineHeight_ <= clip.y || top >= clip.y + clip.h) {
            continue;
        }
        const Line& line = lines_[i];
        if (line.begin == line.end) {
            continue; // 空行无字形可画。
        }
        const std::string slice =
            text_.substr(line.begin, line.end - line.begin);
        canvas.drawText(slice.c_str(), font_, x, top, sizePx_, clip, color);
    }
}

void TextLayout::paint(PaintContext& paint, float x, float y,
                       uint32_t rgba) const {
    const Rect clip = paint.clip(); // 视图局部坐标。
    for (size_t i = 0; i < lines_.size(); ++i) {
        const float top = y + static_cast<float>(i) * lineHeight_;
        if (top + lineHeight_ <= clip.y || top >= clip.y + clip.h) {
            continue;
        }
        const Line& line = lines_[i];
        if (line.begin == line.end) {
            continue;
        }
        const std::string slice =
            text_.substr(line.begin, line.end - line.begin);
        paint.drawText(slice.c_str(), font_, x, top, sizePx_, rgba);
    }
}

void TextLayout::pushLine(const std::vector<GlyphAdvance>& glyphs,
                          const std::vector<float>& prefix, size_t begin,
                          size_t end) {
    // 行尾空白修剪：断行处丢弃的空格不留在任何一行上。
    while (end > begin && isSpace(glyphs[end - 1].codepoint)) {
        --end;
    }
    Line line;
    line.begin = begin < glyphs.size() ? glyphs[begin].byteOffset : text_.size();
    line.end = end < glyphs.size() ? glyphs[end].byteOffset : text_.size();
    line.width = prefix[end] - prefix[begin];
    lines_.push_back(line);
}

} // namespace evk::ui
