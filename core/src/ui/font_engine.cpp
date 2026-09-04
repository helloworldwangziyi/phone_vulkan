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
    // 函数内静态局部变量：首次调用时构造，线程安全由编译器保证（C++11）。
    static FontEngine engine;
    return engine;
}

FontId FontEngine::addFont(const void* ttfData, size_t size) {
    if (!ttfData || size == 0) {
        return kInvalidFont;
    }
    FontRecord record;
    // 拷贝一份自持：调用方的缓冲（如资产读取的临时内存）随后即可释放。
    record.data.resize(size);
    std::copy(static_cast<const unsigned char*>(ttfData),
              static_cast<const unsigned char*>(ttfData) + size, record.data.begin());
    auto* info = new stbtt_fontinfo();
    // TTC 合集取第 0 个字体；普通 TTF 此调用返回 0。
    const int offset = stbtt_GetFontOffsetForIndex(record.data.data(), 0);
    if (offset < 0 || !stbtt_InitFont(info, record.data.data(), offset)) {
        EVK_LOGW("addFont: not a parsable TrueType font ({} bytes)", size);
        delete info;
        return kInvalidFont;
    }
    record.info = info;
    // 注册表下标即 FontId：顺序同时是回退优先级，追加后不再变动。
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
    // 位打包：[63..48] 字体下标 | [47..16] 字形索引 | [15..0] 整数像素高。
    // 三者都在各自位宽内（字形索引用 32 位段是余量，TTF 实际 16 位）。
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
    // 行度量只取首选字体（非法时退到第 0 个）：回退字体不参与，
    // 行高不随内容里有没有中文/emoji 而跳动（与 CSS 行高策略一致）。
    const int index = (preferred >= 0 && preferred < static_cast<int>(fonts_.size()))
                          ? preferred : 0;
    if (fonts_.empty()) {
        *ascent = 0.0f;
        *descent = 0.0f;
        return;
    }
    const auto* info = static_cast<const stbtt_fontinfo*>(fonts_[index].info);
    // em → 像素的换算系数：字体内所有度量都按它缩放。
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
    // 排布核心：逐码点选字体、查字形索引、取推进宽度与字距。
    // 不做双向/整形（无 HarfBuzz），复杂文字（阿拉伯语等）暂不支持。
    std::vector<GlyphRunItem> run;
    if (!utf8 || fonts_.empty()) {
        return run;
    }
    int prevFont = -1;
    int prevGlyph = -1;
    size_t i = 0;
    while (utf8[i] != '\0') {
        const size_t byteOffset = i;
        const uint32_t cp = decodeUtf8(utf8, i);
        const int fontIndex = resolveFont(cp, preferred);
        if (fontIndex < 0) {
            break; // 无可用字体（fonts_ 为空已在上面拦住，这里仅防御）
        }
        const auto* info = static_cast<const stbtt_fontinfo*>(fonts_[fontIndex].info);
        const int gid = stbtt_FindGlyphIndex(info, cp);
        // 每个字体各自缩放到同一 sizePx：混排时名义 em 尺寸一致。
        const float scale = stbtt_ScaleForMappingEmToPixels(info, sizePx);

        GlyphRunItem item;
        item.fontIndex = fontIndex;
        item.glyphIndex = gid;
        item.kern = 0.0f;
        item.codepoint = cp;
        item.byteOffset = byteOffset;
        item.byteLength = i - byteOffset;
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
        // 货架法打包：当前行放得下就接着放；横向放满、或新字形比当前
        // 行高（高字形换行而不是垫高整行，省横向空间）则换行；换行也
        // 放不下才开新页。
        const int needW = w + kGlyphPadding * 2;
        const int needH = h + kGlyphPadding * 2;
        int pageIndex = -1;
        for (int p = static_cast<int>(pages_.size()) - 1; p >= 0; --p) {
            AtlasPage& page = pages_[p];
            const bool wrap = page.cursorX + needW > kAtlasPageSize ||
                              page.rowHeight < needH;
            const int nextX = wrap ? needW : page.cursorX + needW;
            const int nextY = wrap ? page.cursorY + page.rowHeight
                                   : page.cursorY;
            const int nextBottom = wrap ? nextY + needH
                                        : page.cursorY + page.rowHeight;
            if (nextX <= kAtlasPageSize && nextBottom <= kAtlasPageSize) {
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
            // atlas 页不要 mip 链：页会随新字形反复重传（每级都要重算），
            // 而且文字恒 1:1 采样用不到缩小过滤，低级 mip 还会把相邻字形
            // 糊在一起（留边只有 1px）。
            page.texture = TextureStore::instance().addTexture(
                static_cast<uint32_t>(kAtlasPageSize),
                static_cast<uint32_t>(kAtlasPageSize), nullptr,
                /*mipmapped=*/false);
            pageIndex = static_cast<int>(pages_.size());
            pages_.push_back(std::move(page));
        }
        AtlasPage& page = pages_[pageIndex];
        // 与上面的 fit 检查同一个换行条件：横向放满或行高不够都换行。
        if (page.cursorX + needW > kAtlasPageSize || page.rowHeight < needH) {
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
        // 只把本字形的货架格（含留边）报脏：渲染器局部上传这块矩形，
        // 不再因一个字形整页 4MB 重传。
        TextureStore::instance().markDirtyRegion(
            page.texture, static_cast<uint32_t>(dstX),
            static_cast<uint32_t>(dstY), static_cast<uint32_t>(needW),
            static_cast<uint32_t>(needH));
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
    // 宽度 = 逐字形推进累加；高度取行盒（ascent+descent），
    // 与具体文本内容无关——同一字号的"Ag"和"oo"测出同样的高。
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

std::vector<GlyphAdvance> FontEngine::measureGlyphs(const char* utf8,
                                                    float sizePx,
                                                    FontId preferred) const {
    std::vector<GlyphAdvance> result;
    for (const GlyphRunItem& item : layoutRun(utf8, sizePx, preferred)) {
        // kern 折算进后一个字形的推进：累加总和与 measureText 一致，
        // 逐字形右缘只差一个 kern 量级，对断行判断无影响。
        result.push_back(
            {item.codepoint, item.byteOffset, item.byteLength,
             item.advance + item.kern});
    }
    return result;
}

void FontEngine::forEachGlyph(const char* utf8, float sizePx, FontId preferred,
                              const std::function<void(const PlacedGlyph&)>& fn) {
    if (!fn || fonts_.empty()) {
        return;
    }
    float ascent = 0.0f;
    float descent = 0.0f;
    lineMetrics(sizePx, preferred, &ascent, &descent);
    // pen 是笔尖的 x 坐标（基线上的当前位置）；kern 先作用于前一字形
    // 与本字形之间，再算本字形摆放，最后推进 advance。
    float pen = 0.0f;
    for (const GlyphRunItem& item : layoutRun(utf8, sizePx, preferred)) {
        pen += item.kern;
        const CachedGlyph* glyph = rasterizeGlyph(item.fontIndex, item.glyphIndex, sizePx);
        // 空白字形（空格等）没有位图可画：跳过回调，但 pen 照常推进。
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

void FontEngine::prewarm(const char* utf8, float sizePx, FontId preferred) {
    if (!utf8 || sizePx <= 0.0f) {
        return;
    }
    // forEachGlyph 对排布出的每个字形调 rasterizeGlyph：未缓存的字形
    // 当即光栅化进 atlas；回调丢弃排布结果即可。
    forEachGlyph(utf8, sizePx, preferred, [](const PlacedGlyph&) {});
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
