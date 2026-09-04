#pragma once

/**
 * @file texture_store_source.h
 * @brief TextureStore 单例 → gpu::ITextureSource 的 header-only 适配器。
 *
 * 渲染侧只认 gpu::ITextureSource；本适配器把全部调用转发给
 * TextureStore::instance()，由 Compositor 装配进渲染器。
 */
#include "evk/gpu/texture_source.h"
#include "evk/ui/texture_store.h"

namespace evk::ui {

class TextureStoreSource final : public gpu::ITextureSource {
public:
    int textureCount() const override {
        return TextureStore::instance().textureCount();
    }
    uint32_t width(uint32_t id) const override {
        return TextureStore::instance().width(id);
    }
    uint32_t height(uint32_t id) const override {
        return TextureStore::instance().height(id);
    }
    bool mipmapped(uint32_t id) const override {
        return TextureStore::instance().mipmapped(id);
    }
    uint32_t mipLevelCount(uint32_t id) const override {
        return TextureStore::instance().mipLevelCount(id);
    }
    size_t mipChainBytes(uint32_t id) const override {
        return TextureStore::instance().mipChainBytes(id);
    }
    bool copyMipChain(uint32_t id, uint8_t* destination,
                      size_t destinationSize) const override {
        return TextureStore::instance().copyMipChain(id, destination, destinationSize);
    }
    bool copyRegion(uint32_t id, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                    uint8_t* destination, size_t destinationSize) const override {
        return TextureStore::instance().copyRgbaRegion(id, x, y, w, h,
                                                       destination,
                                                       destinationSize);
    }
    bool consumeDirty(uint32_t id, gpu::TextureRegion* outRegion) override {
        return TextureStore::instance().consumeDirty(id, outRegion);
    }
};

} // namespace evk::ui
