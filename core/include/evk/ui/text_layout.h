#pragma once

/**
 * @file text_layout.h
 * @brief 多行文本排版：定宽自动换行、行距/对齐/行数截断与按行绘制。
 *
 * 纯 CPU 模块，不依赖 Vulkan——主机侧测试可以完整跑。宽度测量走
 * FontEngine::measureGlyphs（不触发光栅化），绘制时才逐行调
 * Canvas::drawText（字形按需进 atlas）。
 *
 * 断行规则：可断点由 libunibreak 按 UAX #14 计算（third_party/libunibreak，
 * lang=zh，中文标点避头尾生效——闭标点不落到行首、开标点不留在行尾），
 * 本项目只保留少量壳规则：
 *   - '\n' 强制断行；
 *   - 超宽时回退到行内最近可断点；断行处/行尾的空格丢弃，行首空白跳过；
 *   - 行内无任何可断点（超长英文单词）时硬断。
 * 不做 shaping/ICU BiDi 等复杂规则（仅面向中英文）。
 *
 * 排版表现：
 *   - 行距倍数 lineHeightScale（CSS line-height 风格）：行高 =
 *     自然行高（ascent+descent）× 倍数，多出空间上下均分（half-leading）；
 *   - 逐行水平对齐 TextAlign（左/中/右，限宽时生效）；
 *   - maxLines 行数上限：超出行丢弃，末行按宽度削尾并补"…"。
 *
 * 单线程模型：全部接口只在 UI 线程调用，内部无锁。
 * 排版结果以（文本, 字号, 字体, 宽度, 行距, 对齐, 行数上限）为键缓存：
 * 相同输入的重复 layout() 直接复用，不重排。
 */
#include <cstdint>
#include <string>
#include <vector>

#include "evk/ui/font_engine.h"
#include "evk/ui/paint_canvas.h"
#include "evk/ui/render_view.h"

namespace evk::ui {

/// 段落内逐行水平对齐方式（对照 CSS text-align；无两端对齐）。
enum class TextAlign {
    kLeft,  ///< 左对齐（默认）
    kCenter, ///< 居中
    kRight, ///< 右对齐
};

/**
 * @brief 定宽多行排版器：layout 一次，lineCount/totalHeight/paint 反复用。
 *
 * 每行记录的是原文 UTF-8 的字节区间（换行不复制文本），绘制时按区间
 * 切出子串逐行交给 Canvas::drawText / PaintContext::drawText。
 */
class TextLayout {
public:
    /// 一行：原文中的字节区间 [begin, end) + 行宽。空行 begin == end。
    struct Line {
        size_t begin = 0; ///< 行首字节下标
        size_t end = 0;   ///< 行尾字节下标（不含）
        float width = 0.0f;    ///< 视觉总宽（含省略号；对齐用）
        float textWidth = 0.0f; ///< 正文宽（不含省略号；定位省略号用）
        bool ellipsized = false; ///< 被 maxLines 截断，尾部需画"…"
    };

    /**
     * @brief 按给定宽度排版一段 UTF-8 文本。
     *
     * 输入（文本/字号/字体/宽度/行距/对齐/行数上限）与上次完全相同则
     * 直接复用缓存，不重排。
     * @param utf8 UTF-8 文本（内容拷入内部，入参缓冲随后可释放）
     * @param sizePx 字号（em 像素大小）
     * @param preferred 首选字体；kFontAny = 按注册顺序回退
     * @param maxWidth 行宽上限（像素）；<= 0 表示不软换行（只有 \n 断行）
     * @param lineHeightScale 行距倍数（CSS line-height 风格，1.0 = 字体
     *        自然行高 ascent+descent）；多出的空间上下均分（half-leading）
     * @param align 逐行水平对齐（仅在 maxWidth > 0 时有意义）
     * @param maxLines 行数上限（0 = 不限）；超出的行丢弃，最后一行按
     *        maxWidth 削尾并追加省略号"…"（maxWidth <= 0 时只截不削）
     */
    void layout(const char* utf8, float sizePx, FontId preferred, float maxWidth,
                float lineHeightScale = 1.0f, TextAlign align = TextAlign::kLeft,
                int maxLines = 0);

    /// 行数（空文本为 0）。
    int lineCount() const { return static_cast<int>(lines_.size()); }
    /// 行盒高 = ascent + descent（像素），与 FontEngine 单行测量一致。
    float lineHeight() const { return lineHeight_; }
    /// 内容总高 = 行数 × 行高（无行距）。
    float totalHeight() const {
        return static_cast<float>(lines_.size()) * lineHeight_;
    }
    /// 全部行的字节区间（调试/测试用）。
    const std::vector<Line>& lines() const { return lines_; }
    /// 取一行的原文切片（调试/测试用）。
    std::string lineText(int index) const;
    /// 实际重排次数：相同输入的 layout() 不增加（缓存生效的观测口）。
    int revision() const { return revision_; }

    /**
     * @brief 逐行绘制（屏幕坐标）：只画与 clip 相交的行，视口外的行
     *        不产生顶点。
     * @param canvas 帧顶点流
     * @param x 文本块左上角 x（屏幕像素）
     * @param y 文本块左上角 y（屏幕像素）
     * @param color 文字颜色
     * @param clip 裁剪矩形（屏幕坐标）
     */
    void paint(Canvas& canvas, float x, float y, Color color,
               const Rect& clip) const;

    /**
     * @brief 逐行绘制（视图局部坐标）：只画与 PaintContext 裁剪区相交的行。
     * @param paint 视图绘制上下文
     * @param x 文本块左上角 x（视图局部坐标）
     * @param y 文本块左上角 y（视图局部坐标）
     * @param rgba 文字颜色
     */
    void paint(PaintContext& paint, float x, float y, uint32_t rgba) const;

private:
    /// 成行：修剪行尾空白后，把字形区间 [begin, end) 折算成字节区间入表。
    void pushLine(const std::vector<GlyphAdvance>& glyphs,
                  const std::vector<float>& prefix, size_t begin, size_t end);
    /// maxLines 截断：丢弃超出行，最后一行按 maxWidth 削尾并标记省略号。
    void applyMaxLines(const std::vector<GlyphAdvance>& glyphs,
                       const std::vector<float>& prefix);
    /// 一行的水平偏移（对齐用；kLeft 或不限宽时恒为 0）。
    float alignOffset(float lineWidth) const;

    std::string text_;        ///< 文本自有拷贝（行区间指向它）
    float sizePx_ = 0.0f;     ///< 字号
    FontId font_ = kFontAny;  ///< 首选字体
    float maxWidth_ = 0.0f;   ///< 行宽上限
    float lineHeightScale_ = 1.0f; ///< 行距倍数
    TextAlign align_ = TextAlign::kLeft; ///< 水平对齐
    int maxLines_ = 0;        ///< 行数上限（0 = 不限）
    float lineHeight_ = 0.0f; ///< 行盒高（自然行高 × 行距倍数）
    float leadingHalf_ = 0.0f;   ///< 行距均分后加在行盒顶部的一半
    float ellipsisWidth_ = 0.0f; ///< "…" 的宽度（截断削尾用）
    int revision_ = 0;        ///< 实际重排次数
    bool valid_ = false;      ///< 缓存是否可用（首次 layout 前为 false）
    std::vector<Line> lines_; ///< 行表
};

} // namespace evk::ui
