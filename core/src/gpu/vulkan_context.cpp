/**
 * @file vulkan_context.cpp
 * @brief Vulkan 地基实现：instance、surface、物理/逻辑设备与队列的创建。
 */
#include "evk/gpu/vulkan_context.h"

// EVK_LOGD/I/W/E 日志宏：spdlog 封装，全局统一的日志入口。
#include "evk/log.h"

// std::strcmp：比较扩展/校验层名字。
#include <cstring>
// std::vector：承接 Vulkan 枚举惯用法返回的列表。
#include <vector>

namespace evk::gpu {

// ---- 辅助函数 ----

/**
 * @brief validation layer 的调试回调；VKAPI_ATTR / VKAPI_CALL 是 Vulkan 调用约定宏，
 * 保证回调的调用方式与驱动期望一致（跨平台写回调必须带上）。
 */
static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT /*type*/,
    const VkDebugUtilsMessengerCallbackDataEXT* data,
    void* /*userData*/) {

    // 将 Vulkan 校验消息严重级别映射到日志级别。
    // severity 是位掩码且按位递增，用 >= 比较即可分级，Debug 兜底。
    // 消息前缀 evk-validation 便于在日志里过滤校验层输出。
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        EVK_LOGE("[evk-validation] {}", data->pMessage);
    } else if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        EVK_LOGW("[evk-validation] {}", data->pMessage);
    } else if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT) {
        EVK_LOGI("[evk-validation] {}", data->pMessage);
    } else {
        EVK_LOGD("[evk-validation] {}", data->pMessage);
    }
    // 返回 VK_FALSE 表示"不中止触发这条消息的 Vulkan 调用"：校验层只报告、不改变程序行为；
    // 返回 VK_TRUE 才会让该调用以 VK_ERROR_VALIDATION_FAILED_EXT 失败（几乎只用于调试）。
    return VK_FALSE;
}

/**
 * @brief 逐个核对想启用的校验层是否都在系统可用列表里。
 * @param layers 想启用的校验层名列表
 * @return 全部找到返回 true；任何一层找不到返回 false
 */
static bool checkValidationLayerSupport(const std::vector<const char*>& layers) {
    // Vulkan 两次调用枚举惯用法：第一次传 nullptr 只取回数量。
    uint32_t count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    // 按数量分配接收数组。
    std::vector<VkLayerProperties> available(count);
    // 第二次调用才真正把校验层属性填进数组。
    vkEnumerateInstanceLayerProperties(&count, available.data());

    // 逐个核对：想启用的每一层都必须出现在系统可用列表里。
    for (const char* layer : layers) {
        bool found = false;
        // layerName 是定长字符数组（不是 std::string），要用 strcmp 比较。
        for (const auto& props : available) {
            if (std::strcmp(props.layerName, layer) == 0) {
                found = true;
                break;
            }
        }
        // 任何一层找不到就直接判失败。
        if (!found) return false;
    }
    return true;
}

/// 小工具：在 vkEnumerate*ExtensionProperties 返回的列表里按名字找扩展。
static bool hasExtension(const std::vector<VkExtensionProperties>& props, const char* name) {
    // extensionName 同样是定长字符数组，命中即返回。
    for (const auto& p : props) {
        if (std::strcmp(p.extensionName, name) == 0) return true;
    }
    return false;
}

// ---- VulkanContext 实现 ----

VulkanContext::VulkanContext(IPlatform* platform) : platform_(platform) {}

VulkanContext::~VulkanContext() {
    // 析构兜底调 shutdown()；内部对空句柄有判断，重复调用也安全。
    shutdown();
}

bool VulkanContext::initialize() {
    // 按依赖顺序搭建 Vulkan 栈。每一步都依赖前一步，所以任何一环失败都会立刻停止启动。
    // ① 地基：VkInstance 是 Vulkan 入口；surface 必须基于 instance 创建。
    if (!createInstance()) return false;
    if (!createSurface()) return false;
    // ② 选 GPU 与建逻辑设备：这两步都要查询 surface 的 present 支持，故排在 surface 之后。
    if (!pickPhysicalDevice()) return false;
    if (!createLogicalDevice()) return false;
    return true;
}

void VulkanContext::shutdown() {
    // 逻辑设备先销毁：它是上层所有对象的"父对象"
    // （调用方已 vkDeviceWaitIdle 并销毁全部设备级子对象）。
    if (device_ != VK_NULL_HANDLE) {
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }

    if (instance_ != VK_NULL_HANDLE) {
        if (debugMessenger_ != VK_NULL_HANDLE) {
            // 调试消息器归实例所有，所以要先销毁它。
            // vkDestroyDebugUtilsMessengerEXT 同样是扩展函数，要用 vkGetInstanceProcAddr 动态取地址。
            auto func = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
            if (func) func(instance_, debugMessenger_, nullptr);
            debugMessenger_ = VK_NULL_HANDLE;
        }
        // surface 由实例创建，先于实例销毁。
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
        // 最后销毁 VkInstance：Vulkan 对象树的根。
        vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }

    // 句柄复位为空：保证 shutdown 幂等，析构再调也不会重复销毁。
    physicalDevice_ = VK_NULL_HANDLE;
}

bool VulkanContext::createInstance() {
    // 实例创建主要是在探测能力：先选 surface 扩展，加载器支持时再加调试扩展。
    // VkApplicationInfo：告诉驱动应用/引擎名称与 API 版本，驱动可据此做兼容性处理与优化。
    VkApplicationInfo appInfo{};
    // sType 是 Vulkan 所有 CreateInfo 结构体的固定首字段模式：标明自身类型，
    // 主要供 pNext 扩展链做类型识别；每个结构体都必须显式填。
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    // 名称与版本只是元数据；apiVersion 声明我们按 Vulkan 1.0 写，新版驱动也兼容。
    appInfo.pApplicationName = "estarx_vulkan";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "estarx_vulkan";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_0;

    // 先放入通用 surface 扩展，再按平台和调试能力补充。
    // VK_KHR_surface 是平台无关的窗口表面抽象，任何上屏渲染都要它；
    // 配套的平台 surface 扩展（每个窗口系统一个）由 IPlatform 提供。
    std::vector<const char*> extensions = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        platform_->getSurfaceExtensionName(),
    };
    // 平台额外要求的实例扩展（MoltenVK：portability 枚举相关）。
    platform_->getRequiredInstanceExtensions(extensions);

    // 枚举实例级扩展：仍是"先取数量、再取数据"的两次调用惯用法。
    uint32_t extCount = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &extCount, nullptr);
    std::vector<VkExtensionProperties> availableExts(extCount);
    vkEnumerateInstanceExtensionProperties(nullptr, &extCount, availableExts.data());

    // debug_utils 是可选扩展：先探测再启用，避免在不支持的加载器上直接创建失败。
    bool useDebugUtils = hasExtension(availableExts, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    if (useDebugUtils) {
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

    // 校验层是可选的，只在构建配置要求时启用。
    // 校验层只在构建定义了 EVK_ENABLE_VALIDATION 时才尝试启用，发布版完全不带。
    std::vector<const char*> layers;
#ifdef EVK_ENABLE_VALIDATION
    // VK_LAYER_KHRONOS_validation 是官方统一校验层，开发期能捕获绝大多数 API 误用。
    layers.push_back("VK_LAYER_KHRONOS_validation");
    // 目标机上可能没装校验层（尤其手机）：探测不到就静默放弃，而不是让初始化失败。
    if (!checkValidationLayerSupport(layers)) {
        layers.clear();
    }
#endif

    // VkInstanceCreateInfo 总装：应用信息 + 要启用的扩展与校验层。
    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    // 平台要求的实例创建标志（MoltenVK：VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR，
    // 不加则枚举不到可移植性物理设备）。
    createInfo.flags = platform_->getInstanceCreateFlags();
    createInfo.pApplicationInfo = &appInfo;
    // 扩展与层都以"数量 + 字符串指针数组"的形式传入。
    createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();
    createInfo.enabledLayerCount = static_cast<uint32_t>(layers.size());
    createInfo.ppEnabledLayerNames = layers.data();

    // vkCreateInstance 是整个 Vulkan 栈第一个真正干活的调用；失败通常是缺扩展或驱动问题。
    if (vkCreateInstance(&createInfo, nullptr, &instance_) != VK_SUCCESS) {
        EVK_LOGE("vkCreateInstance failed");
        return false;
    }

    // 同时满足"有 debug_utils 扩展"且"校验层真的启用了"才挂调试回调。
    if (useDebugUtils && !layers.empty()) {
        // messenger 配置：只收 WARNING / ERROR 级别的消息。
        VkDebugUtilsMessengerCreateInfoEXT debugCreateInfo{};
        debugCreateInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        debugCreateInfo.messageSeverity =
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        // 消息类型覆盖 general / validation / performance 三类。
        debugCreateInfo.messageType =
            VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        // pfnUserCallback 指向前面的 debugCallback；回调直接走 spdlog，不需要 pUserData。
        debugCreateInfo.pfnUserCallback = debugCallback;
        debugCreateInfo.pUserData = nullptr;

        // vkCreateDebugUtilsMessengerEXT 是扩展函数，不在 loader 的核心导出表里，
        // 必须用 vkGetInstanceProcAddr 按名字动态加载；加载不到就跳过（缺调试不影响渲染）。
        auto func = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT"));
        if (func) {
            func(instance_, &debugCreateInfo, nullptr, &debugMessenger_);
        }
    }

    return true;
}

bool VulkanContext::createSurface() {
    // surface 的创建交给平台抽象层处理。
    // 具体怎么建 surface 只有平台知道：Android 包 ANativeWindow，
    // 所以委托给 IPlatform，本文件不含任何平台 API。
    return platform_->createVulkanSurface(instance_, &surface_);
}

bool VulkanContext::pickPhysicalDevice() {
    // 只保留既能绘制到 surface、又能 present swapchain 图像的设备。
    // 枚举系统里所有支持 Vulkan 的 GPU（独显/核显/软渲染都可能出现）。
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance_, &count, nullptr);
    // count 为 0 说明设备上根本没有 Vulkan 驱动，直接失败。
    if (count == 0) {
        EVK_LOGE("no Vulkan-capable GPU found");
        return false;
    }

    // 第二次调用取回全部 GPU 句柄。
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance_, &count, devices.data());

    // 逐个 GPU 检查是否满足最低要求，第一个合格的直接用。
    for (const auto& dev : devices) {
        // 队列族：GPU 的硬件队列按能力分组（图形/计算/传输…），先查各族属性。
        uint32_t queueFamilyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &queueFamilyCount, nullptr);
        std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &queueFamilyCount, queueFamilies.data());

        // 要找两个队列族：一个能执行图形命令，一个能把图像呈现到我们的 surface。
        int graphicsFamily = -1;
        int presentFamily = -1;
        // 图形队列和呈现队列可能属于不同的队列族。
        for (uint32_t i = 0; i < queueFamilyCount; ++i) {
            // VK_QUEUE_GRAPHICS_BIT 表示该族支持图形命令（画三角形需要它）。
            if (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                graphicsFamily = static_cast<int>(i);
            }
            // present 支持不能看 queueFlags，必须用 vkGetPhysicalDeviceSurfaceSupportKHR
            // 针对具体 surface 查询：同一 GPU 对不同 surface 的答案可能不同。
            VkBool32 presentSupport = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(dev, i, surface_, &presentSupport);
            if (presentSupport) {
                presentFamily = static_cast<int>(i);
            }
        }

        // 再查设备级扩展：swapchain 是设备扩展，没有它就无法把渲染结果换到屏幕。
        uint32_t extCount = 0;
        vkEnumerateDeviceExtensionProperties(dev, nullptr, &extCount, nullptr);
        std::vector<VkExtensionProperties> exts(extCount);
        vkEnumerateDeviceExtensionProperties(dev, nullptr, &extCount, exts.data());
        // VK_KHR_swapchain 是显示图像的必备扩展；规范不保证存在，必须探测。
        bool swapchainSupported = hasExtension(exts, VK_KHR_SWAPCHAIN_EXTENSION_NAME);

        // 三个条件同时满足才算合适：能画、能呈现、支持 swapchain。
        if (graphicsFamily >= 0 && presentFamily >= 0 && swapchainSupported) {
            physicalDevice_ = dev;
            return true;
        }
    }

    // 遍历完所有 GPU 都不满足要求，只能报错放弃。
    EVK_LOGE("no suitable GPU found");
    return false;
}

bool VulkanContext::createLogicalDevice() {
    // 创建最小化的逻辑设备，并申请所需的队列族。
    // 与挑选设备时相同的队列族查询，这次针对已选定的 GPU，要拿准确的族索引来建队列。
    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &queueFamilyCount, queueFamilies.data());

    // 再定位一次 graphics / present 族：多数手机 GPU 上两者是同一族，桌面 GPU 可能分开。
    int graphicsFamily = -1;
    int presentFamily = -1;
    for (uint32_t i = 0; i < queueFamilyCount; ++i) {
        if (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            graphicsFamily = static_cast<int>(i);
        }
        VkBool32 presentSupport = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(physicalDevice_, i, surface_, &presentSupport);
        if (presentSupport) {
            presentFamily = static_cast<int>(i);
        }
    }

    // 有些设备一个队列族同时承担两种职责，有些则需要分开的图形和呈现队列。
    // 队列族去重：若两族相同只建一条队列，同一族重复申请没有意义。
    std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
    std::vector<uint32_t> uniqueFamilies = {static_cast<uint32_t>(graphicsFamily)};
    if (static_cast<uint32_t>(presentFamily) != static_cast<uint32_t>(graphicsFamily)) {
        uniqueFamilies.push_back(static_cast<uint32_t>(presentFamily));
    }

    // 队列优先级范围 0.0~1.0；每个族只要一条队列时取值无所谓，给 1.0。
    float queuePriority = 1.0f;
    // 每个去重后的族填一份 VkDeviceQueueCreateInfo：族索引 + 队列数 + 优先级指针。
    for (uint32_t family : uniqueFamilies) {
        VkDeviceQueueCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        info.queueFamilyIndex = family;
        info.queueCount = 1;
        info.pQueuePriorities = &queuePriority;
        queueCreateInfos.push_back(info);
    }

    // 空的 VkPhysicalDeviceFeatures{}：2D UI 不需要任何可选设备特性（几何着色器、宽线等）。
    VkPhysicalDeviceFeatures features{};

    // 设备级必须启用 VK_KHR_swapchain，否则后面创建交换链会被校验层拦下。
    std::vector<const char*> deviceExtensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    // 平台额外要求的设备扩展（MoltenVK：VK_KHR_portability_subset）。
    platform_->getRequiredDeviceExtensions(deviceExtensions);

    // VkDeviceCreateInfo 总装：队列需求 + 特性 + 扩展。
    VkDeviceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
    createInfo.pQueueCreateInfos = queueCreateInfos.data();
    createInfo.pEnabledFeatures = &features;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size());
    createInfo.ppEnabledExtensionNames = deviceExtensions.data();

    // 逻辑设备 vs 物理设备：physical 是硬件本身，logical 是我们与硬件交互的"会话"，
    // 之后几乎所有 Vulkan 调用都以 device_ 为第一个参数。
    if (vkCreateDevice(physicalDevice_, &createInfo, nullptr, &device_) != VK_SUCCESS) {
        EVK_LOGE("vkCreateDevice failed");
        return false;
    }

    // 队列句柄不用"创建"：设备建好后按族索引取出即可（每族只建了 1 条，取 index 0）。
    vkGetDeviceQueue(device_, static_cast<uint32_t>(graphicsFamily), 0, &graphicsQueue_);
    vkGetDeviceQueue(device_, static_cast<uint32_t>(presentFamily), 0, &presentQueue_);
    return true;
}

uint32_t VulkanContext::findMemoryType(uint32_t typeFilter,
                                       VkMemoryPropertyFlags properties) const {
    // 在物理设备的内存类型里找一个满足这些标志位的类型。
    // 先取物理设备的内存类型表：每类带一组属性标志（HOST_VISIBLE / DEVICE_LOCAL 等）。
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &memProps);

    // 逐个内存类型检查两个条件。
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        // typeFilter 是位掩码：第 i 位为 1 表示资源允许用第 i 类内存；
        // (propertyFlags & properties) == properties 是子集测试：我们要的属性它全都有。
        if ((typeFilter & (1u << i)) &&
            (memProps.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    // 找不到时兜底返回 0：教学代码的简化，严格做法是报错。
    return 0;
}

VkShaderModule VulkanContext::createShaderModule(const uint32_t* code,
                                                 size_t codeSize) const {
    // shader 在 Vulkan 里是预编译的 SPIR-V 字节码；shader module 只是它的薄包装。
    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    // codeSize 单位是字节（不是 uint32 个数）；pCode 要求 4 字节对齐。
    createInfo.codeSize = codeSize;
    createInfo.pCode = code;

    // 这里开销很小：SPIR-V 到 GPU 指令的真正编译发生在管线创建时。
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device_, &createInfo, nullptr, &module) != VK_SUCCESS) {
        EVK_LOGE("vkCreateShaderModule failed");
        return VK_NULL_HANDLE;
    }
    return module;
}

} // namespace evk::gpu
