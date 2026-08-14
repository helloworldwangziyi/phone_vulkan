// ============================================================================
// iOS 平台适配：CAMetalLayer → Vulkan surface（经 MoltenVK）。
//
// MoltenVK 是 Vulkan 在 Metal 之上的可移植性实现，因此与原生 Vulkan 驱动
// 相比有两处额外要求，都通过 IPlatform 的可选钩子声明给 renderer：
//   - 实例：VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR 标志 +
//     portability_enumeration / get_physical_device_properties2 扩展，
//     否则枚举不到可移植性物理设备；
//   - 设备：VK_KHR_portability_subset 扩展。
// ============================================================================

// 必须定义在包含任何 vulkan 头文件之前：
// - VK_USE_PLATFORM_METAL_EXT 打开 VK_EXT_metal_surface 的类型与函数声明
//   （vkCreateMetalSurfaceEXT / VkMetalSurfaceCreateInfoEXT）；
// - VK_ENABLE_BETA_EXTENSIONS 打开 vulkan_beta.h（VK_KHR_portability_subset
//   是 provisional 扩展，宏定义在里面）。
#define VK_USE_PLATFORM_METAL_EXT
#define VK_ENABLE_BETA_EXTENSIONS

#include "evk/platform.h"

#import <QuartzCore/CAMetalLayer.h>

#include <cstring>

#include "evk/log.h"

namespace evk {

class IosPlatform : public IPlatform {
public:
    // layer 由壳层（ViewController）持有并保证比本对象活得久。
    explicit IosPlatform(CAMetalLayer* layer) : layer_(layer) {}

    const char* getSurfaceExtensionName() const override {
        return VK_EXT_METAL_SURFACE_EXTENSION_NAME;
    }

    bool createVulkanSurface(VkInstance instance, VkSurfaceKHR* surface) override {
        if (!layer_) {
            EVK_LOGE("iOS metal layer is null");
            return false;
        }
        VkMetalSurfaceCreateInfoEXT createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT;
        createInfo.pLayer = layer_;
        if (vkCreateMetalSurfaceEXT(instance, &createInfo, nullptr, surface) !=
            VK_SUCCESS) {
            EVK_LOGE("vkCreateMetalSurfaceEXT failed");
            return false;
        }
        return true;
    }

    void getSurfaceSize(uint32_t* width, uint32_t* height) override {
        // drawableSize 是像素单位（壳层在布局时已乘 contentsScale）。
        const CGSize size = layer_ ? layer_.drawableSize : CGSizeZero;
        *width = static_cast<uint32_t>(size.width);
        *height = static_cast<uint32_t>(size.height);
    }

    uint32_t getInstanceCreateFlags() const override {
        return VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }

    void getRequiredInstanceExtensions(std::vector<const char*>& out) const override {
        // portability_subset 在设备创建时校验此扩展已启用。
        out.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
        // VK_KHR_portability_enumeration 属于「经过官方 Vulkan loader」的场景：
        // 直链 MoltenVK（无 loader）时它不在可用列表里，强行启用会让
        // vkCreateInstance 报 VK_ERROR_EXTENSION_NOT_PRESENT。因此先探测再启用。
        uint32_t count = 0;
        if (vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr) !=
            VK_SUCCESS) {
            return;
        }
        std::vector<VkExtensionProperties> props(count);
        vkEnumerateInstanceExtensionProperties(nullptr, &count, props.data());
        for (const auto& p : props) {
            if (std::strcmp(p.extensionName,
                            VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME) == 0) {
                out.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
                return;
            }
        }
    }

    void getRequiredDeviceExtensions(std::vector<const char*>& out) const override {
        out.push_back(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME);
    }

private:
    CAMetalLayer* layer_ = nullptr;
};

IPlatform* createIosPlatform(CAMetalLayer* layer) {
    return new IosPlatform(layer);
}

void destroyIosPlatform(IPlatform* platform) {
    delete platform;
}

} // namespace evk

extern "C" {

// C 链接工厂：桥接层只需声明原型，无需知道 IosPlatform 类布局。
evk::IPlatform* evkCreateIosPlatform(const void* layer) {
    return evk::createIosPlatform((__bridge CAMetalLayer*)layer);
}

void evkDestroyIosPlatform(evk::IPlatform* platform) {
    evk::destroyIosPlatform(platform);
}

} // extern "C"
