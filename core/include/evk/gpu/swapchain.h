#pragma once

/**
 * @file swapchain.h
 * @brief 交换链模块：swapchain、image view、MSAA 颜色图、render pass 与 framebuffer。
 */
#include <vulkan/vulkan.h>
#include <cstdint>
#include <vector>

namespace evk::gpu {

class VulkanContext;

/**
 * @brief 交换链及其下游资源（视图、MSAA、render pass、framebuffer）。
 *
 * 这些资源的格式与尺寸全部跟随 swapchain，窗口尺寸/旋转变化时由
 * Renderer 编排整体重建（管线归属 UiPipeline，同批销毁重建）。
 */
class Swapchain {
public:
    explicit Swapchain(VulkanContext& context);

    /**
     * @brief 创建 swapchain、image view、MSAA 颜色图与 render pass。
     *
     * MSAA 采样数是设备属性，每次创建时重查（resize 重建结果不变）；
     * MSAA 颜色图创建失败时内部降级为单采样，不判失败。
     * @return true 表示成功（swapchain / imageView / renderPass 必需）
     */
    bool create();

    /**
     * @brief 按 renderPass 与当前尺寸创建 framebuffer（每张 swapchain 图像一个）。
     * @return true 表示成功
     */
    bool createFramebuffers();

    /**
     * @brief 销毁全部跟随 swapchain 的资源（不动管线与设备级设施）。
     */
    void cleanup();

    /**
     * @brief 记录 surface 新尺寸并置重建标记。
     *
     * 只记尺寸并置标志位：本函数可能在渲染途中被平台线程调用，
     * 直接重建会撞上在飞的帧，真正的重建延迟到 render() 的安全点再做。
     * @param width 新 surface 宽度（像素）
     * @param height 新 surface 高度（像素）
     */
    void setSize(uint32_t width, uint32_t height);

    /**
     * @brief 平台知道 surface 变了、但还不知道准确尺寸时置重建标记；
     * 尺寸留给重建时重新查询。
     */
    void requestRebuild();

    /**
     * @brief 读取并清除重建标记（render 循环每帧开头调用）。
     * @return true 表示本帧开始前需要重建 swapchain
     */
    bool takeRebuildRequest();

    VkSwapchainKHR handle() const { return swapchain_; }
    VkRenderPass renderPass() const { return renderPass_; }
    VkFormat imageFormat() const { return swapchainImageFormat_; }
    VkExtent2D extent() const { return swapchainExtent_; }
    VkSampleCountFlagBits msaaSamples() const { return msaaSamples_; }
    VkSurfaceTransformFlagBitsKHR surfaceTransform() const { return surfaceTransform_; }
    VkFramebuffer framebuffer(uint32_t imageIndex) const {
        return swapchainFramebuffers_[imageIndex];
    }

private:
    bool createSwapchain();
    bool createImageViews();
    /**
     * @brief 查设备 MSAA 采样数上限，取 4→2→1 中最先被支持的一档。
     *
     * 颜色附件的可用采样数是设备属性（limits.framebufferColorSampleCounts），
     * 只依赖物理设备，每次创建时查一次即可，resize 重建结果不变。
     * 上限封在 4x：8x 显存带宽再翻倍，边缘质量收益却很小。
     */
    VkSampleCountFlagBits pickMsaaSampleCount() const;
    /**
     * @brief 创建 MSAA 多重采样颜色图（图像 + 显存 + 视图）。
     *
     * 尺寸/格式与 swapchain 图像一致，采样数为 msaaSamples_。管线画进这张
     * 多采样图，subpass 结束时由 resolve attachment 自动平均回 swapchain 图像。
     * msaaSamples_ 为 1 时是空操作；分配失败则降级为单采样（宁可锯齿也不黑屏）。
     */
    bool createColorResources();
    bool createRenderPass();

    VulkanContext& context_;

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    std::vector<VkImage> swapchainImages_;
    std::vector<VkImageView> swapchainImageViews_;
    VkFormat swapchainImageFormat_ = VK_FORMAT_UNDEFINED;
    VkExtent2D swapchainExtent_{};

    // ---- MSAA 多重采样设施：几何边缘抗锯齿 ----
    /// 实际启用的采样数（pickMsaaSampleCount 选出；1 = 未启用 MSAA）。
    VkSampleCountFlagBits msaaSamples_ = VK_SAMPLE_COUNT_1_BIT;
    VkImage msaaColorImage_ = VK_NULL_HANDLE;       ///< 多采样颜色图（管线真正的绘制目标）
    VkDeviceMemory msaaColorImageMemory_ = VK_NULL_HANDLE;
    VkImageView msaaColorImageView_ = VK_NULL_HANDLE; ///< 挂进 framebuffer 的视图

    VkRenderPass renderPass_ = VK_NULL_HANDLE;

    std::vector<VkFramebuffer> swapchainFramebuffers_;

    uint32_t width_ = 0;
    uint32_t height_ = 0;
    bool framebufferResized_ = false;
    /**
     * @brief createSwapchain 时记录的 surface 当前变换（折叠屏内屏等"自然方向为横"的屏，
     * 竖持时上报 ROTATE_90）：呈现时系统按它旋转帧缓冲，投影与裁剪要做补偿。
     */
    VkSurfaceTransformFlagBitsKHR surfaceTransform_ = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
};

} // namespace evk::gpu
