#pragma once

/**
 * @file texture_source.h
 * @brief 渲染器读取 UI 纹理的抽象接口：数量、尺寸、mip 链与脏页语义。
 */
#include <cstddef>
#include <cstdint>

namespace evk::gpu {

/// 像素矩形（脏区域并集用；单位像素，w/h 为 0 表示空区域）。
struct TextureRegion {
    uint32_t x = 0, y = 0, w = 0, h = 0;
};

/**
 * @brief UI 纹理数据源（字形 atlas 页 + 业务位图）的查询/脏页接口。
 *
 * TextureCache 只经本接口取纹理信息与像素，不再直接依赖 UI 层的
 * TextureStore 单例；UI 层用 header-only 适配器（ui/texture_store_source.h）
 * 把 TextureStore 接到本接口。纹理 id 从 1 起，0 保留给渲染器的白纹理。
 */
class ITextureSource {
public:
    virtual ~ITextureSource() = default;

    /// 已注册纹理数（最大有效 id = textureCount()）。
    virtual int textureCount() const = 0;

    virtual uint32_t width(uint32_t id) const = 0;  ///< 宽（像素）
    virtual uint32_t height(uint32_t id) const = 0; ///< 高（像素）

    /// 该纹理是否生成 mip 链（false 时 mipLevelCount/mipChainBytes 按 1 级处理）。
    virtual bool mipmapped(uint32_t id) const = 0;

    /**
     * @brief 完整 mip 链层数：逐级缩半直到 1x1。非 mipmapped 纹理恒为 1。
     */
    virtual uint32_t mipLevelCount(uint32_t id) const = 0;

    /// 完整 mip 链的总字节数（各级 RGBA8、行紧密、级间紧密排列）。
    virtual size_t mipChainBytes(uint32_t id) const = 0;

    /**
     * @brief 导出整个 mip 链的 RGBA8 字节：level 0 为原图，之后逐级缩半。
     * @param id 纹理 id
     * @param destination 接收缓冲，容量至少为 mipChainBytes(id)
     * @param destinationSize 缓冲字节数
     * @return 参数及容量有效时返回 true
     */
    virtual bool copyMipChain(uint32_t id, uint8_t* destination,
                              size_t destinationSize) const = 0;

    /**
     * @brief 导出 level 0 的一个像素矩形（RGBA8、行紧密排列）。
     *
     * 脏区域局部上传用：只转换并拷出脏区，而非整张纹理。
     * @param destination 接收缓冲，容量至少为 w*h*4
     * @return 参数越界或容量不足时返回 false
     */
    virtual bool copyRegion(uint32_t id, uint32_t x, uint32_t y, uint32_t w,
                            uint32_t h, uint8_t* destination,
                            size_t destinationSize) const = 0;

    /**
     * @brief 读取并清除脏标记；true = 需要重新上传。
     * @param outRegion 非空时输出自上次上传以来修改区域的并集
     *        （新建纹理/整图重改为整张纹理）
     */
    virtual bool consumeDirty(uint32_t id, TextureRegion* outRegion) = 0;
};

} // namespace evk::gpu
