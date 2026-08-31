/**
 * @file text_layout.cpp
 * @brief 多行文本排版实现：逐码点宽度累加 + libunibreak（UAX #14）断行。
 */
#include "evk/ui/text_layout.h"

#include <utility>

#include "linebreak.h"

namespace evk::ui {

namespace {

/// 词间断行/修剪用的"空白"：只认 ASCII 空格（规则从简；可断点由 UAX #14 定）。
bool isSpace(uint32_t cp) { return cp == 0x20; }

} // namespace

void TextLayout::layout(const char* utf8, float sizePx, FontId preferred,
                        float maxWidth, float lineHeightScale, TextAlign align,
                        int maxLines) {
    const char* safe = utf8 ? utf8 : "";
    if (valid_ && text_ == safe && sizePx_ == sizePx && font_ == preferred &&
        maxWidth_ == maxWidth && lineHeightScale_ == lineHeightScale &&
        align_ == align && maxLines_ == maxLines) {
        return; // 输入未变：复用缓存，不重排。
    }
    text_ = safe;
    sizePx_ = sizePx;
    font_ = preferred;
    maxWidth_ = maxWidth;
    lineHeightScale_ = lineHeightScale;
    align_ = align;
    maxLines_ = maxLines;
    lines_.clear();
    valid_ = true;
    ++revision_;

    FontEngine& fonts = FontEngine::instance();
    // 自然行高不依赖文本内容：空串测量拿到的就是 ascent+descent。
    // 行距倍数在其上缩放，多出的空间上下均分（half-leading，CSS 风格）。
    float natural = 0.0f;
    fonts.measureText("", sizePx, preferred, nullptr, &natural);
    lineHeight_ = natural * lineHeightScale;
    leadingHalf_ = (lineHeight_ - natural) * 0.5f;
    // 省略号宽度只依赖字号/字体，排版时量好（测量不触发光栅化）。
    fonts.measureText("…", sizePx, preferred, &ellipsisWidth_, nullptr);
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

    // 可断点由 libunibreak 按 UAX #14 算出（lang=zh：中文标点避头尾等
    // 规则生效）。输出是逐字节标记：有效值写在一个码点的**最后一个字节**
    // 上（前面的字节一律 INSIDEACHAR），折算成逐字形的 canBreakAfter。
    // '\n' 的 MUSTBREAK 由下面扫描里的显式分支处理，这里只认 ALLOWBREAK。
    std::vector<char> brks(text_.size(), LINEBREAK_NOBREAK);
    set_linebreaks_utf8(reinterpret_cast<const utf8_t*>(text_.c_str()),
                        text_.size(), "zh", brks.data());
    std::vector<bool> canBreakAfter(n, false);
    for (size_t i = 0; i < n; ++i) {
        const size_t lastByte = glyphs[i].byteOffset + glyphs[i].byteLength - 1;
        canBreakAfter[i] = brks[lastByte] == LINEBREAK_ALLOWBREAK;
    }

    const bool softWrap = maxWidth > 0.0f;
    size_t lineStart = 0;               // 当前行首（字形下标）
    size_t lastBreak = n;               // 行内最近一个"字后可断"（n = 无）
    size_t i = 0;
    while (i < n) {
        const uint32_t cp = glyphs[i].codepoint;
        if (cp == '\n') { // 强制断行：'\n' 本身不进任何一行。
            pushLine(glyphs, prefix, lineStart, i);
            lineStart = i + 1;
            lastBreak = n;
            ++i;
            continue;
        }
        if (isSpace(cp) && i == lineStart) { // 行首空白跳过。
            lineStart = i + 1;
            ++i;
            continue;
        }
        if (softWrap && i > lineStart &&
            prefix[i + 1] - prefix[lineStart] > maxWidth) {
            // 超宽选断点（优先级从高到低）：
            // 1. 当前字形自身是可断空白：断在它之后。行尾空白成行时修剪、
            //    不占视觉宽度，所以"连空格一起超宽"时仍可收下前面的内容；
            //    前提是去掉它之后行宽合规（prefix[i] 即不含本字形的宽）。
            // 2. 回退到行内最近可断点（断在空格时空格同样修剪丢弃）。
            // 3. 行内无可断点（超长单词）则硬断。
            if (isSpace(cp) && canBreakAfter[i] &&
                prefix[i] - prefix[lineStart] <= maxWidth) {
                pushLine(glyphs, prefix, lineStart, i + 1);
                lineStart = i + 1;
                while (lineStart < n && isSpace(glyphs[lineStart].codepoint)) {
                    ++lineStart; // 断点后连续空白同样跳过。
                }
                lastBreak = n;
                ++i; // 本字形已归入上一行，直接前进。
                continue;
            }
            size_t breakAt = i;
            size_t lineEnd = i;
            if (lastBreak != n) { // lastBreak 恒在 [lineStart, i) 内
                lineEnd = lastBreak + 1;
                breakAt = lastBreak + 1;
            }
            pushLine(glyphs, prefix, lineStart, lineEnd);
            lineStart = breakAt;
            while (lineStart < n && isSpace(glyphs[lineStart].codepoint)) {
                ++lineStart; // 断点后连续空白同样跳过。
            }
            // 重扫新行已越过的部分，重建"最近可断点"候选。
            lastBreak = n;
            for (size_t j = lineStart; j < i; ++j) {
                if (canBreakAfter[j]) {
                    lastBreak = j;
                }
            }
            continue; // 不推进 i：让溢出字形在新行重新评估。
        }
        // 断点记录在溢出判断之后：当前字形若为可断点，已在上方分支
        // 单独裁决；记录到这里的是留给后续字形的候选。
        if (canBreakAfter[i]) {
            lastBreak = i;
        }
        ++i;
    }
    if (lineStart < n) {
        pushLine(glyphs, prefix, lineStart, n);
    } else if (n > 0 && glyphs[n - 1].codepoint == '\n') {
        // 文本以 '\n' 结尾：尾部还有一个空行。
        pushLine(glyphs, prefix, n, n);
    }
    applyMaxLines(glyphs, prefix);
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
        // half-leading：行距多出的空间上下均分，正文从行盒中线下移半份。
        const float textTop = top + leadingHalf_;
        const float left = x + alignOffset(line.width);
        if (line.begin != line.end) {
            const std::string slice =
                text_.substr(line.begin, line.end - line.begin);
            canvas.drawText(slice.c_str(), font_, left, textTop, sizePx_, clip,
                            color);
        }
        if (line.ellipsized) { // 截断行：省略号紧跟削尾后的正文。
            canvas.drawText("…", font_, left + line.textWidth, textTop, sizePx_,
                            clip, color);
        }
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
        const float textTop = top + leadingHalf_;
        const float left = x + alignOffset(line.width);
        if (line.begin != line.end) {
            const std::string slice =
                text_.substr(line.begin, line.end - line.begin);
            paint.drawText(slice.c_str(), font_, left, textTop, sizePx_, rgba);
        }
        if (line.ellipsized) {
            paint.drawText("…", font_, left + line.textWidth, textTop, sizePx_,
                           rgba);
        }
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
    line.textWidth = line.width;
    lines_.push_back(line);
}

void TextLayout::applyMaxLines(const std::vector<GlyphAdvance>& glyphs,
                               const std::vector<float>& prefix) {
    if (maxLines_ <= 0 || static_cast<int>(lines_.size()) <= maxLines_) {
        return;
    }
    lines_.resize(static_cast<size_t>(maxLines_));
    if (lines_.empty()) {
        return;
    }
    Line& last = lines_.back();
    last.ellipsized = true;
    if (maxWidth_ <= 0.0f) {
        // 不限宽时没有截断基准：只丢行，行尾直接补省略号。
        last.width += ellipsisWidth_;
        return;
    }
    // 行字节区间 → 字形下标区间（glyphs 按 byteOffset 升序）。
    const size_t n = glyphs.size();
    size_t gb = 0;
    while (gb < n && glyphs[gb].byteOffset < last.begin) {
        ++gb;
    }
    size_t ge = gb;
    while (ge < n && glyphs[ge].byteOffset < last.end) {
        ++ge;
    }
    // 从行尾向前削，直到"削后正文宽 + 省略号宽"收进 maxWidth。
    while (ge > gb && prefix[ge] - prefix[gb] + ellipsisWidth_ > maxWidth_) {
        --ge;
    }
    // 削尾落点若贴着空格，连同空格一起去掉，不留"abc …"式的悬空。
    while (ge > gb && isSpace(glyphs[ge - 1].codepoint)) {
        --ge;
    }
    last.end = ge < n ? glyphs[ge].byteOffset : text_.size();
    last.textWidth = prefix[ge] - prefix[gb];
    last.width = last.textWidth + ellipsisWidth_;
}

float TextLayout::alignOffset(float lineWidth) const {
    if (maxWidth_ <= 0.0f || align_ == TextAlign::kLeft) {
        return 0.0f;
    }
    const float slack = maxWidth_ - lineWidth;
    if (slack <= 0.0f) {
        return 0.0f; // 行比容器宽（硬断/省略号兜底场景）：不反向偏移。
    }
    return align_ == TextAlign::kCenter ? slack * 0.5f : slack;
}

} // namespace evk::ui
