#pragma once

#include <vulkan/vulkan.h>
#include <cstdint>
#include <vector>

namespace evk {

// Platform abstraction. The renderer uses this to create a VkSurfaceKHR
// and query the drawing area size. Logging goes through evk/log.h (EVK_LOG*
// macros) directly, not through this interface.
class IPlatform {
public:
    virtual ~IPlatform() = default;

    // Return the name of the platform surface extension that must be enabled
    // when creating the VkInstance (e.g. VK_KHR_ANDROID_SURFACE_EXTENSION_NAME
    // on Android; iOS/HarmonyOS provide their own).
    virtual const char* getSurfaceExtensionName() const = 0;

    // Create a Vulkan surface bound to the native window.
    virtual bool createVulkanSurface(VkInstance instance, VkSurfaceKHR* surface) = 0;

    // Return the current size of the surface in pixels.
    virtual void getSurfaceSize(uint32_t* width, uint32_t* height) = 0;

    // ---- 可选钩子（默认空实现，非 MoltenVK 平台不用管） ----

    // 额外的 VkInstanceCreateFlagBits（MoltenVK 需要 ENUMERATE_PORTABILITY_BIT）。
    virtual uint32_t getInstanceCreateFlags() const { return 0; }

    // 平台额外要求的实例扩展（MoltenVK：portability_enumeration +
    // get_physical_device_properties2）。
    virtual void getRequiredInstanceExtensions(
        std::vector<const char*>& /*out*/) const {}

    // 平台额外要求的设备扩展（MoltenVK：portability_subset）。
    virtual void getRequiredDeviceExtensions(
        std::vector<const char*>& /*out*/) const {}
};

} // namespace evk
