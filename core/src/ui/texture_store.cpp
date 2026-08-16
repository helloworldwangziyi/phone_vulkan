/**
 * @file texture_store.cpp
 * @brief 统一纹理仓库实现：RGBA 像素登记与脏页管理。
 */
#include "evk/ui/texture_store.h"

#include <cstring>

namespace evk::ui {

TextureStore& TextureStore::instance() {
    static TextureStore store;
    return store;
}

TextureId TextureStore::addTexture(uint32_t width, uint32_t height,
                                   const uint32_t* rgbaPixels) {
    if (width == 0 || height == 0) {
        return kInvalidTexture;
    }
    Entry entry;
    entry.width = width;
    entry.height = height;
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
