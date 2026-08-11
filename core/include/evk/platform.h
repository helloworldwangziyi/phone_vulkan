#pragma once

#include <vulkan/vulkan.h>
#include <cstdint>

namespace evk {

// Platform abstraction. The renderer uses this to create a VkSurfaceKHR
// and query the drawing area size. Logging goes through evk/log.h (EVK_LOG*
// macros) directly, not through this interface.
class IPlatform {
public:
    virtual ~IPlatform() = default;

    // Create a Vulkan surface bound to the native window.
    virtual bool createVulkanSurface(VkInstance instance, VkSurfaceKHR* surface) = 0;

    // Return the current size of the surface in pixels.
    virtual void getSurfaceSize(uint32_t* width, uint32_t* height) = 0;
};

} // namespace evk
