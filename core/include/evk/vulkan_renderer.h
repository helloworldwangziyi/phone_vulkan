#pragma once

/**
 * @file vulkan_renderer.h
 * @brief 极简自包含 Vulkan 渲染器：渲染 ui::Canvas 收集的 2D UI 几何。
 */
#include "evk/render_platform.h"
#include "evk/gpu/vulkan_context.h"
#include "evk/gpu/swapchain.h"
#include "evk/gpu/ui_pipeline.h"
#include "evk/gpu/texture_cache.h"
#include "evk/ui/paint_canvas.h"
#include <vulkan/vulkan.h>
#include <cstdint>
#include <vector>

namespace evk {

/**
 * @brief 极简自包含的 Vulkan 渲染器。
 *
 * GPU 设施拆给 gpu/ 子模块：instance/设备归 gpu::VulkanContext，
 * swapchain/MSAA/render pass 归 gpu::Swapchain，管线归 gpu::UiPipeline，
 * 纹理归 gpu::TextureCache；本类只做帧编排（acquire → 录制 → 提交 →
 * present）并持有命令池、动态顶点缓冲与同步原语。
 * 原生 surface 由 IPlatform 提供。
 */
class Renderer {
public:
    /**
     * @param platform 平台抽象层（surface 创建与尺寸查询）
     * @param textureSource UI 纹理数据源（注入给 gpu::TextureCache）
     */
    Renderer(IPlatform* platform, gpu::ITextureSource* textureSource);
    ~Renderer();

    /**
     * @brief 初始化整套 Vulkan 管线。
     * @return true 表示成功；任一步失败即中止启动
     */
    bool initialize();

    /**
     * @brief 释放全部 Vulkan 资源。
     */
    void shutdown();

    /**
     * @brief 把 Canvas 收集的本帧几何绘制成一帧。
     * @param canvas 本帧收集的 UI 几何
     * @return true 表示绘制成功
     */
    bool render(const ui::Canvas& canvas);

    /**
     * @brief 通知渲染器 surface 尺寸已变化。
     * @param width 新 surface 宽度（像素）
     * @param height 新 surface 高度（像素）
     */
    void setSize(uint32_t width, uint32_t height);

    /**
     * @brief 标记下次 render 调用时强制重建 swapchain。
     */
    void requestSwapchainRebuild();

private:
    bool createCommandPool();
    bool createCommandBuffers();
    bool createSyncObjects();
    bool createVertexBuffers();
    bool createVertexBuffer(uint32_t frameSlot, uint32_t capacity);
    void destroyVertexBuffers();

    /**
     * @brief swapchain 重建编排：管线与 swapchain 资源同批销毁后按原序重建。
     */
    void recreateSwapchain();
    void recordCommandBuffer(VkCommandBuffer cmd, uint32_t imageIndex, const ui::Canvas& canvas);

    /**
     * @brief 把 UI 顶点上传到当前帧独立的动态顶点缓冲。
     * @param data 顶点数组首地址
     * @param count 顶点个数；缓冲不足时按需扩容
     * @param frameSlot 当前 in-flight 帧槽
     */
    bool uploadVertices(const ui::UiVertex* data, uint32_t count,
                        uint32_t frameSlot);

    // ---- GPU 子模块（声明顺序即构造顺序：context 最先，其余引用它） ----
    gpu::VulkanContext context_;
    gpu::TextureCache textureCache_;
    gpu::Swapchain swapchain_;
    gpu::UiPipeline uiPipeline_;

    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> commandBuffers_;

    std::vector<VkSemaphore> imageAvailableSemaphores_;
    std::vector<VkSemaphore> renderFinishedSemaphores_;
    std::vector<VkFence> inFlightFences_;

    uint32_t currentFrame_ = 0;

    std::vector<VkBuffer> vertexBuffers_; ///< 每个 in-flight 帧独占，避免 CPU 覆盖在途顶点
    std::vector<VkDeviceMemory> vertexBufferMemorys_;
    std::vector<uint32_t> vertexBufferCapacities_;
    static constexpr uint32_t kInitialVertexCapacity = 8192;
};

} // namespace evk
