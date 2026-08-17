#pragma once

/**
 * @file texture_store.h
 * @brief 统一纹理仓库：字形 atlas 页与业务位图共用一套 id / 像素 / 脏页管理。
 *
 * CPU 侧的纹理登记处（RGBA8888、行紧密排列），渲染器每帧把"新建或脏"
 * 的纹理整张上传 GPU 并按 id 建 descriptor。三类用户：
 *   - FontEngine：字形 atlas 页（rgb 恒白、a 为覆盖率）；
 *   - App：drawImage 用的任意位图（顶点色作染色）；
 *   - 渲染器：id 0 固定为 1x1 白纹理（纯色批次的占位），不经本仓库。
 * 单线程模型：全部接口只在 UI 线程调用，内部无锁。
 */
#include <cstddef>
#include <cstdint>
#include <vector>

namespace evk::ui {

/// 纹理句柄：从 1 起；0 保留给渲染器的白纹理。
using TextureId = uint32_t;
constexpr TextureId kInvalidTexture = 0;

class TextureStore {
public:
    static TextureStore& instance();

    /**
     * @brief 注册一张 RGBA8888 纹理（像素按行紧密排列，数据会被拷贝）。
     * @param width 宽（像素）
     * @param height 高（像素）
     * @param rgbaPixels 每像素一个 0xRRGGBBAA；nullptr 表示全透明初始化
     * @param mipmapped true = 上传时为它生成完整 mip 链（缩小采样抗锯齿）；
     *                  字形 atlas 页传 false——页会反复改传，且低级 mip
     *                  会把相邻字形糊在一起
     * @return 纹理句柄（从 1 起，单调递增）
     */
    TextureId addTexture(uint32_t width, uint32_t height, const uint32_t* rgbaPixels,
                         bool mipmapped = true);

    /// 已注册纹理数（最大有效 id = textureCount()）。
    int textureCount() const;

    /// 指定纹理的像素（只读；渲染器上传与绘制侧采样用）。
    const uint32_t* pixels(TextureId id) const;

    /// 指定纹理的可写像素（FontEngine 写字形位图用；写完记得 markDirty）。
    uint32_t* mutablePixels(TextureId id);

    uint32_t width(TextureId id) const;  ///< 宽（像素）
    uint32_t height(TextureId id) const; ///< 高（像素）

    /**
     * @brief 按 R、G、B、A 字节顺序导出像素，供图形 API 上传。
     *
     * 仓库对外使用便于阅读的 0xRRGGBBAA 数值；该数值在小端 CPU 内存中
     * 并不是 RGBA 字节序，因此不能 reinterpret_cast 后直接交给 Vulkan。
     * @param id 纹理句柄
     * @param destination 接收连续 RGBA8 字节的缓冲
     * @param destinationSize 缓冲字节数，至少为 width * height * 4
     * @return 参数及容量有效时返回 true
     */
    bool copyRgbaBytes(TextureId id, uint8_t* destination,
                       size_t destinationSize) const;

    /// 该纹理是否生成 mip 链（注册时指定；false 时以下两个接口按 1 级处理）。
    bool mipmapped(TextureId id) const;

    /**
     * @brief 完整 mip 链层数：逐级缩半直到 1x1。非 mipmapped 纹理恒为 1。
     */
    uint32_t mipLevelCount(TextureId id) const;

    /// 完整 mip 链的总字节数（各级 RGBA8、行紧密、级间紧密排列）。
    size_t mipChainBytes(TextureId id) const;

    /**
     * @brief 导出整个 mip 链：level 0 为原图，之后逐级 2x2 缩半。
     *
     * 缩半用「按 alpha 加权的预乘平均」：颜色先乘透明度再求均值，
     * 避免普通 box filter 把全透明像素的颜色混进半透明边缘（光晕）。
     * @param id 纹理句柄
     * @param destination 接收缓冲，容量至少为 mipChainBytes(id)
     * @param destinationSize 缓冲字节数
     * @return 参数及容量有效时返回 true
     */
    bool copyMipChain(TextureId id, uint8_t* destination,
                      size_t destinationSize) const;

    /// 读取并清除脏标记；true = 需要整张重新上传。
    bool consumeDirty(TextureId id);

    /// 标记脏（新增纹理初始即为脏；动态改图后调用）。
    void markDirty(TextureId id);

    /// 清空全部登记（测试用；运行中调用会让既有 id 失效）。
    void reset();

private:
    TextureStore() = default;

    struct Entry {
        uint32_t width = 0;
        uint32_t height = 0;
        std::vector<uint32_t> data; ///< 每像素 0xRRGGBBAA，行紧密排列
        bool mipmapped = true; ///< 上传时是否生成 mip 链（业务位图开，atlas 页关）
        bool dirty = true; ///< 新建/被改：待渲染器整张上传
    };

    /// 下标 = id - 1（id 0 是渲染器白纹理，不入表）。
    std::vector<Entry> entries_;
};

} // namespace evk::ui
