#pragma once

/**
 * @file texture_store_source.h
 * @brief Raster 线程私有的纹理副本 → gpu::ITextureSource。
 *
 * UI 只传 TextureUpdate 值快照；本类绝不访问 UI TextureStore 单例。
 * 副本保留完整原图，上传失败可重试；mip 生成与 RGBA 转换都在 Raster。
 */
#include "evk/gpu/texture_source.h"
#include "evk/ui/texture_store.h"
#include <algorithm>
#include <stdexcept>

namespace evk::ui {

class TextureStoreSource final : public gpu::ITextureSource {
public:
    void apply(const std::vector<TextureUpdate>& updates) {
        for (const auto& update : updates) {
            if (update.id == static_cast<uint32_t>(store_.textureCount() + 1)) {
                store_.addTexture(update.width, update.height, nullptr, update.mipmapped);
            }
            const auto& r = update.region;
            if (update.id == 0 || update.id > static_cast<uint32_t>(store_.textureCount()) ||
                store_.width(update.id) != update.width || store_.height(update.id) != update.height ||
                r.x > update.width || r.y > update.height ||
                r.w > update.width - r.x || r.h > update.height - r.y ||
                update.pixels.size() != static_cast<size_t>(r.w) * r.h) {
                throw std::logic_error("Invalid texture snapshot order or region");
            }
            auto* pixels = store_.mutablePixels(update.id);
            for (uint32_t y = 0; y < r.h; ++y) {
                std::copy_n(update.pixels.data() + static_cast<size_t>(y) * r.w, r.w,
                            pixels + static_cast<size_t>(y + r.y) * update.width + r.x);
            }
            store_.markDirtyRegion(update.id, r.x, r.y, r.w, r.h);
        }
    }
    int textureCount() const override {
        return store_.textureCount();
    }
    uint32_t width(uint32_t id) const override {
        return store_.width(id);
    }
    uint32_t height(uint32_t id) const override {
        return store_.height(id);
    }
    bool mipmapped(uint32_t id) const override {
        return store_.mipmapped(id);
    }
    uint32_t mipLevelCount(uint32_t id) const override {
        return store_.mipLevelCount(id);
    }
    size_t mipChainBytes(uint32_t id) const override {
        return store_.mipChainBytes(id);
    }
    bool copyMipChain(uint32_t id, uint8_t* destination,
                      size_t destinationSize) const override {
        return store_.copyMipChain(id, destination, destinationSize);
    }
    bool copyRegion(uint32_t id, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                    uint8_t* destination, size_t destinationSize) const override {
        return store_.copyRgbaRegion(id, x, y, w, h,
                                                       destination,
                                                       destinationSize);
    }
    bool consumeDirty(uint32_t id, gpu::TextureRegion* outRegion) override {
        return store_.consumeDirty(id, outRegion);
    }
private:
    TextureStore store_;
};

} // namespace evk::ui
