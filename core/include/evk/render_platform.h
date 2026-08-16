#pragma once

/**
 * @file render_platform.h
 * @brief 平台抽象层：渲染器经它创建 VkSurfaceKHR、查询绘制区尺寸。
 */
#include <vulkan/vulkan.h>
#include <cstdint>
#include <vector>

namespace evk {

/**
 * @brief 平台抽象接口。渲染器经它创建 VkSurfaceKHR、查询绘制区尺寸；
 * 日志不经过本接口，统一走 evk/log.h（EVK_LOG* 宏）。
 */
class IPlatform {
public:
    virtual ~IPlatform() = default;

    /**
     * @brief 返回创建 VkInstance 时必须启用的平台 surface 扩展名
     * （Android 为 VK_KHR_ANDROID_SURFACE_EXTENSION_NAME，
     * iOS/鸿蒙各有自己的扩展）。
     * @return 需要启用的 surface 扩展名
     */
    virtual const char* getSurfaceExtensionName() const = 0;

    /**
     * @brief 创建绑定到原生窗口的 Vulkan surface。
     * @param instance 创建 surface 所在的 VkInstance
     * @param surface 输出：创建出的 surface 句柄
     * @return true 表示创建成功
     */
    virtual bool createVulkanSurface(VkInstance instance, VkSurfaceKHR* surface) = 0;

    /**
     * @brief 返回 surface 当前的像素尺寸。
     * @param width 输出：surface 宽度（像素）
     * @param height 输出：surface 高度（像素）
     */
    virtual void getSurfaceSize(uint32_t* width, uint32_t* height) = 0;

    // ---- 可选钩子（默认空实现，非 MoltenVK 平台不用管） ----

    /**
     * @brief 额外的 VkInstanceCreateFlagBits（MoltenVK 需要 ENUMERATE_PORTABILITY_BIT）。
     * @return 追加到 VkInstanceCreateInfo::flags 的标志位；默认 0
     */
    virtual uint32_t getInstanceCreateFlags() const { return 0; }

    /**
     * @brief 平台额外要求的实例扩展（MoltenVK：portability_enumeration +
     * get_physical_device_properties2）。
     * @param out 输出：扩展名追加到该列表
     */
    virtual void getRequiredInstanceExtensions(
        std::vector<const char*>& /*out*/) const {}

    /**
     * @brief 平台额外要求的设备扩展（MoltenVK：portability_subset）。
     * @param out 输出：扩展名追加到该列表
     */
    virtual void getRequiredDeviceExtensions(
        std::vector<const char*>& /*out*/) const {}
};

} // namespace evk
