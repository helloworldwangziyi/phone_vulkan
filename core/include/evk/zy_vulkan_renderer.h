#pragma once

/**
 * @file zy_vulkan_renderer.h
 * @brief 极简自包含 Vulkan 渲染器：渲染 ui::Canvas 收集的 2D UI 几何。
 */
#include "evk/zy_render_platform.h"
#include "evk/ui/zy_paint_canvas.h"
#include <vulkan/vulkan.h>
#include <cstdint>
#include <vector>

namespace evk {

/**
 * @brief A minimal, self-contained Vulkan renderer.
 *
 * It creates a swapchain, render pass, graphics pipeline and renders the
 * 2D UI geometry collected in an ui::Canvas. The actual native surface is
 * provided by IPlatform.
 */
class Renderer {
public:
    explicit Renderer(IPlatform* platform);
    ~Renderer();

    /**
     * @brief Initialize the entire Vulkan pipeline.
     * @return true on success; any step failure aborts startup.
     */
    bool initialize();

    /**
     * @brief Release all Vulkan resources.
     */
    void shutdown();

    /**
     * @brief Draw one frame from the canvas content. Returns true on success.
     * @param canvas The UI geometry collected for this frame.
     * @return true on success.
     */
    bool render(const ui::Canvas& canvas);

    /**
     * @brief Notify the renderer that the surface size has changed.
     * @param width New surface width in pixels.
     * @param height New surface height in pixels.
     */
    void setSize(uint32_t width, uint32_t height);

    /**
     * @brief Force the swapchain to be recreated on the next render call.
     */
    void requestSwapchainRebuild();

private:
    bool createInstance();
    bool createSurface();
    bool pickPhysicalDevice();
    bool createLogicalDevice();
    bool createSwapchain();
    bool createImageViews();
    bool createRenderPass();
    bool createGraphicsPipeline();
    bool createFramebuffers();
    bool createCommandPool();
    bool createCommandBuffers();
    bool createSyncObjects();
    bool createVertexBuffer();

    void cleanupSwapchain();
    void recreateSwapchain();
    void recordCommandBuffer(VkCommandBuffer cmd, uint32_t imageIndex, const ui::Canvas& canvas);

    /**
     * @brief 把 UI 顶点上传到动态顶点缓冲；超出容量时截断并告警。
     * @param data 顶点数组首地址
     * @param count 顶点个数；超出 kVertexCapacity 时截断
     */
    void uploadVertices(const ui::UiVertex* data, uint32_t count);

    uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties);
    VkShaderModule createShaderModule(const uint32_t* code, size_t codeSize);

    IPlatform* platform_ = nullptr;

    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debugMessenger_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;

    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;

    VkQueue graphicsQueue_ = VK_NULL_HANDLE;
    VkQueue presentQueue_ = VK_NULL_HANDLE;

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    std::vector<VkImage> swapchainImages_;
    std::vector<VkImageView> swapchainImageViews_;
    VkFormat swapchainImageFormat_ = VK_FORMAT_UNDEFINED;
    VkExtent2D swapchainExtent_{};

    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline graphicsPipeline_ = VK_NULL_HANDLE;

    std::vector<VkFramebuffer> swapchainFramebuffers_;

    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> commandBuffers_;

    static constexpr uint32_t kMaxFramesInFlight = 2;
    std::vector<VkSemaphore> imageAvailableSemaphores_;
    std::vector<VkSemaphore> renderFinishedSemaphores_;
    std::vector<VkFence> inFlightFences_;

    uint32_t currentFrame_ = 0;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    bool framebufferResized_ = false;
    /**
     * @brief createSwapchain 时记录的 surface 当前变换（折叠屏内屏等"自然方向为横"的屏，
     * 竖持时上报 ROTATE_90）：呈现时系统按它旋转帧缓冲，投影与裁剪要做补偿。
     */
    VkSurfaceTransformFlagBitsKHR surfaceTransform_ = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;

    VkBuffer vertexBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory vertexBufferMemory_ = VK_NULL_HANDLE;
    static constexpr uint32_t kVertexCapacity = 8192; ///< 动态顶点缓冲容量（UiVertex 个数）
};

} // namespace evk
