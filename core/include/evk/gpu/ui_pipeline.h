#pragma once

/**
 * @file ui_pipeline.h
 * @brief 唯一 UI 图形管线：pipeline layout 与 graphics pipeline 的创建与归属。
 */
#include <vulkan/vulkan.h>

namespace evk::gpu {

class VulkanContext;

/**
 * @brief UI 渲染的唯一图形管线（"纹理 × 顶点色"，viewport/scissor 为动态状态）。
 *
 * 预编译 SPIR-V 内嵌头 evk/assets/ui_shaders.h 的引用收敛在本模块；
 * 管线配置跟随 swapchain 的格式/尺寸与采样数，swapchain 重建时一并重建。
 */
class UiPipeline {
public:
    explicit UiPipeline(VulkanContext& context);

    /**
     * @brief 创建 pipeline layout 与 graphics pipeline。
     * @param renderPass 兼容的 render pass（Swapchain 提供）
     * @param extent swapchain 当前尺寸（viewport/scissor 占位；实际值录制时动态设置）
     * @param msaaSamples 光栅化采样数，必须与 renderPass 附件声明一致
     * @param descriptorSetLayout set 0 组合图像采样布局（TextureCache 提供）
     * @return true 表示成功
     */
    bool create(VkRenderPass renderPass, VkExtent2D extent,
                VkSampleCountFlagBits msaaSamples,
                VkDescriptorSetLayout descriptorSetLayout);

    /**
     * @brief 销毁管线与布局；句柄复位为空，重复调用安全。
     */
    void destroy();

    VkPipeline pipeline() const { return graphicsPipeline_; }
    VkPipelineLayout layout() const { return pipelineLayout_; }

private:
    VulkanContext& context_;

    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline graphicsPipeline_ = VK_NULL_HANDLE;
};

} // namespace evk::gpu
