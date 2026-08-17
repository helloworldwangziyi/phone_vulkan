/**
 * @file texture_store.cpp
 * @brief 统一纹理仓库实现：RGBA 像素登记与脏页管理。
 */
#include "evk/ui/texture_store.h"

#include <algorithm>
#include <cstring>

namespace evk::ui {

TextureStore& TextureStore::instance() {
    static TextureStore store;
    return store;
}

TextureId TextureStore::addTexture(uint32_t width, uint32_t height,
                                   const uint32_t* rgbaPixels, bool mipmapped) {
    if (width == 0 || height == 0) {
        return kInvalidTexture;
    }
    Entry entry;
    entry.width = width;
    entry.height = height;
    entry.mipmapped = mipmapped;
    entry.data.assign(static_cast<size_t>(width) * height, 0);
    if (rgbaPixels) {
        std::memcpy(entry.data.data(), rgbaPixels,
                    sizeof(uint32_t) * entry.data.size());
    }
    entries_.push_back(std::move(entry));
    return static_cast<TextureId>(entries_.size());
}

int TextureStore::textureCount() const {
    return static_cast<int>(entries_.size());
}

const uint32_t* TextureStore::pixels(TextureId id) const {
    if (id == 0 || id > entries_.size()) {
        return nullptr;
    }
    return entries_[id - 1].data.data();
}

uint32_t* TextureStore::mutablePixels(TextureId id) {
    if (id == 0 || id > entries_.size()) {
        return nullptr;
    }
    return entries_[id - 1].data.data();
}

uint32_t TextureStore::width(TextureId id) const {
    if (id == 0 || id > entries_.size()) {
        return 0;
    }
    return entries_[id - 1].width;
}

uint32_t TextureStore::height(TextureId id) const {
    if (id == 0 || id > entries_.size()) {
        return 0;
    }
    return entries_[id - 1].height;
}

bool TextureStore::copyRgbaBytes(TextureId id, uint8_t* destination,
                                 size_t destinationSize) const {
    if (id == 0 || id > entries_.size() || !destination) {
        return false;
    }
    const Entry& entry = entries_[id - 1];
    const size_t required = entry.data.size() * 4;
    if (destinationSize < required) {
        return false;
    }
    for (size_t i = 0; i < entry.data.size(); ++i) {
        const uint32_t rgba = entry.data[i];
        destination[i * 4] = static_cast<uint8_t>(rgba >> 24);
        destination[i * 4 + 1] = static_cast<uint8_t>(rgba >> 16);
        destination[i * 4 + 2] = static_cast<uint8_t>(rgba >> 8);
        destination[i * 4 + 3] = static_cast<uint8_t>(rgba);
    }
    return true;
}

bool TextureStore::mipmapped(TextureId id) const {
    if (id == 0 || id > entries_.size()) {
        return false;
    }
    return entries_[id - 1].mipmapped;
}

uint32_t TextureStore::mipLevelCount(TextureId id) const {
    if (id == 0 || id > entries_.size() || !entries_[id - 1].mipmapped) {
        return 1;
    }
    // 逐级缩半直到 1x1 的完整链：levels = floor(log2(max(w,h))) + 1。
    uint32_t levels = 1;
    uint32_t m = std::max(entries_[id - 1].width, entries_[id - 1].height);
    while (m > 1) {
        m >>= 1;
        ++levels;
    }
    return levels;
}

size_t TextureStore::mipChainBytes(TextureId id) const {
    if (id == 0 || id > entries_.size()) {
        return 0;
    }
    const Entry& entry = entries_[id - 1];
    const uint32_t levels = mipLevelCount(id);
    size_t total = 0;
    uint32_t w = entry.width;
    uint32_t h = entry.height;
    for (uint32_t i = 0; i < levels; ++i) {
        total += static_cast<size_t>(w) * h * 4;
        w = std::max(1u, w >> 1);
        h = std::max(1u, h >> 1);
    }
    return total;
}

bool TextureStore::copyMipChain(TextureId id, uint8_t* destination,
                                size_t destinationSize) const {
    if (id == 0 || id > entries_.size() || !destination) {
        return false;
    }
    const Entry& entry = entries_[id - 1];
    const uint32_t levels = mipLevelCount(id);
    if (destinationSize < mipChainBytes(id)) {
        return false;
    }

    // level 0：与 copyRgbaBytes 相同的 0xRRGGBBAA → RGBA 字节序转换。
    uint32_t srcW = entry.width;
    uint32_t srcH = entry.height;
    for (size_t i = 0; i < entry.data.size(); ++i) {
        const uint32_t rgba = entry.data[i];
        destination[i * 4] = static_cast<uint8_t>(rgba >> 24);
        destination[i * 4 + 1] = static_cast<uint8_t>(rgba >> 16);
        destination[i * 4 + 2] = static_cast<uint8_t>(rgba >> 8);
        destination[i * 4 + 3] = static_cast<uint8_t>(rgba);
    }

    // 逐级缩半。src 指向上一级（刚写完的字节），dst 写当前级，级间紧密排列。
    const uint8_t* src = destination;
    uint8_t* dst = destination + static_cast<size_t>(srcW) * srcH * 4;
    for (uint32_t level = 1; level < levels; ++level) {
        const uint32_t dstW = std::max(1u, srcW >> 1);
        const uint32_t dstH = std::max(1u, srcH >> 1);
        for (uint32_t y = 0; y < dstH; ++y) {
            for (uint32_t x = 0; x < dstW; ++x) {
                // 2x2 块（奇数边钳到有效纹素，边纹素重复计权）。
                // 按 alpha 加权的预乘平均：颜色乘透明度后再求均值，
                // 全透明像素的颜色就不会渗进半透明边缘形成光晕。
                uint32_t sumR = 0, sumG = 0, sumB = 0, sumA = 0;
                for (uint32_t dy = 0; dy < 2; ++dy) {
                    const uint32_t sy = std::min(y * 2 + dy, srcH - 1);
                    for (uint32_t dx = 0; dx < 2; ++dx) {
                        const uint32_t sx = std::min(x * 2 + dx, srcW - 1);
                        const uint8_t* p = src + (static_cast<size_t>(sy) * srcW + sx) * 4;
                        const uint32_t a = p[3];
                        sumR += p[0] * a;
                        sumG += p[1] * a;
                        sumB += p[2] * a;
                        sumA += a;
                    }
                }
                uint8_t* out = dst + (static_cast<size_t>(y) * dstW + x) * 4;
                out[3] = static_cast<uint8_t>((sumA + 2) / 4);
                if (sumA > 0) {
                    out[0] = static_cast<uint8_t>((sumR + sumA / 2) / sumA);
                    out[1] = static_cast<uint8_t>((sumG + sumA / 2) / sumA);
                    out[2] = static_cast<uint8_t>((sumB + sumA / 2) / sumA);
                } else {
                    out[0] = out[1] = out[2] = 0;
                }
            }
        }
        src = dst;
        srcW = dstW;
        srcH = dstH;
        dst += static_cast<size_t>(dstW) * dstH * 4;
    }
    return true;
}

bool TextureStore::consumeDirty(TextureId id) {
    if (id == 0 || id > entries_.size()) {
        return false;
    }
    const bool dirty = entries_[id - 1].dirty;
    entries_[id - 1].dirty = false;
    return dirty;
}

void TextureStore::markDirty(TextureId id) {
    if (id != 0 && id <= entries_.size()) {
        entries_[id - 1].dirty = true;
    }
}

void TextureStore::reset() {
    entries_.clear();
}

} // namespace evk::ui
