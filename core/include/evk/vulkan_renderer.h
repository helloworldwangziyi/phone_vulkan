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
    /**
     * @brief 查设备 MSAA 采样数上限，取 4→2→1 中最先被支持的一档。
     *
     * 颜色附件的可用采样数是设备属性（limits.framebufferColorSampleCounts），
     * 只依赖物理设备，initialize 时查一次即可，resize 不必重查。
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
    bool createGraphicsPipeline();
    bool createFramebuffers();
    bool createCommandPool();
    bool createCommandBuffers();
    bool createSyncObjects();
    bool createVertexBuffers();
    bool createVertexBuffer(uint32_t frameSlot, uint32_t capacity);
    void destroyVertexBuffers();
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
        uint32_t mipLevels = 1;  ///< mip 链层数（业务位图 >1，白纹理/atlas 页为 1）
        bool uploaded = false;     ///< true = 当前布局为 SHADER_READ_ONLY_OPTIMAL
        bool pendingUpload = true; ///< 新建/脏页：待首个命令缓冲里上传像素
    };

    bool createTextureResources();
    /**
     * @param mipLevels mip 链层数：>1 时上传阶段逐层写入（缩小采样抗锯齿）；
     *                  1 表示无 mip（白纹理、字形 atlas 页）
     */
    bool createSampledTexture(TextureObj& tex, uint32_t width, uint32_t height,
                              uint32_t mipLevels);
    bool ensureTextureUploadCapacity(uint32_t frameSlot, VkDeviceSize required);
    void destroyTextureResources();
    void ensureStoreTextures();
    void uploadPendingTextures(VkCommandBuffer cmd, uint32_t frameSlot);
    VkDescriptorSet descriptorFor(uint32_t textureId) const;

    void cleanupSwapchain();
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

    // ---- MSAA 多重采样设施：几何边缘抗锯齿 ----
    /// 实际启用的采样数（pickMsaaSampleCount 选出；1 = 未启用 MSAA）。
    VkSampleCountFlagBits msaaSamples_ = VK_SAMPLE_COUNT_1_BIT;
    VkImage msaaColorImage_ = VK_NULL_HANDLE;       ///< 多采样颜色图（管线真正的绘制目标）
    VkDeviceMemory msaaColorImageMemory_ = VK_NULL_HANDLE;
    VkImageView msaaColorImageView_ = VK_NULL_HANDLE; ///< 挂进 framebuffer 的视图

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

    std::vector<VkBuffer> vertexBuffers_; ///< 每个 in-flight 帧独占，避免 CPU 覆盖在途顶点
    std::vector<VkDeviceMemory> vertexBufferMemorys_;
    std::vector<uint32_t> vertexBufferCapacities_;
    static constexpr uint32_t kInitialVertexCapacity = 8192;

    // ---- 纹理设施：字形 atlas 采样 + 1x1 白纹理占位 ----

    VkSampler sampler_ = VK_NULL_HANDLE;            ///< 共享采样器（线性 + 边缘钳制）
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptorSetLayout_ = VK_NULL_HANDLE; ///< binding0 = 组合图像采样
    TextureObj whiteTexture_;                        ///< textureId 0：纯色批次的白纹理
    /// TextureStore 第 n 号纹理的 GPU 对象；下标即 n，[0] 占位（白纹理）。
    std::vector<TextureObj> storeTextures_;
    std::vector<VkBuffer> textureUploadBuffers_; ///< 每 in-flight 帧一块纹理上传中转
    std::vector<VkDeviceMemory> textureUploadMemorys_;
    std::vector<VkDeviceSize> textureUploadCapacities_;
};

} // namespace evk
