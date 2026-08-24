/**
 * @file texture_cache.cpp
 * @brief 纹理 GPU 缓存实现：白纹理 + 数据源纹理的采样对象、descriptor 与上传中转。
 */
#include "evk/gpu/texture_cache.h"

// VulkanContext：设备句柄与显存类型查找；kMaxFramesInFlight 帧槽数。
#include "evk/gpu/vulkan_context.h"
// EVK_LOGE/W 日志宏：spdlog 封装，全局统一的日志入口。
#include "evk/log.h"

// std::max：中转缓冲扩容的边界 clamp 要用。
#include <algorithm>
// std::memset / std::memcpy：白纹理初始化与像素拷贝。
#include <cstring>
#include <limits>
#include <vector>

namespace evk::gpu {

/// 纹理中转缓冲的最小容量，遇到更大的批次时按 2 倍动态扩容。
constexpr VkDeviceSize kInitialTextureUploadCapacity = 64 * 1024;
/// descriptor 池上限：白纹理 + atlas 页 + 业务位图的总预算。
constexpr int kMaxDescriptorSets = 64;

TextureCache::TextureCache(VulkanContext& context, ITextureSource* source)
    : context_(context), source_(source) {}

bool TextureCache::initialize() {
    const VkDevice device = context_.device();

    // 采样器：线性过滤（文字边缘抗锯齿靠它插值覆盖率）+ 三线性 mip
    // （业务位图缩小采样时在相邻两级 mip 间再插值一次，消除闪烁锯齿）。
    // maxLod 拉到 VK_LOD_CLAMP_NONE：实际采样层数被各纹理自身的 levelCount
    // 钳住——白纹理和 atlas 页只有 1 级，自动退化为普通线性过滤。
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
    if (vkCreateSampler(device, &samplerInfo, nullptr, &sampler_) != VK_SUCCESS) {
        EVK_LOGE("vkCreateSampler failed");
        return false;
    }

    // descriptor 布局：binding 0 = 组合图像采样器，片元阶段可读。
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &binding;
    if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &descriptorSetLayout_) != VK_SUCCESS) {
        EVK_LOGE("vkCreateDescriptorSetLayout failed");
        return false;
    }

    // descriptor 池：白纹理 + 全部 atlas 页各占一个 set。
    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = kMaxDescriptorSets;
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = kMaxDescriptorSets;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &descriptorPool_) != VK_SUCCESS) {
        EVK_LOGE("vkCreateDescriptorPool failed");
        return false;
    }

    // 白纹理也是 RGBA8；其四个通道在上传阶段显式写为 255。单级 mip。
    if (!createSampledTexture(whiteTexture_, 1, 1, 1)) {
        return false;
    }

    textureUploadBuffers_.assign(kMaxFramesInFlight, VK_NULL_HANDLE);
    textureUploadMemorys_.assign(kMaxFramesInFlight, VK_NULL_HANDLE);
    textureUploadCapacities_.assign(kMaxFramesInFlight, 0);
    return true;
}

bool TextureCache::ensureTextureUploadCapacity(uint32_t frameSlot,
                                               VkDeviceSize required) {
    const VkDevice device = context_.device();

    if (frameSlot >= textureUploadBuffers_.size() || required == 0) {
        return false;
    }
    if (textureUploadCapacities_[frameSlot] >= required) {
        return true;
    }

    VkDeviceSize capacity = std::max(kInitialTextureUploadCapacity,
                                     textureUploadCapacities_[frameSlot]);
    while (capacity < required) {
        if (capacity > std::numeric_limits<VkDeviceSize>::max() / 2) {
            capacity = required;
            break;
        }
        capacity *= 2;
    }

    VkBuffer newBuffer = VK_NULL_HANDLE;
    VkDeviceMemory newMemory = VK_NULL_HANDLE;
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = capacity;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device, &bufferInfo, nullptr, &newBuffer) != VK_SUCCESS) {
        EVK_LOGE("texture upload vkCreateBuffer failed");
        return false;
    }

    VkMemoryRequirements memReq;
    vkGetBufferMemoryRequirements(device, newBuffer, &memReq);
    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex = context_.findMemoryType(
        memReq.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(device, &allocInfo, nullptr, &newMemory) != VK_SUCCESS) {
        EVK_LOGE("texture upload vkAllocateMemory failed");
        vkDestroyBuffer(device, newBuffer, nullptr);
        return false;
    }
    if (vkBindBufferMemory(device, newBuffer, newMemory, 0) != VK_SUCCESS) {
        EVK_LOGE("texture upload vkBindBufferMemory failed");
        vkFreeMemory(device, newMemory, nullptr);
        vkDestroyBuffer(device, newBuffer, nullptr);
        return false;
    }

    if (textureUploadBuffers_[frameSlot] != VK_NULL_HANDLE) {
        vkDestroyBuffer(device, textureUploadBuffers_[frameSlot], nullptr);
    }
    if (textureUploadMemorys_[frameSlot] != VK_NULL_HANDLE) {
        vkFreeMemory(device, textureUploadMemorys_[frameSlot], nullptr);
    }
    textureUploadBuffers_[frameSlot] = newBuffer;
    textureUploadMemorys_[frameSlot] = newMemory;
    textureUploadCapacities_[frameSlot] = capacity;
    return true;
}

/**
 * @brief 创建一张采样纹理（图像 + 显存 + 视图 + descriptor set）。
 *
 * 像素上传不在这里做——新建的纹理标记 pendingUpload，
 * 首个命令缓冲里经中转缓冲整体拷入（见 uploadPendingTextures）。
 * @param tex 目标纹理对象
 * @param width 图像宽（像素）
 * @param height 图像高（像素）
 * @param mipLevels mip 链层数；>1 时视图覆盖全部层级，上传逐层写入
 * @return true 表示创建成功
 */
bool TextureCache::createSampledTexture(TextureObj& tex, uint32_t width,
                                        uint32_t height, uint32_t mipLevels) {
    const VkDevice device = context_.device();

    tex.mipLevels = std::max(1u, mipLevels);
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.extent = {width, height, 1};
    imageInfo.mipLevels = tex.mipLevels;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device, &imageInfo, nullptr, &tex.image) != VK_SUCCESS) {
        EVK_LOGE("vkCreateImage failed");
        return false;
    }

    VkMemoryRequirements memReq;
    vkGetImageMemoryRequirements(device, tex.image, &memReq);
    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex = context_.findMemoryType(memReq.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(device, &allocInfo, nullptr, &tex.memory) != VK_SUCCESS) {
        EVK_LOGE("texture vkAllocateMemory failed");
        vkDestroyImage(device, tex.image, nullptr);
        tex.image = VK_NULL_HANDLE;
        return false;
    }
    if (vkBindImageMemory(device, tex.image, tex.memory, 0) != VK_SUCCESS) {
        EVK_LOGE("texture vkBindImageMemory failed");
        vkFreeMemory(device, tex.memory, nullptr);
        vkDestroyImage(device, tex.image, nullptr);
        tex.memory = VK_NULL_HANDLE;
        tex.image = VK_NULL_HANDLE;
        return false;
    }

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = tex.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = tex.mipLevels;
    viewInfo.subresourceRange.layerCount = 1;
    if (vkCreateImageView(device, &viewInfo, nullptr, &tex.view) != VK_SUCCESS) {
        EVK_LOGE("vkCreateImageView failed");
        vkDestroyImage(device, tex.image, nullptr);
        vkFreeMemory(device, tex.memory, nullptr);
        tex.image = VK_NULL_HANDLE;
        tex.memory = VK_NULL_HANDLE;
        return false;
    }

    VkDescriptorSetAllocateInfo setInfo{};
    setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    setInfo.descriptorPool = descriptorPool_;
    setInfo.descriptorSetCount = 1;
    setInfo.pSetLayouts = &descriptorSetLayout_;
    if (vkAllocateDescriptorSets(device, &setInfo, &tex.set) != VK_SUCCESS) {
        EVK_LOGE("vkAllocateDescriptorSets failed");
        vkDestroyImageView(device, tex.view, nullptr);
        vkDestroyImage(device, tex.image, nullptr);
        vkFreeMemory(device, tex.memory, nullptr);
        tex.view = VK_NULL_HANDLE;
        tex.image = VK_NULL_HANDLE;
        tex.memory = VK_NULL_HANDLE;
        return false;
    }
    VkDescriptorImageInfo descImage{};
    descImage.sampler = sampler_;
    descImage.imageView = tex.view;
    descImage.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = tex.set;
    write.dstBinding = 0;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.descriptorCount = 1;
    write.pImageInfo = &descImage;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);

    tex.uploaded = false;
    tex.pendingUpload = true;
    return true;
}

void TextureCache::ensureStoreTextures() {
    if (!source_) {
        return;
    }
    const int count = source_->textureCount();
    // [0] 占位（0 号是渲染器自己的白纹理），下标 i 对应数据源第 i 号。
    if (static_cast<int>(storeTextures_.size()) < count + 1) {
        storeTextures_.resize(count + 1);
    }
    for (int id = 1; id <= count; ++id) {
        TextureObj& tex = storeTextures_[id];
        if (tex.image == VK_NULL_HANDLE) {
            // 业务位图建完整 mip 链（缩小采样抗锯齿）；atlas 页注册时
            // 标了 mipmapped=false，保持单级。
            const uint32_t textureId = static_cast<uint32_t>(id);
            const uint32_t mipLevels = source_->mipmapped(textureId)
                                           ? source_->mipLevelCount(textureId) : 1;
            TextureObj created;
            if (!createSampledTexture(created, source_->width(textureId),
                                      source_->height(textureId), mipLevels)) {
                // 本帧建不出就留空槽，下一帧重试；期间该纹理的绘制退化为白块。
                EVK_LOGW("texture {} unavailable; retried next frame", id);
                continue;
            }
            tex = created;
        }
    }
}

void TextureCache::uploadPendingTextures(VkCommandBuffer cmd, uint32_t frameSlot) {
    const VkDevice device = context_.device();

    // 收集本帧要传的纹理。mipmapped 位图要带上整条 mip 链
    // （各级 RGBA8 紧密排列，CPU 预生成），单级纹理 size 仍等于 w*h*4。
    struct PendingCopy {
        TextureObj* tex;
        uint32_t textureId; ///< 数据源纹理 id；0 = 白纹理占位
        VkDeviceSize size;
        uint32_t width, height;
        uint32_t mipLevels;
        bool white;
    };
    std::vector<PendingCopy> copies;
    VkDeviceSize total = 0;
    if (whiteTexture_.pendingUpload) {
        copies.push_back({&whiteTexture_, 0, 4, 1, 1, 1, true});
        total += 4;
    }
    if (source_) {
        for (size_t id = 1; id < storeTextures_.size(); ++id) {
            TextureObj& tex = storeTextures_[id];
            if (tex.image == VK_NULL_HANDLE) {
                continue;
            }
            const uint32_t textureId = static_cast<uint32_t>(id);
            if (source_->consumeDirty(textureId)) {
                tex.pendingUpload = true;
            }
            if (tex.pendingUpload) {
                const uint32_t w = source_->width(textureId);
                const uint32_t h = source_->height(textureId);
                const VkDeviceSize size =
                    static_cast<VkDeviceSize>(source_->mipChainBytes(textureId));
                if (size > 0) {
                    copies.push_back({&tex, textureId, size, w, h, tex.mipLevels, false});
                    total += size;
                }
            }
        }
    }
    if (copies.empty()) {
        return;
    }

    if (!ensureTextureUploadCapacity(frameSlot, total)) {
        EVK_LOGE("unable to allocate {} bytes for texture upload",
                 static_cast<uint64_t>(total));
        return;
    }

    // CPU 侧写入中转缓冲（HOST_VISIBLE + COHERENT，写完即对 GPU 可见）。
    VkDeviceSize offset = 0;
    void* mapped = nullptr;
    if (vkMapMemory(device, textureUploadMemorys_[frameSlot], 0, total, 0,
                    &mapped) != VK_SUCCESS) {
        EVK_LOGE("texture upload vkMapMemory failed");
        return;
    }
    for (const PendingCopy& copy : copies) {
        uint8_t* destination = static_cast<uint8_t*>(mapped) + offset;
        if (copy.white) {
            std::memset(destination, 0xFF, 4);
        } else if (!source_->copyMipChain(copy.textureId, destination,
                                          static_cast<size_t>(copy.size))) {
            EVK_LOGE("texture {} mip chain conversion failed", copy.textureId);
            vkUnmapMemory(device, textureUploadMemorys_[frameSlot]);
            return;
        }
        offset += copy.size;
    }
    vkUnmapMemory(device, textureUploadMemorys_[frameSlot]);

    // 逐个纹理：屏障到 TRANSFER_DST（整条 mip 链）→ 逐级拷贝 →
    // 屏障到 SHADER_READ_ONLY。各级一次布局转换即可，无需逐层折腾。
    offset = 0;
    for (const PendingCopy& copy : copies) {
        VkImageMemoryBarrier toDst{};
        toDst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toDst.image = copy.tex->image;
        toDst.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        toDst.subresourceRange.levelCount = copy.mipLevels;
        toDst.subresourceRange.layerCount = 1;
        // 首次上传从 UNDEFINED 开始；脏纹理重传时必须声明它当前的采样布局。
        toDst.oldLayout = copy.tex->uploaded
                              ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                              : VK_IMAGE_LAYOUT_UNDEFINED;
        toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toDst.srcAccessMask = copy.tex->uploaded ? VK_ACCESS_SHADER_READ_BIT : 0;
        toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        const VkPipelineStageFlags sourceStage =
            copy.tex->uploaded ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                               : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        vkCmdPipelineBarrier(cmd, sourceStage,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &toDst);

        // mip 链在缓冲中级间紧密排列：逐级一个 copy region，
        // region 的 mipLevel 指层、extent 逐级缩半、bufferOffset 顺序前进。
        VkDeviceSize levelOffset = offset;
        uint32_t levelW = copy.width;
        uint32_t levelH = copy.height;
        for (uint32_t level = 0; level < copy.mipLevels; ++level) {
            VkBufferImageCopy region{};
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.mipLevel = level;
            region.imageSubresource.layerCount = 1;
            region.imageExtent = {levelW, levelH, 1};
            region.bufferOffset = levelOffset;
            // RGBA 紧密排列：行间距 = 宽度（纹素），无对齐填充。
            region.bufferRowLength = levelW;
            region.bufferImageHeight = levelH;
            vkCmdCopyBufferToImage(cmd, textureUploadBuffers_[frameSlot],
                                   copy.tex->image,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            levelOffset += static_cast<VkDeviceSize>(levelW) * levelH * 4;
            levelW = std::max(1u, levelW >> 1);
            levelH = std::max(1u, levelH >> 1);
        }

        VkImageMemoryBarrier toRead{};
        toRead.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toRead.image = copy.tex->image;
        toRead.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        toRead.subresourceRange.levelCount = copy.mipLevels;
        toRead.subresourceRange.layerCount = 1;
        toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
                             0, nullptr, 1, &toRead);

        offset += copy.size;
        copy.tex->uploaded = true;
        copy.tex->pendingUpload = false;
    }
}

VkDescriptorSet TextureCache::descriptorFor(uint32_t textureId) const {
    if (textureId == 0) {
        return whiteTexture_.set;
    }
    if (textureId < storeTextures_.size() &&
        storeTextures_[textureId].set != VK_NULL_HANDLE) {
        return storeTextures_[textureId].set;
    }
    return whiteTexture_.set;
}

void TextureCache::shutdown() {
    const VkDevice device = context_.device();

    auto destroyTexture = [device](TextureObj& tex) {
        // descriptor set 归池所有，销毁池即释放，无需单独删。
        if (tex.view != VK_NULL_HANDLE) vkDestroyImageView(device, tex.view, nullptr);
        if (tex.image != VK_NULL_HANDLE) vkDestroyImage(device, tex.image, nullptr);
        if (tex.memory != VK_NULL_HANDLE) vkFreeMemory(device, tex.memory, nullptr);
        tex = TextureObj{};
    };
    destroyTexture(whiteTexture_);
    for (auto& tex : storeTextures_) {
        destroyTexture(tex);
    }
    storeTextures_.clear();
    for (size_t i = 0; i < textureUploadBuffers_.size(); ++i) {
        if (textureUploadBuffers_[i] != VK_NULL_HANDLE) {
            vkDestroyBuffer(device, textureUploadBuffers_[i], nullptr);
        }
        if (textureUploadMemorys_[i] != VK_NULL_HANDLE) {
            vkFreeMemory(device, textureUploadMemorys_[i], nullptr);
        }
    }
    textureUploadBuffers_.clear();
    textureUploadMemorys_.clear();
    textureUploadCapacities_.clear();
    if (descriptorPool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device, descriptorPool_, nullptr);
        descriptorPool_ = VK_NULL_HANDLE;
    }
    if (descriptorSetLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device, descriptorSetLayout_, nullptr);
        descriptorSetLayout_ = VK_NULL_HANDLE;
    }
    if (sampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(device, sampler_, nullptr);
        sampler_ = VK_NULL_HANDLE;
    }
}

} // namespace evk::gpu
