#pragma once

/**
 * @file texture_cache.h
 * @brief 纹理 GPU 缓存：白纹理与 ITextureSource 纹理的采样对象、descriptor 与上传中转。
 */
#include "evk/gpu/texture_source.h"
#include <vulkan/vulkan.h>
#include <cstdint>
#include <vector>

namespace evk::gpu {

class VulkanContext;

/**
 * @brief 纹理设施：共享采样器、descriptor 池与布局、1x1 白纹理、
 * 数据源纹理的 GPU 镜像，以及按帧分槽的上传中转缓冲。
 *
 * 白纹理让纯色批次与文字/图像批次共用同一条管线：shader 恒为
 * "顶点色 × 纹理 r 通道"，纯色批次采样到 1 即直出顶点色。
 * 纹理数据只经 ITextureSource 读取，本模块不依赖 UI 层。
 */
class TextureCache {
public:
    /**
     * @param context Vulkan 地基（设备与显存查找）
     * @param source UI 纹理数据源；可为 nullptr（仅剩白纹理可用）
     */
    TextureCache(VulkanContext& context, ITextureSource* source);

    /**
     * @brief 创建与 swapchain 无关的纹理设施：采样器、descriptor 布局与池、
     * 1x1 白纹理、每 in-flight 帧一块的上传中转缓冲（首个命令缓冲里才写满）。
     * @return true 表示成功
     */
    bool initialize();

    /**
     * @brief 释放纹理设施的全部 Vulkan 对象（白纹理、数据源纹理、采样器、
     * descriptor 池与布局、上传中转缓冲）；幂等。
     */
    void shutdown();

    /**
     * @brief 数据源长出新纹理时补建对应的采样纹理与 descriptor set。
     *
     * 在每帧录制命令前调用；新建纹理标记 pendingUpload，
     * 本帧命令缓冲里就会完成首次像素上传。
     */
    void ensureStoreTextures();

    /**
     * @brief 把待上传纹理（首次的白纹理 + 新建/脏的数据源纹理）写进命令缓冲。
     *
     * 必须在 vkCmdBeginRenderPass 之前调用：布局转换与拷贝都不是
     * render pass 内合法操作。上传用本帧槽位的中转缓冲，CPU 侧先行写入。
     * @param cmd 本帧命令缓冲
     * @param frameSlot 本帧槽位（选对应的中转缓冲）
     */
    void uploadPendingTextures(VkCommandBuffer cmd, uint32_t frameSlot);

    /**
     * @brief 批次纹理号 → descriptor set。0 = 白纹理；n = 数据源第 n 号纹理。
     *
     * 纹理创建失败时对应批次退化为白纹理（内容不可见但不崩）。
     */
    VkDescriptorSet descriptorFor(uint32_t textureId) const;

    /// set 0 的纹理布局（管线布局的输入，见 UiPipeline::create）。
    VkDescriptorSetLayout descriptorSetLayout() const { return descriptorSetLayout_; }

private:
    /**
     * @brief 一张采样纹理：图像 + 显存 + 视图 + descriptor set 的组合体。
     *
     * 白纹理与数据源纹理共用这个结构；descriptor set 在创建时一次性
     * 绑好 sampler/view，绘制批次只需按 textureId 换绑对应的 set。
     */
    struct TextureObj {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkDescriptorSet set = VK_NULL_HANDLE;
        uint32_t mipLevels = 1;  ///< mip 链层数（业务位图 >1，白纹理/atlas 页为 1）
        bool uploaded = false;     ///< true = 当前布局为 SHADER_READ_ONLY_OPTIMAL
        bool pendingUpload = true; ///< 新建/脏页：待首个命令缓冲里上传像素
    };

    /**
     * @param mipLevels mip 链层数：>1 时上传阶段逐层写入（缩小采样抗锯齿）；
     *                  1 表示无 mip（白纹理、字形 atlas 页）
     */
    bool createSampledTexture(TextureObj& tex, uint32_t width, uint32_t height,
                              uint32_t mipLevels);
    bool ensureTextureUploadCapacity(uint32_t frameSlot, VkDeviceSize required);

    VulkanContext& context_;
    ITextureSource* source_ = nullptr; ///< UI 纹理数据源（不持有所有权）

    VkSampler sampler_ = VK_NULL_HANDLE;            ///< 共享采样器（线性 + 边缘钳制）
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptorSetLayout_ = VK_NULL_HANDLE; ///< binding0 = 组合图像采样
    TextureObj whiteTexture_;                        ///< textureId 0：纯色批次的白纹理
    /// 数据源第 n 号纹理的 GPU 对象；下标即 n，[0] 占位（白纹理）。
    std::vector<TextureObj> storeTextures_;
    std::vector<VkBuffer> textureUploadBuffers_; ///< 每 in-flight 帧一块纹理上传中转
    std::vector<VkDeviceMemory> textureUploadMemorys_;
    std::vector<VkDeviceSize> textureUploadCapacities_;
};

} // namespace evk::gpu
