#pragma once

/**
 * @file vulkan_renderer.h
 * @brief 极简自包含 Vulkan 渲染器：渲染 ui::Canvas 收集的 2D UI 几何。
 */
// glm 深度映射约定（Vulkan NDC z∈[0,1]）必须在任何 glm 头之前定义；
// 本头经成员函数签名把 glm 暴露给包含者，宏因此收敛在此处。
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include "evk/render_platform.h"
#include "evk/gpu/vulkan_context.h"
#include "evk/gpu/swapchain.h"
#include "evk/gpu/ui_pipeline.h"
#include "evk/gpu/texture_cache.h"
#include "evk/gpu/offscreen_effects.h"
#include "evk/ui/paint_canvas.h"
#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <cstddef>
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
    /// 离屏效果设施创建（视觉尺寸 + swapchain 格式/采样数）；失败仅降级。
    bool createOffscreenEffects();

    /**
     * @brief swapchain 重建编排：管线与 swapchain 资源同批销毁后按原序重建。
     */
    void recreateSwapchain();
    void recordCommandBuffer(VkCommandBuffer cmd, uint32_t imageIndex, const ui::Canvas& canvas);

    /**
     * @brief 录制 [begin, end) 范围的普通/SDF 批次（跳过空批与 kBlur 标记批）。
     * @param mvp 像素 → NDC 投影（直通路径含旋转补偿；离屏场景为纯正交）
     * @param targetExtent 渲染目标尺寸（直通 = buffer；离屏 = 视觉尺寸）
     * @param transform 呈现旋转变换（离屏路径传 IDENTITY：场景按视觉方向绘制）
     */
    void recordBatchRange(VkCommandBuffer cmd, const ui::Canvas& canvas,
                          size_t begin, size_t end, const glm::mat4& mvp,
                          VkExtent2D targetExtent,
                          VkSurfaceTransformFlagBitsKHR transform);

    /**
     * @brief 录制一次背景模糊的离屏部分：场景 resolve → H/V 两趟高斯
     *        （1/4 降采样 ping-pong）。调用前后都在场景 render pass 之外。
     * @return true 表示模糊已执行（调用方应在续画段合成 pingB）；
     *         false 表示区域出屏跳过（无需合成，场景布局未被触碰）
     */
    bool recordBlurBackdrop(VkCommandBuffer cmd, const ui::EffectParams& params);

    /// 图像布局屏障（离屏段间同步用）。
    void barrierImage(VkCommandBuffer cmd, VkImage image,
                      VkImageLayout oldLayout, VkImageLayout newLayout,
                      VkAccessFlags srcAccess, VkAccessFlags dstAccess,
                      VkPipelineStageFlags srcStage,
                      VkPipelineStageFlags dstStage);

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
    gpu::OffscreenEffects offscreenEffects_; ///< 背景模糊离屏设施（随 swapchain 重建）

    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> commandBuffers_;

    std::vector<VkSemaphore> imageAvailableSemaphores_;
    std::vector<VkSemaphore> renderFinishedSemaphores_;
    std::vector<VkFence> inFlightFences_;

    uint32_t currentFrame_ = 0;
    /// 离屏图像已被任一帧使用过：离屏路径帧首据此决定是否需要跨帧屏障
    /// （首帧无先前采样者，无需屏障）。
    bool offscreenUsedOnce_ = false;

    std::vector<VkBuffer> vertexBuffers_; ///< 每个 in-flight 帧独占，避免 CPU 覆盖在途顶点
    std::vector<VkDeviceMemory> vertexBufferMemorys_;
    std::vector<uint32_t> vertexBufferCapacities_;
    static constexpr uint32_t kInitialVertexCapacity = 8192;
};

} // namespace evk
