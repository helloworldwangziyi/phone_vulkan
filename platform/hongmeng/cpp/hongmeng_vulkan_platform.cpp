// ============================================================================
// 鸿蒙平台适配：XComponent 的 OHNativeWindow → Vulkan surface（VK_OHOS_surface）。
//
// 与 Android 壳（android_vulkan_platform.cpp）的对应关系：
//   ANativeWindow（Java Surface 解包）→ OHNativeWindow（XComponent 独立 surface）
//   vkCreateAndroidSurfaceKHR        → vkCreateSurfaceOHOS
//
// 两处值得注意的差别：
//   - 窗口句柄所有权：Android 由 ANativeWindow_fromSurface 取得、析构时 release；
//     鸿蒙的 OHNativeWindow 由 XComponent 框架持有（OnSurfaceCreated 下发、
//     OnSurfaceDestroyed 回收），本类只借用、不释放。本对象随桥接层在
//     OnSurfaceDestroyed 回调里先销毁（见 hongmeng_napi_bridge.cpp），
//     生命周期天然安全。
//   - 尺寸获取：Android 可随时 ANativeWindow_getWidth 现查；鸿蒙要在
//     XComponent 回调里用 OH_NativeXComponent_GetXComponentSize 查，
//     因此尺寸随构造传入并缓存；之后的尺寸变化走 nativeResize 事件通道
//     （dispatchEvent(SurfaceChanged) + renderer()->setSize），不经过本类。
//
// VK_USE_PLATFORM_OHOS 由构建系统定义（见 samples/hongmeng/.../CMakeLists.txt），
// 定义后 vulkan.h 才会包含 vulkan_ohos.h，暴露 vkCreateSurfaceOHOS 等声明。
// ============================================================================

#include "evk/render_platform.h"

#include <cstdint>

#include "evk/log.h"

namespace evk {

class HongmengPlatform : public IPlatform {
public:
    HongmengPlatform(OHNativeWindow* window, uint32_t width, uint32_t height)
        : window_(window), width_(width), height_(height) {}

    const char* getSurfaceExtensionName() const override {
        return VK_OHOS_SURFACE_EXTENSION_NAME;
    }

    bool createVulkanSurface(VkInstance instance, VkSurfaceKHR* surface) override {
        if (!window_) {
            EVK_LOGE("OHOS native window is null");
            return false;
        }
        VkSurfaceCreateInfoOHOS createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_SURFACE_CREATE_INFO_OHOS;
        createInfo.window = window_;
        if (vkCreateSurfaceOHOS(instance, &createInfo, nullptr, surface) != VK_SUCCESS) {
            EVK_LOGE("vkCreateSurfaceOHOS failed");
            return false;
        }
        return true;
    }

    void getSurfaceSize(uint32_t* width, uint32_t* height) override {
        *width = width_;
        *height = height_;
    }

private:
    OHNativeWindow* window_ = nullptr; // 借用指针，XComponent 框架持有
    uint32_t width_ = 0;               // 创建时刻的物理像素尺寸
    uint32_t height_ = 0;
};

IPlatform* createHongmengPlatform(OHNativeWindow* window, uint32_t width, uint32_t height) {
    return new HongmengPlatform(window, width, height);
}

void destroyHongmengPlatform(IPlatform* platform) {
    delete platform;
}

} // namespace evk

extern "C" {
// 以 C 链接导出工厂函数，NAPI 桥接层无需感知 HongmengPlatform 类的内存布局
// （与 Android evkCreateAndroidPlatform 同款约定）。
evk::IPlatform* evkCreateHongmengPlatform(void* window, int32_t width, int32_t height) {
    return evk::createHongmengPlatform(static_cast<OHNativeWindow*>(window),
                                       static_cast<uint32_t>(width),
                                       static_cast<uint32_t>(height));
}

void evkDestroyHongmengPlatform(evk::IPlatform* platform) {
    evk::destroyHongmengPlatform(platform);
}
} // extern "C"
