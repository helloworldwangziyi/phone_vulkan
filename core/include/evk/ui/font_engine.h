#pragma once

/**
 * @file font_engine.h
 * @brief 字体引擎：TTF 解析、多字体回退、按需字形光栅化与 atlas 打包。
 *
 * 纯 CPU 模块，不依赖 Vulkan——主机侧测试可以完整跑。字形 atlas 页
 * 登记在 TextureStore（RGBA，rgb 恒白、a 为覆盖率），渲染器经统一
 * 纹理通道上传与采样。
 *
 * 单线程模型：全部接口只在 UI 线程调用，内部无锁。
 * 字形缓存以 (字体, 字形索引, 整数像素高) 为键；每个字号独立光栅化，
 * UI 常用字号有限，暂不做 SDF/多尺寸复用（后续可拓展）。
 */
#include <cstddef>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

#include "evk/ui/texture_store.h"

namespace evk::ui {

/// 字体句柄：addFont 按注册顺序发放，从 0 起；kFontAny(-1) 表示无偏好。
using FontId = int32_t;
constexpr FontId kInvalidFont = -1;
/// 无偏好字体：排版时完全按注册顺序的回退链选字。
constexpr FontId kFontAny = -1;

/**
 * @brief 排版后的一个字形：atlas 位置 + 屏幕几何。
 *
 * 由 forEachGlyph 逐个吐出，Canvas 把它转成纹理四边形。
 * 坐标全部相对"文本行盒子的左上角"（y 向下），行盒高 = ascent+descent。
 */
struct PlacedGlyph {
    TextureId texture;    ///< 字形所在的 atlas 页（TextureStore 句柄）
    float u0, v0, u1, v1; ///< 归一化纹理坐标（已做半像素内缩，防相邻字形渗色）
    float x, y;           ///< 字形四边形左上角（相对行盒子左上角）
    float w, h;           ///< 字形四边形屏幕尺寸（像素）
};

/**
 * @brief 逐码点推进宽度（纯测量结果，不触发光栅化）。
 *
 * 由 measureGlyphs 按原文顺序吐出；携带字节区间，调用方据此把
 * 码点流映射回原文 UTF-8 切片（逐行绘制、断行回退都要用）。
 */
struct GlyphAdvance {
    uint32_t codepoint; ///< 码点（非法 UTF-8 段解码为 U+FFFD）
    size_t byteOffset;  ///< 在输入串中的字节起点
    size_t byteLength;  ///< UTF-8 编码字节数
    float advance;      ///< 推进宽度（像素）；与前一字形的 kern 已折算在内
};

/**
 * @brief 字体引擎单例：字体注册 → 文本排版 → 字形 atlas。
 *
 * 排版按"首选字体优先，缺字沿注册顺序回退"的规则逐码点解析：
 * 典型配置是 Latin 字体在前（英文数字用它，字形小而精细）、
 * CJK 字体在后补全汉字；混排文本一次遍历自动分派。
 * 所有字体都没有的码点退化为首选字体的 .notdef（豆腐块）。
 */
class FontEngine {
public:
    static FontEngine& instance();

    /**
     * @brief 注册一个 TrueType 字体（stb_truetype 解析，仅支持 glyf 轮廓的 TTF）。
     *
     * 数据会被拷贝进引擎内部，调用方传入的缓冲随后可释放。
     * @param ttfData 字体文件字节流（TTF；OTF/CFF 不支持）
     * @param size 字节数
     * @return 字体句柄；解析失败返回 kInvalidFont
     */
    FontId addFont(const void* ttfData, size_t size);

    /// 已注册字体数。
    int fontCount() const;

    /// 清空全部字体与 atlas（测试用；运行中调用会让既有 FontId 失效）。
    void reset();

    /**
     * @brief 单行文本测量（不绘制，不触发光栅化）。
     * @param utf8 UTF-8 文本
     * @param sizePx 字号（em 像素大小）
     * @param preferred 首选字体；kFontAny = 无偏好（纯回退链）
     * @param outWidth 输出：行宽（像素）；可为 nullptr
     * @param outHeight 输出：行盒高 = ascent+descent（像素）；可为 nullptr
     * @return 行宽（像素），与 outWidth 相同
     */
    float measureText(const char* utf8, float sizePx, FontId preferred,
                      float* outWidth = nullptr, float* outHeight = nullptr) const;

    /**
     * @brief 逐码点测量推进宽度（不绘制，不触发光栅化）。
     *
     * 与 measureText 共用同一条排布逻辑（layoutRun），只是把"每个码点
     * 多宽"逐字吐出来——多行排版（TextLayout）按它累加行宽定断点。
     * 行高走 measureText 的 outHeight（不依赖文本内容）。
     * @param utf8 UTF-8 文本
     * @param sizePx 字号（em 像素大小）
     * @param preferred 首选字体；kFontAny = 无偏好（纯回退链）
     * @return 逐码点测量结果，顺序与原文一致
     */
    std::vector<GlyphAdvance> measureGlyphs(const char* utf8, float sizePx,
                                            FontId preferred) const;

    /**
     * @brief 单行排版并逐字形回调（测量与绘制共用同一条排布逻辑）。
     *
     * 首次遇到的字形会在 atlas 里按需光栅化（页登记进 TextureStore 并
     * 打脏标记），渲染器随后把脏纹理上传 GPU（当帧生效）。
     * @param utf8 UTF-8 文本
     * @param sizePx 字号（em 像素大小）
     * @param preferred 首选字体；kFontAny = 无偏好
     * @param fn 每个字形回调一次，参数是排布结果
     */
    void forEachGlyph(const char* utf8, float sizePx, FontId preferred,
                      const std::function<void(const PlacedGlyph&)>& fn);

    /**
     * @brief 预热字形缓存：排版指定文本并光栅化全部字形，丢弃排布结果。
     *
     * 字形是懒光栅化的——某个 (字体, 字形, 字号) 组合首次上屏时才跑
     * stb_truetype 光栅化并把 atlas 页打脏重传。页面首次进入的转场动画
     * 正好撞上这波开销（逐字光栅化 + 整页 atlas 重传），表现为首帧卡顿。
     * 启动时（或闲时）把已知文案预热一遍，之后的绘制全部命中缓存。
     * @param utf8 UTF-8 文本
     * @param sizePx 字号（em 像素大小）——必须与绘制时完全一致才命中缓存
     * @param preferred 首选字体——必须与绘制时一致：首选字体不同，同一码点
     *        可能解析到不同字体，缓存键就不同
     */
    void prewarm(const char* utf8, float sizePx, FontId preferred);

    // ---- atlas 访问（渲染层/测试消费，像素细节走 TextureStore） ----

    /// atlas 页数（随字形增多而增长，页满自动开新页）。
    int pageCount() const;
    /// 指定页对应的 TextureStore 句柄。
    TextureId pageTexture(int page) const;
    /// atlas 页边长（方形，当前固定 1024）。
    int pageSize() const;

private:
    FontEngine() = default;

    struct FontRecord {
        std::vector<unsigned char> data; ///< 字体文件字节（自有拷贝）
        void* info = nullptr;            ///< stbtt_fontinfo（前向隐藏实现细节）
    };

    struct CachedGlyph {
        TextureId texture; ///< 所在 atlas 页（TextureStore 句柄）
        int x, y;       ///< 页内像素位置（含 1px 留边）
        int w, h;       ///< 光栅化尺寸（不含留边）
        float xoff, yoff; ///< 相对"笔尖基线"的摆放偏移（stbtt 惯例，y 向下）
        float advance;  ///< 推进宽度（像素）
    };

    /// 一页 atlas：货架法打包游标 + TextureStore 里的像素本体。
    struct AtlasPage {
        TextureId texture = kInvalidTexture;
        int cursorX = 0;   ///< 当前行写入位置
        int cursorY = 0;   ///< 当前行的 y 起点
        int rowHeight = 0; ///< 当前行已用高度（同行按最高字形对齐）
    };

    /// 排版用的解析结果（measureText / measureGlyphs / forEachGlyph 共用）。
    struct GlyphRunItem {
        int fontIndex;   ///< 实际选中的字体（注册表下标）
        int glyphIndex;  ///< 字形索引（stbtt 的 gid）
        float advance;   ///< 本字形推进（像素）
        float kern;      ///< 与前一个字形的字距调整（像素）
        uint32_t codepoint; ///< 码点（measureGlyphs 用）
        size_t byteOffset;  ///< 在输入串中的字节起点（measureGlyphs 用）
        size_t byteLength;  ///< UTF-8 编码字节数（measureGlyphs 用）
    };

    /// (字体, 字形, 整数像素高) → 缓存条目。
    static uint64_t makeKey(int fontIndex, int glyphIndex, int pxSize);

    /// 查缓存；未命中则光栅化并打进 atlas（写脏标记），失败返回 nullptr。
    const CachedGlyph* rasterizeGlyph(int fontIndex, int glyphIndex, float sizePx);

    /// 按回退规则为码点选字体，返回注册表下标；都不含则返回 -1。
    int resolveFont(uint32_t codepoint, FontId preferred) const;

    /// 解析整行文本为 GlyphRunItem 序列（两条路径共享的排布核心）。
    std::vector<GlyphRunItem> layoutRun(const char* utf8, float sizePx, FontId preferred) const;

    /// 首选字体（或第 0 个字体）在 sizePx 下的行度量。
    void lineMetrics(float sizePx, FontId preferred, float* ascent, float* descent) const;

    std::vector<FontRecord> fonts_; ///< 注册表（顺序即回退顺序）
    std::vector<AtlasPage> pages_;  ///< atlas 页（像素在 TextureStore）
    std::unordered_map<uint64_t, CachedGlyph> glyphCache_; ///< 字形缓存
};

} // namespace evk::ui
