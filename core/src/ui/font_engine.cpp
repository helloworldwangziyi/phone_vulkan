/**
 * @file font_engine.cpp
 * @brief 字体引擎实现：stb_truetype 解析、回退选字、字形光栅化与货架法 atlas。
 */
#include "evk/ui/font_engine.h"

#include <algorithm>
#include <cstdio>
#include <vector>

#include "evk/log.h"
#include "evk/ui/texture_store.h"

// stb_truetype 以头文件库方式引入：恰在一个翻译单元里定义 IMPLEMENTATION 宏。
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

namespace evk::ui {

namespace {

/// atlas 页边长（方形，RGBA）。1024 在移动端是安全尺寸：单页 4MB，
/// 按常用字号一个中文字形约 50×50，一页能放约 300 个。
constexpr int kAtlasPageSize = 1024;
/// 页数上限：渲染器 descriptor 池按它预分配；超出后不再缓存新字形
/// （仅告警一次），既有内容不受影响。
constexpr int kMaxAtlasPages = 16;
/// 字形四周的空白边（像素）：配合 UV 半像素内缩，阻止线性采样读到相邻字形。
constexpr int kGlyphPadding = 1;

/**
 * @brief 逐字节解码 UTF-8。非法序列按 U+FFFD 处理并继续（绘制不能因坏输入崩掉）。
 * @param s 输入串
 * @param i 进出的字节下标，返回时指向下一个字符
 * @return 解码出的码点
 */
uint32_t decodeUtf8(const char* s, size_t& i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) {
        ++i;
        return c;
    }
    // 多字节首字节给出后续字节数与初始位；每段 6 个有效位。
    int extra = 0;
    uint32_t cp = 0;
    if ((c & 0xE0) == 0xC0) { extra = 1; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { extra = 2; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { extra = 3; cp = c & 0x07; }
    else { ++i; return 0xFFFD; }

    for (int k = 0; k < extra; ++k) {
        const unsigned char cc = static_cast<unsigned char>(s[i + 1 + k]);
        if ((cc & 0xC0) != 0x80) {
            i += 1 + k;
            return 0xFFFD;
        }
        cp = (cp << 6) | (cc & 0x3F);
    }
    i += 1 + extra;
    return cp;
}

} // namespace

FontEngine& FontEngine::instance() {
    static FontEngine engine;
    return engine;
}

FontId FontEngine::addFont(const void* ttfData, size_t size) {
    if (!ttfData || size == 0) {
        return kInvalidFont;
    }
    FontRecord record;
    record.data.resize(size);
    std::copy(static_cast<const unsigned char*>(ttfData),
              static_cast<const unsigned char*>(ttfData) + size, record.data.begin());
    auto* info = new stbtt_fontinfo();
    const int offset = stbtt_GetFontOffsetForIndex(record.data.data(), 0);
    if (offset < 0 || !stbtt_InitFont(info, record.data.data(), offset)) {
        EVK_LOGW("addFont: not a parsable TrueType font ({} bytes)", size);
        delete info;
        return kInvalidFont;
    }
    record.info = info;
    fonts_.push_back(std::move(record));
    return static_cast<FontId>(fonts_.size() - 1);
}

int FontEngine::fontCount() const {
    return static_cast<int>(fonts_.size());
}

void FontEngine::reset() {
    for (auto& font : fonts_) {
        delete static_cast<stbtt_fontinfo*>(font.info);
    }
    fonts_.clear();
    pages_.clear();
    glyphCache_.clear();
}

uint64_t FontEngine::makeKey(int fontIndex, int glyphIndex, int pxSize) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(fontIndex)) << 48) |
           (static_cast<uint64_t>(static_cast<uint32_t>(glyphIndex)) << 16) |
           static_cast<uint64_t>(static_cast<uint16_t>(pxSize));
}

int FontEngine::resolveFont(uint32_t codepoint, FontId preferred) const {
    if (fonts_.empty()) {
        return -1;
    }
    // 首选字体优先；缺字沿注册顺序回退。
    if (preferred >= 0 && preferred < static_cast<int>(fonts_.size())) {
        const auto* info = static_cast<const stbtt_fontinfo*>(fonts_[preferred].info);
        if (stbtt_FindGlyphIndex(info, codepoint) != 0) {
            return preferred;
        }
    }
    for (size_t i = 0; i < fonts_.size(); ++i) {
        const auto* info = static_cast<const stbtt_fontinfo*>(fonts_[i].info);
        if (stbtt_FindGlyphIndex(info, codepoint) != 0) {
            return static_cast<int>(i);
        }
    }
    // 全都缺字：用首个字体的 .notdef（豆腐块），保证排版宽度仍然可预期。
    return 0;
}

void FontEngine::lineMetrics(float sizePx, FontId preferred, float* ascent,
                             float* descent) const {
    const int index = (preferred >= 0 && preferred < static_cast<int>(fonts_.size()))
                          ? preferred : 0;
    if (fonts_.empty()) {
        *ascent = 0.0f;
        *descent = 0.0f;
        return;
    }
    const auto* info = static_cast<const stbtt_fontinfo*>(fonts_[index].info);
    const float scale = stbtt_ScaleForMappingEmToPixels(info, sizePx);
    int a = 0, d = 0, lineGap = 0;
    stbtt_GetFontVMetrics(info, &a, &d, &lineGap);
    *ascent = a * scale;
    // stbtt 的 descent 是负数（基线以下），行盒高按 ascent - descent 计。
    *descent = -d * scale;
}

std::vector<FontEngine::GlyphRunItem> FontEngine::layoutRun(const char* utf8,
                                                            float sizePx,
                                                            FontId preferred) const {
    std::vector<GlyphRunItem> run;
    if (!utf8 || fonts_.empty()) {
        return run;
    }
    int prevFont = -1;
    int prevGlyph = -1;
    size_t i = 0;
    while (utf8[i] != '\0') {
        const uint32_t cp = decodeUtf8(utf8, i);
        const int fontIndex = resolveFont(cp, preferred);
        if (fontIndex < 0) {
            break;
        }
        const auto* info = static_cast<const stbtt_fontinfo*>(fonts_[fontIndex].info);
        const int gid = stbtt_FindGlyphIndex(info, cp);
        const float scale = stbtt_ScaleForMappingEmToPixels(info, sizePx);

        GlyphRunItem item;
        item.fontIndex = fontIndex;
        item.glyphIndex = gid;
        item.kern = 0.0f;
        int advance = 0, leftBearing = 0;
        stbtt_GetGlyphHMetrics(info, gid, &advance, &leftBearing);
        item.advance = advance * scale;
        // 字距调整只在同一字体内部有效（跨字体的 kerning 无定义）。
        if (prevFont == fontIndex && prevGlyph >= 0) {
            item.kern = stbtt_GetGlyphKernAdvance(info, prevGlyph, gid) * scale;
        }
        prevFont = fontIndex;
        prevGlyph = gid;
        run.push_back(item);
    }
    return run;
}

const FontEngine::CachedGlyph* FontEngine::rasterizeGlyph(int fontIndex, int glyphIndex,
                                                          float sizePx) {
    // 字号量化到整数像素：缓存键稳定，同字号的重复文本命中缓存。
    const int pxSize = std::max(1, static_cast<int>(sizePx + 0.5f));
    const uint64_t key = makeKey(fontIndex, glyphIndex, pxSize);
    const auto hit = glyphCache_.find(key);
    if (hit != glyphCache_.end()) {
        return &hit->second;
    }

    const auto* info = static_cast<const stbtt_fontinfo*>(fonts_[fontIndex].info);
    const float scale = stbtt_ScaleForMappingEmToPixels(info, static_cast<float>(pxSize));
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    stbtt_GetGlyphBitmapBox(info, glyphIndex, scale, scale, &x0, &y0, &x1, &y1);
    const int w = x1 - x0;
    const int h = y1 - y0;
    int advance = 0, leftBearing = 0;
    stbtt_GetGlyphHMetrics(info, glyphIndex, &advance, &leftBearing);

    CachedGlyph cached;
    cached.texture = kInvalidTexture;
    cached.x = cached.y = 0;
    cached.w = std::max(0, w);
    cached.h = std::max(0, h);
    cached.xoff = static_cast<float>(x0);
    cached.yoff = static_cast<float>(y0);
    cached.advance = advance * scale;

    if (w > 0 && h > 0) {
        // 货架法打包：当前行放得下就接着放，放不下换行，行也放不下开新页。
        const int needW = w + kGlyphPadding * 2;
        const int needH = h + kGlyphPadding * 2;
        int pageIndex = -1;
        for (int p = static_cast<int>(pages_.size()) - 1; p >= 0; --p) {
            AtlasPage& page = pages_[p];
            const int nextX = page.cursorX + needW;
            const int nextY = (page.rowHeight >= needH)
                                  ? page.cursorY
                                  : page.cursorY + page.rowHeight;
            if (nextX <= kAtlasPageSize &&
                nextY + std::max(page.rowHeight, needH) <= kAtlasPageSize) {
                pageIndex = p;
                break;
            }
        }
        if (pageIndex < 0) {
            if (static_cast<int>(pages_.size()) >= kMaxAtlasPages) {
                static bool warned = false;
                if (!warned) {
                    EVK_LOGW("font atlas full ({} pages); new glyphs are dropped",
                             kMaxAtlasPages);
                    warned = true;
                }
                return nullptr;
            }
            AtlasPage page;
            page.texture = TextureStore::instance().addTexture(
                static_cast<uint32_t>(kAtlasPageSize),
                static_cast<uint32_t>(kAtlasPageSize), nullptr);
            pageIndex = static_cast<int>(pages_.size());
            pages_.push_back(std::move(page));
        }
        AtlasPage& page = pages_[pageIndex];
        if (page.rowHeight < needH) {
            // 换到新一行：光标下移到更高字形的顶部。
            page.cursorY += page.rowHeight;
            page.cursorX = 0;
            page.rowHeight = needH;
        }
        const int dstX = page.cursorX;
        const int dstY = page.cursorY;

        // stbtt 光栅出单通道覆盖率，展开成 RGBA（rgb 恒白、a=覆盖率）：
        // shader 的"顶点色 × 纹理"公式据此把文字颜色乘上覆盖度。
        std::vector<unsigned char> coverage(static_cast<size_t>(w) * h);
        stbtt_MakeGlyphBitmap(info, coverage.data(), w, h, w, scale, scale, glyphIndex);
        uint32_t* dst = TextureStore::instance().mutablePixels(page.texture) +
                        static_cast<size_t>(dstY + kGlyphPadding) * kAtlasPageSize +
                        dstX + kGlyphPadding;
        for (int row = 0; row < h; ++row) {
            for (int col = 0; col < w; ++col) {
                dst[col] = 0xFFFFFF00u | coverage[static_cast<size_t>(row) * w + col];
            }
            dst += kAtlasPageSize;
        }
        TextureStore::instance().markDirty(page.texture);
        page.cursorX += needW;

        cached.texture = page.texture;
        cached.x = dstX;
        cached.y = dstY;
    }
    // 空白字形（空格等）也缓存：宽高为 0 但推进宽度有效。
    return &glyphCache_.emplace(key, cached).first->second;
}

float FontEngine::measureText(const char* utf8, float sizePx, FontId preferred,
                              float* outWidth, float* outHeight) const {
    float width = 0.0f;
    for (const GlyphRunItem& item : layoutRun(utf8, sizePx, preferred)) {
        width += item.advance + item.kern;
    }
    float ascent = 0.0f;
    float descent = 0.0f;
    lineMetrics(sizePx, preferred, &ascent, &descent);
    if (outWidth) {
        *outWidth = width;
    }
    if (outHeight) {
        *outHeight = ascent + descent;
    }
    return width;
}

void FontEngine::forEachGlyph(const char* utf8, float sizePx, FontId preferred,
                              const std::function<void(const PlacedGlyph&)>& fn) {
    if (!fn || fonts_.empty()) {
        return;
    }
    float ascent = 0.0f;
    float descent = 0.0f;
    lineMetrics(sizePx, preferred, &ascent, &descent);
    float pen = 0.0f;
    for (const GlyphRunItem& item : layoutRun(utf8, sizePx, preferred)) {
        pen += item.kern;
        const CachedGlyph* glyph = rasterizeGlyph(item.fontIndex, item.glyphIndex, sizePx);
        if (glyph && glyph->w > 0 && glyph->h > 0) {
            PlacedGlyph placed;
            placed.texture = glyph->texture;
            const float size = static_cast<float>(kAtlasPageSize);
            // UV 指向"去掉留边的字形本体"，再向内缩半个纹素：
            // 线性采样在最外圈也不会读到隔壁字形或留边的零。
            placed.u0 = (glyph->x + kGlyphPadding + 0.5f) / size;
            placed.v0 = (glyph->y + kGlyphPadding + 0.5f) / size;
            placed.u1 = (glyph->x + kGlyphPadding + glyph->w - 0.5f) / size;
            placed.v1 = (glyph->y + kGlyphPadding + glyph->h - 0.5f) / size;
            placed.x = pen + glyph->xoff;
            // yoff 以基线为原点（向上为负），行盒左上角在基线上方 ascent 处。
            placed.y = ascent + glyph->yoff;
            placed.w = static_cast<float>(glyph->w);
            placed.h = static_cast<float>(glyph->h);
            fn(placed);
        }
        pen += item.advance;
    }
}

int FontEngine::pageCount() const {
    return static_cast<int>(pages_.size());
}

TextureId FontEngine::pageTexture(int page) const {
    if (page < 0 || page >= static_cast<int>(pages_.size())) {
        return kInvalidTexture;
    }
    return pages_[page].texture;
}

int FontEngine::pageSize() const {
    return kAtlasPageSize;
}

} // namespace evk::ui
