#pragma once

/**
 * @file offscreen_effects.h
 * @brief 离屏效果设施：背景模糊（BackdropFilter）所需的场景纹理、
 *        高斯 ping-pong 目标、render pass 与小管线组。
 *
 * 生命周期跟随 swapchain（尺寸/采样数/格式依赖它）：由 Renderer 在
 * initialize / recreateSwapchain / shutdown 三处编排。与 TextureCache
 * 分离——纹理缓存是设备级常驻设施，本模块随尺寸重建。
 *
 * 结构（对照 Flutter BackdropFilter 的 saveLayer → 模糊 → 合成）：
 *   场景段：内容画进 sceneMsaa（MSAA，与主通道同采样数），段尾 resolve
 *     到 sceneResolve（可采样）；无 MSAA 时直接画进 sceneResolve；
 *   模糊：遇 kBlur 标记批 → sceneResolve 采样，H 通道写 pingA、V 通道
 *     写 pingB（经典 5 采样线性优化高斯核）；
 *   合成：pingB 经 sdfBlit 管线（圆角遮罩）画回场景，继续后续内容；
 *   末段：场景整体经 blit 管线全屏画到 swapchain（旋转补偿集中在此）。
 */
#include <vulkan/vulkan.h>

namespace evk::gpu {

class VulkanContext;

class OffscreenEffects {
public:
    explicit OffscreenEffects(VulkanContext& context);

    /**
     * @brief 按视觉尺寸/格式/采样数创建全部离屏资源。
     * @param visualExtent 视觉方向的尺寸（90/270° 旋转时与 swapchain 宽高互换）
     * @param format 颜色格式（与 swapchain 一致）
     * @param msaaSamples 场景多重采样数（与主通道一致；1 = 单采样直写 resolve 图）
     * @param textureSetLayout set 0 组合图像采样布局（TextureCache 提供）
     * @param sharedLayout 共享 pipeline layout（UiPipeline 提供，push 112B）
     * @return true 表示成功；失败时本模块处于未就绪态（调用方走降级路径）
     */
    bool create(VkExtent2D visualExtent, VkFormat format,
                VkSampleCountFlagBits msaaSamples,
                VkDescriptorSetLayout textureSetLayout,
                VkPipelineLayout sharedLayout);

    /**
     * @brief 销毁全部资源；句柄复位，重复调用安全。
     */
    void destroy();

    bool ready() const { return ready_; }
    VkExtent2D extent() const { return extent_; }
    /// 模糊 ping-pong 目标尺寸（视觉尺寸的 1/4 降采样——大 sigma 下小核
    /// 不再欠采样出残影，同时省带宽；合成时经线性过滤放大回去）。
    VkExtent2D pingExtent() const { return pingExtent_; }

    /**
     * @brief 创建三条无顶点输入小管线（模糊/圆角合成/末段 blit）。
     * 在 create() 成功后调用；主 render pass 与共享布局由 Swapchain/
     * UiPipeline 提供（swapchain 重建时随管线同批重建）。
     */
    bool createPipelines(VkRenderPass mainRenderPass,
                         VkPipelineLayout sharedLayout,
                         VkSampleCountFlagBits msaaSamples);

    /// 场景 render pass：clear 用于帧首段，load 用于模糊合成后的续画段。
    VkRenderPass scenePass(bool load) const {
        return load ? sceneLoadPass_ : sceneClearPass_;
    }
    VkFramebuffer sceneFramebuffer() const { return sceneFramebuffer_; }
    VkRenderPass blurPass() const { return blurPass_; }
    /// 模糊 ping-pong framebuffer：false = pingA（H 通道），true = pingB（V 通道）。
    VkFramebuffer blurFramebuffer(bool vertical) const {
        return vertical ? pingFramebuffer_[1] : pingFramebuffer_[0];
    }

    VkPipeline blurPipeline() const { return blurPipeline_; }
    VkPipeline sdfBlitPipeline() const { return sdfBlitPipeline_; }
    VkPipeline blitPipeline() const { return blitPipeline_; }

    /// 采样 descriptor：场景 resolve 图 / ping 图（绑定到 set 0）。
    VkDescriptorSet sceneSet() const { return sceneSet_; }
    VkDescriptorSet pingSet(bool vertical) const {
        return vertical ? pingSet_[1] : pingSet_[0];
    }

    /// 场景 resolve 图（渲染段间显式布局屏障用）。
    VkImage sceneImage() const { return sceneResolveImage_; }
    /// ping 图（模糊两趟之间的写→读屏障用）。
    VkImage pingImage(bool vertical) const {
        return pingImage_[vertical ? 1 : 0];
    }

private:
    bool createImage(VkExtent2D extent, VkFormat format,
                     VkSampleCountFlagBits samples, VkImageUsageFlags usage,
                     VkImage* image, VkDeviceMemory* memory,
                     VkImageView* view);
    bool createScenePasses(VkFormat format, VkSampleCountFlagBits msaaSamples);
    bool createBlurPass(VkFormat format);
    bool createFramebuffers();
    bool createDescriptors(VkDescriptorSetLayout textureSetLayout);

    VulkanContext& context_;

    VkExtent2D extent_{};
    VkExtent2D pingExtent_{}; ///< 模糊目标尺寸（extent_ 的 1/4）
    VkSampleCountFlagBits msaaSamples_ = VK_SAMPLE_COUNT_1_BIT;
    bool ready_ = false;

    // ---- 场景目标（MSAA 图 + resolve 图；单采样时只用 resolve 图） ----
    VkImage sceneMsaaImage_ = VK_NULL_HANDLE;
    VkDeviceMemory sceneMsaaMemory_ = VK_NULL_HANDLE;
    VkImageView sceneMsaaView_ = VK_NULL_HANDLE;
    VkImage sceneResolveImage_ = VK_NULL_HANDLE;
    VkDeviceMemory sceneResolveMemory_ = VK_NULL_HANDLE;
    VkImageView sceneResolveView_ = VK_NULL_HANDLE;

    // ---- 模糊 ping-pong（单采样、可采样） ----
    VkImage pingImage_[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkDeviceMemory pingMemory_[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkImageView pingView_[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};

    // ---- render pass 与 framebuffer ----
    VkRenderPass sceneClearPass_ = VK_NULL_HANDLE;
    VkRenderPass sceneLoadPass_ = VK_NULL_HANDLE;
    VkRenderPass blurPass_ = VK_NULL_HANDLE;
    VkFramebuffer sceneFramebuffer_ = VK_NULL_HANDLE;
    VkFramebuffer pingFramebuffer_[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};

    // ---- 采样设施（独立小池 + 独立采样器；set 布局复用 TextureCache 的） ----
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorSet sceneSet_ = VK_NULL_HANDLE;
    VkDescriptorSet pingSet_[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};

    // ---- 小管线组（无顶点输入，共享 UiPipeline 的 pipeline layout） ----
    VkPipeline blurPipeline_ = VK_NULL_HANDLE;
    VkPipeline sdfBlitPipeline_ = VK_NULL_HANDLE;
    VkPipeline blitPipeline_ = VK_NULL_HANDLE;
};

} // namespace evk::gpu
