#pragma once

/**
 * @file vulkan_renderer.h
 * @brief 极简自包含 Vulkan 渲染器：渲染 ui::Canvas 收集的 2D UI 几何。
 */
#include "evk/render_platform.h"
#include "evk/ui/paint_canvas.h"
#include <vulkan/vulkan.h>
#include <cstdint>
#include <vector>

namespace evk {

/**
 * @brief 极简自包含的 Vulkan 渲染器。
 *
 * 内部创建 swapchain、render pass、图形管线，把 ui::Canvas 收集的 2D UI
 * 几何绘制到屏幕；原生 surface 由 IPlatform 提供。
 */
class Renderer {
public:
    explicit Renderer(IPlatform* platform);
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
    /**
     * @brief 一张采样纹理：图像 + 显存 + 视图 + descriptor set 的组合体。
     *
     * 白纹理与字形 atlas 页共用这个结构；descriptor set 在创建时一次性
     * 绑好 sampler/view，绘制批次只需按 textureId 换绑对应的 set。
     */
    struct TextureObj {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkDescriptorSet set = VK_NULL_HANDLE;
        bool pendingUpload = true; ///< 新建/脏页：待首个命令缓冲里上传像素
    };

    bool createTextureResources();
    bool createSampledTexture(TextureObj& tex, uint32_t width, uint32_t height,
                              const uint8_t* initialPixel);
    void destroyTextureResources();
    void ensureAtlasTextures();
    void uploadPendingTextures(VkCommandBuffer cmd, uint32_t frameSlot);
    VkDescriptorSet descriptorFor(uint32_t textureId) const;

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

    // ---- 纹理设施：字形 atlas 采样 + 1x1 白纹理占位 ----

    VkSampler sampler_ = VK_NULL_HANDLE;            ///< 共享采样器（线性 + 边缘钳制）
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptorSetLayout_ = VK_NULL_HANDLE; ///< binding0 = 组合图像采样
    TextureObj whiteTexture_;                        ///< textureId 0：纯色批次的白纹理
    std::vector<TextureObj> atlasTextures_;          ///< textureId n+1 = 字形 atlas 第 n 页
    std::vector<VkBuffer> atlasStagingBuffers_;      ///< 每 in-flight 帧一块 atlas 上传中转
    std::vector<VkDeviceMemory> atlasStagingMemorys_;
    uint8_t whitePixel_ = 255;                       ///< 白纹理的唯一像素（首次上传用）
};

} // namespace evk
