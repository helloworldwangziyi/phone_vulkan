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
 * @brief Platform abstraction. The renderer uses this to create a VkSurfaceKHR
 * and query the drawing area size. Logging goes through evk/log.h (EVK_LOG*
 * macros) directly, not through this interface.
 */
class IPlatform {
public:
    virtual ~IPlatform() = default;

    /**
     * @brief Return the name of the platform surface extension that must be enabled
     * when creating the VkInstance (e.g. VK_KHR_ANDROID_SURFACE_EXTENSION_NAME
     * on Android; iOS/HarmonyOS provide their own).
     * @return The surface extension name to enable.
     */
    virtual const char* getSurfaceExtensionName() const = 0;

    /**
     * @brief Create a Vulkan surface bound to the native window.
     * @param instance The instance the surface is created on.
     * @param surface Out: the created surface handle.
     * @return true on success.
     */
    virtual bool createVulkanSurface(VkInstance instance, VkSurfaceKHR* surface) = 0;

    /**
     * @brief Return the current size of the surface in pixels.
     * @param width Out: surface width in pixels.
     * @param height Out: surface height in pixels.
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
