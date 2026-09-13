#pragma once

/**
 * @file ui_pipeline.h
 * @brief UI 图形管线组：主管线（纹理 × 顶点色）+ SDF 管线（阴影/圆角裁剪），
 *        共享同一 pipeline layout。
 */
#include <vulkan/vulkan.h>

namespace evk::gpu {

class VulkanContext;

/// 管线种类：主管线走全部普通批次；kSdf 走效果批次（阴影/圆角遮罩）。
enum class PipelineKind { kMain, kSdf };

/**
 * @brief UI 渲染的图形管线组（viewport/scissor 为动态状态）。
 *
 * 两条管线共享同一 pipeline layout：set 0 = 组合图像采样（批次纹理），
 * push constant = 96B（mat4 mvp + effectRect + effectParams，
 * VERTEX|FRAGMENT 可见；主管线只用前 64B mvp）。
 * 预编译 SPIR-V 内嵌头（evk/assets/ui_shaders.h / ui_sdf_shaders.h）的引用
 * 收敛在本模块；管线配置跟随 swapchain 的格式/尺寸与采样数，
 * swapchain 重建时一并重建。
 */
class UiPipeline {
public:
    explicit UiPipeline(VulkanContext& context);

    /**
     * @brief 创建共享 pipeline layout 与两条 graphics pipeline。
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
     * @brief 销毁全部管线与布局；句柄复位为空，重复调用安全。
     */
    void destroy();

    VkPipeline pipeline(PipelineKind kind) const {
        return kind == PipelineKind::kSdf ? sdfPipeline_ : mainPipeline_;
    }
    VkPipelineLayout layout() const { return pipelineLayout_; }

private:
    VulkanContext& context_;

    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline mainPipeline_ = VK_NULL_HANDLE;
    VkPipeline sdfPipeline_ = VK_NULL_HANDLE;
};

} // namespace evk::gpu
