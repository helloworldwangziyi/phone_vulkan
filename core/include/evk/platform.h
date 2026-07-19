#pragma once

#include <vulkan/vulkan.h>
#include <cstdint>
#include <cstdarg>

namespace evk {

enum class LogLevel {
    Verbose,
    Debug,
    Info,
    Warn,
    Error
};

// Platform abstraction. The renderer uses this to create a VkSurfaceKHR,
// query the drawing area size, and emit log messages.
class IPlatform {
public:
    virtual ~IPlatform() = default;

    // Create a Vulkan surface bound to the native window.
    virtual bool createVulkanSurface(VkInstance instance, VkSurfaceKHR* surface) = 0;

    // Return the current size of the surface in pixels.
    virtual void getSurfaceSize(uint32_t* width, uint32_t* height) = 0;

    // Emit a log message.
    virtual void log(LogLevel level, const char* tag, const char* fmt, va_list args) = 0;
};

// Helper to emit a formatted log through the platform.
void logMessage(IPlatform* platform, LogLevel level, const char* tag, const char* fmt, ...);

} // namespace evk
