#pragma once

/**
 * @file vulkan_context.h
 * @brief Vulkan 地基：instance、surface、物理/逻辑设备与队列的创建与归属。
 */
#include "evk/render_platform.h"
#include <vulkan/vulkan.h>
#include <cstddef>
#include <cstdint>

namespace evk::gpu {

/// CPU 最多领先 GPU 的帧数：每帧一套命令缓冲 / 同步原语 / 顶点与纹理中转缓冲。
constexpr uint32_t kMaxFramesInFlight = 2;

/**
 * @brief Vulkan 地基模块：instance、debug messenger、surface、物理/逻辑设备、队列。
 *
 * 只负责"把设备跑起来"并提供只读访问器；swapchain、管线、纹理等上层
 * 设施各自归属 gpu/ 下的对应模块。
 */
class VulkanContext {
public:
    /// 构造只保存平台抽象层指针；所有 Vulkan 对象推迟到 initialize() 里创建。
    explicit VulkanContext(IPlatform* platform);
    ~VulkanContext();

    /**
     * @brief 按依赖顺序创建 instance → surface → 物理设备 → 逻辑设备。
     * @return true 表示成功；任一步失败即中止
     */
    bool initialize();

    /**
     * @brief 释放 device / surface / debug messenger / instance；幂等。
     */
    void shutdown();

    IPlatform* platform() const { return platform_; }
    VkInstance instance() const { return instance_; }
    VkSurfaceKHR surface() const { return surface_; }
    VkPhysicalDevice physicalDevice() const { return physicalDevice_; }
    VkDevice device() const { return device_; }
    VkQueue graphicsQueue() const { return graphicsQueue_; }
    VkQueue presentQueue() const { return presentQueue_; }

    /**
     * @brief 在物理设备的内存类型里找一个满足这些标志位的类型。
     * @return 命中的内存类型下标；找不到时兜底返回 0（教学代码的简化）
     */
    uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const;

    /**
     * @brief 把预编译 SPIR-V 字节码包装成 shader module。
     * @return 失败返回 VK_NULL_HANDLE
     */
    VkShaderModule createShaderModule(const uint32_t* code, size_t codeSize) const;

private:
    bool createInstance();
    bool createSurface();
    bool pickPhysicalDevice();
    bool createLogicalDevice();

    IPlatform* platform_ = nullptr;

    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debugMessenger_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;

    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;

    VkQueue graphicsQueue_ = VK_NULL_HANDLE;
    VkQueue presentQueue_ = VK_NULL_HANDLE;
};

} // namespace evk::gpu
