// Renderer 主头文件：声明本文件要实现的所有方法与 Vulkan 句柄成员。
#include "evk/renderer.h"
// 预编译 SPIR-V 字节码（assets::ui_vert_spv / ui_frag_spv），编译期内嵌进二进制，免运行时读文件。
#include "evk/assets/ui_shaders.h"

// std::max / std::min：交换链尺寸与裁剪矩形的边界 clamp 要用。
#include <algorithm>
// std::strcmp / std::memcpy：比较扩展/校验层名字、把顶点数据拷进映射内存。
#include <cstring>
// std::vector：承接 Vulkan 枚举惯用法返回的列表。
#include <vector>

// 关键宏：让 GLM 的投影矩阵把深度映射到 Vulkan 的 NDC 范围 [0,1]，
// 而不是 OpenGL 默认的 [-1,1]；必须在包含任何 glm 头之前定义才生效。
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
// glm 核心类型（mat4 等），用于构造正交投影矩阵。
#include <glm/glm.hpp>
// glm::ortho 所在的矩阵变换头。
#include <glm/gtc/matrix_transform.hpp>

namespace evk {

// ============================================================================
// 辅助函数
// ============================================================================

// validation layer 的调试回调；VKAPI_ATTR / VKAPI_CALL 是 Vulkan 调用约定宏，
// 保证回调的调用方式与驱动期望一致（跨平台写回调必须带上）。
static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT /*type*/,
    const VkDebugUtilsMessengerCallbackDataEXT* data,
    void* userData) {

    // userData 是创建 messenger 时传入的 IPlatform*，取回后用来打日志。
    auto* platform = static_cast<IPlatform*>(userData);
    // 将 Vulkan 校验消息严重级别映射到渲染器的日志级别。
    // severity 是位掩码且按位递增，用 >= 比较即可分级，默认 Debug 兜底。
    LogLevel level = LogLevel::Debug;
    // ERROR 及以上 → Error 日志。
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        level = LogLevel::Error;
    // WARNING → Warn 日志。
    } else if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        level = LogLevel::Warn;
    // INFO → Info 日志；更低的 VERBOSE 保持 Debug。
    } else if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT) {
        level = LogLevel::Info;
    }
    // 校验消息原样转发到平台日志，tag 标成 evk-validation 便于过滤。
    logMessage(platform, level, "evk-validation", "%s", data->pMessage);
    // 返回 VK_FALSE 表示"不中止触发这条消息的 Vulkan 调用"：校验层只报告、不改变程序行为；
    // 返回 VK_TRUE 才会让该调用以 VK_ERROR_VALIDATION_FAILED_EXT 失败（几乎只用于调试）。
    return VK_FALSE;
}

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

// 小工具：在 vkEnumerate*ExtensionProperties 返回的列表里按名字找扩展。
static bool hasExtension(const std::vector<VkExtensionProperties>& props, const char* name) {
    // extensionName 同样是定长字符数组，命中即返回。
    for (const auto& p : props) {
        if (std::strcmp(p.extensionName, name) == 0) return true;
    }
    return false;
}

// ============================================================================
// 渲染器实现
// ============================================================================

// 单次 render() 内"重建交换链 → 重画"的最大重试次数（见 render() 注释）。
constexpr uint32_t kMaxFrameAttempts = 3;

// 构造只保存平台抽象层指针；所有 Vulkan 对象推迟到 initialize() 里创建。
Renderer::Renderer(IPlatform* platform) : platform_(platform) {}

Renderer::~Renderer() {
    // 析构兜底调 shutdown()；内部对空句柄有判断，重复调用也安全。
    shutdown();
}

bool Renderer::initialize() {
    // 按依赖顺序搭建 Vulkan 栈。每一步都依赖前一步，所以任何一环失败都会立刻停止启动。
    // ① 地基：VkInstance 是 Vulkan 入口；surface 必须基于 instance 创建。
    if (!createInstance()) return false;
    if (!createSurface()) return false;
    // ② 选 GPU 与建逻辑设备：这两步都要查询 surface 的 present 支持，故排在 surface 之后。
    if (!pickPhysicalDevice()) return false;
    if (!createLogicalDevice()) return false;
    // ③ 交换链及其下游：imageView / renderPass / pipeline / framebuffer 全部依赖 swapchain 的
    // 格式与尺寸，窗口尺寸变化时这一段要整体重建（见 recreateSwapchain）。
    if (!createSwapchain()) return false;
    if (!createImageViews()) return false;
    if (!createRenderPass()) return false;
    if (!createGraphicsPipeline()) return false;
    if (!createFramebuffers()) return false;
    // ④ 执行设施：命令池、顶点缓冲、命令缓冲、同步原语，与 swapchain 尺寸无关。
    if (!createCommandPool()) return false;
    if (!createVertexBuffer()) return false;
    if (!createCommandBuffers()) return false;
    if (!createSyncObjects()) return false;
    return true;
}

void Renderer::shutdown() {
    // 按相反顺序释放资源。设备必须先空闲，才能销毁任何仍在飞行中的对象。
    // 初始化可能中途失败退出，device_ 仍是空句柄，先判一下。
    if (device_ != VK_NULL_HANDLE) {
        // 先等 GPU 跑完所有在飞工作：销毁仍在被 GPU 使用的对象是未定义行为。
        vkDeviceWaitIdle(device_);

        // 同步原语：fence 与两类 semaphore，每帧一套。
        for (auto fence : inFlightFences_) vkDestroyFence(device_, fence, nullptr);
        for (auto sem : renderFinishedSemaphores_) vkDestroySemaphore(device_, sem, nullptr);
        for (auto sem : imageAvailableSemaphores_) vkDestroySemaphore(device_, sem, nullptr);

        // VkBuffer 与它的内存是两个独立对象：先销毁 buffer 句柄，再释放设备内存。
        vkDestroyBuffer(device_, vertexBuffer_, nullptr);
        vkFreeMemory(device_, vertexBufferMemory_, nullptr);

        // 销毁命令池会连带释放从中分配的所有命令缓冲。
        vkDestroyCommandPool(device_, commandPool_, nullptr);

        // swapchain 相关资源（framebuffer / pipeline / renderPass / imageView / swapchain）集中清理。
        cleanupSwapchain();

        // 逻辑设备最后销毁：它是上面所有对象的"父对象"。
        vkDestroyDevice(device_, nullptr);
    }

    if (instance_ != VK_NULL_HANDLE) {
        if (debugMessenger_ != VK_NULL_HANDLE) {
            // 调试消息器归实例所有，所以要先销毁它。
            // vkDestroyDebugUtilsMessengerEXT 同样是扩展函数，要用 vkGetInstanceProcAddr 动态取地址。
            auto func = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
            if (func) func(instance_, debugMessenger_, nullptr);
        }
        // surface 由实例创建，先于实例销毁。
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
        // 最后销毁 VkInstance：Vulkan 对象树的根。
        vkDestroyInstance(instance_, nullptr);
    }

    // 清空所有持有句柄的 vector，避免残留已销毁的句柄值。
    inFlightFences_.clear();
    renderFinishedSemaphores_.clear();
    imageAvailableSemaphores_.clear();
    commandBuffers_.clear();
    swapchainFramebuffers_.clear();
    swapchainImageViews_.clear();
    swapchainImages_.clear();

    // 成员句柄复位为空：保证 shutdown 幂等，析构再调也不会重复销毁。
    device_ = VK_NULL_HANDLE;
    physicalDevice_ = VK_NULL_HANDLE;
    surface_ = VK_NULL_HANDLE;
    instance_ = VK_NULL_HANDLE;
}

bool Renderer::createInstance() {
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
    // VK_KHR_surface 是平台无关的窗口表面抽象，任何上屏渲染都要它。
    std::vector<const char*> extensions = {
        VK_KHR_SURFACE_EXTENSION_NAME,
#ifdef __ANDROID__
        // Android 还需平台相关的 VK_KHR_android_surface（每个窗口系统都有自己的 surface 扩展）。
        VK_KHR_ANDROID_SURFACE_EXTENSION_NAME,
#endif
    };

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
    createInfo.pApplicationInfo = &appInfo;
    // 扩展与层都以"数量 + 字符串指针数组"的形式传入。
    createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();
    createInfo.enabledLayerCount = static_cast<uint32_t>(layers.size());
    createInfo.ppEnabledLayerNames = layers.data();

    // vkCreateInstance 是整个 Vulkan 栈第一个真正干活的调用；失败通常是缺扩展或驱动问题。
    if (vkCreateInstance(&createInfo, nullptr, &instance_) != VK_SUCCESS) {
        logMessage(platform_, LogLevel::Error, "evk", "vkCreateInstance failed");
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
        // pfnUserCallback 指向前面的 debugCallback；pUserData 透传 IPlatform 供回调打日志。
        debugCreateInfo.pfnUserCallback = debugCallback;
        debugCreateInfo.pUserData = platform_;

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

bool Renderer::createSurface() {
    // surface 的创建交给平台抽象层处理。
    // 具体怎么建 surface 只有平台知道：Android 包 ANativeWindow，
    // 所以委托给 IPlatform，本文件不含任何平台 API。
    return platform_->createVulkanSurface(instance_, &surface_);
}

bool Renderer::pickPhysicalDevice() {
    // 只保留既能绘制到 surface、又能 present swapchain 图像的设备。
    // 枚举系统里所有支持 Vulkan 的 GPU（独显/核显/软渲染都可能出现）。
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance_, &count, nullptr);
    // count 为 0 说明设备上根本没有 Vulkan 驱动，直接失败。
    if (count == 0) {
        logMessage(platform_, LogLevel::Error, "evk", "no Vulkan-capable GPU found");
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
    logMessage(platform_, LogLevel::Error, "evk", "no suitable GPU found");
    return false;
}

bool Renderer::createLogicalDevice() {
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
    const char* deviceExtensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};

    // VkDeviceCreateInfo 总装：队列需求 + 特性 + 扩展。
    VkDeviceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
    createInfo.pQueueCreateInfos = queueCreateInfos.data();
    createInfo.pEnabledFeatures = &features;
    createInfo.enabledExtensionCount = 1;
    createInfo.ppEnabledExtensionNames = deviceExtensions;

    // 逻辑设备 vs 物理设备：physical 是硬件本身，logical 是我们与硬件交互的"会话"，
    // 之后几乎所有 Vulkan 调用都以 device_ 为第一个参数。
    if (vkCreateDevice(physicalDevice_, &createInfo, nullptr, &device_) != VK_SUCCESS) {
        logMessage(platform_, LogLevel::Error, "evk", "vkCreateDevice failed");
        return false;
    }

    // 队列句柄不用"创建"：设备建好后按族索引取出即可（每族只建了 1 条，取 index 0）。
    vkGetDeviceQueue(device_, static_cast<uint32_t>(graphicsFamily), 0, &graphicsQueue_);
    vkGetDeviceQueue(device_, static_cast<uint32_t>(presentFamily), 0, &presentQueue_);
    return true;
}

bool Renderer::createSwapchain() {
    // 选择适合当前窗口的 surface 格式、present 模式和尺寸。
    // 三连查询之一：surface 能力（尺寸范围、图像数量范围、当前变换等）。
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice_, surface_, &caps);

    // 先查询 surface 能力，再决定具体的 swapchain 参数。
    // 三连查询之二：surface 支持的像素格式 + 色彩空间列表。
    uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_, surface_, &formatCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    if (formatCount > 0) {
        vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_, surface_, &formatCount, formats.data());
    }

    // 三连查询之三：支持的 present 模式（图像以什么节奏换到屏幕）。
    uint32_t presentModeCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice_, surface_, &presentModeCount, nullptr);
    std::vector<VkPresentModeKHR> presentModes(presentModeCount);
    if (presentModeCount > 0) {
        vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice_, surface_, &presentModeCount, presentModes.data());
    }

    // 选格式：默认取第一个，但优先 B8G8R8A8_UNORM + sRGB：移动端最常见、与 shader 输出匹配。
    VkSurfaceFormatKHR surfaceFormat = formats[0];
    for (const auto& f : formats) {
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            surfaceFormat = f;
            break;
        }
    }

    // present 模式先兜底 FIFO：垂直同步、不撕裂，规范保证所有驱动都支持。
    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
    for (const auto& pm : presentModes) {
        // 驱动支持时优先用 Mailbox，可降低显示延迟。
        // MAILBOX 相当于三重缓冲：队列满时新帧直接替换旧帧，比 FIFO 延迟低，有则优先。
        if (pm == VK_PRESENT_MODE_MAILBOX_KHR) {
            presentMode = pm;
            break;
        }
    }

    VkExtent2D extent;
    // 如果 surface 固定了尺寸，Vulkan 会直接给出精确的 extent。
    // UINT32_MAX 是规范约定的"尺寸不固定"标记；这里先处理"已固定"的常见分支（多数手机如此）。
    if (caps.currentExtent.width != UINT32_MAX) {
        extent = caps.currentExtent;
    } else {
        // UINT32_MAX 表示尺寸可由我们自由选（桌面窗口常见）：向平台要当前像素尺寸，
        // 再 clamp 到驱动允许的 [minImageExtent, maxImageExtent] 区间。
        platform_->getSurfaceSize(&width_, &height_);
        extent.width = std::max(caps.minImageExtent.width, std::min(caps.maxImageExtent.width, width_));
        extent.height = std::max(caps.minImageExtent.height, std::min(caps.maxImageExtent.height, height_));
    }

    // 90/270 变换下呈现时系统会把 buffer 旋转上屏，buffer 必须是"旋转前"尺寸
    // （与视觉尺寸互换），否则旋转后再被拉伸适配，画面压扁/偏移。
    // 平台可能上报旋转前尺寸，也可能上报与 SurfaceView 相同的视觉尺寸。
    // 仅在 extent 与 SurfaceView 尺寸一致时交换，避免已是旋转前尺寸的平台被重复交换。
    // width_/height_ 还没上报过（为 0）时无法区分，维持原值；首次 setSize 触发的
    // 重建会再走到这里修正。
    const bool rotate90or270 = (caps.currentTransform == VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR ||
                                caps.currentTransform == VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR);
    if (rotate90or270 && width_ != 0 && height_ != 0 &&
        extent.width == width_ && extent.height == height_) {
        std::swap(extent.width, extent.height);
        // 互换后必须仍落在驱动允许的范围内；越界则换回，宁可不做补偿。
        if (extent.width < caps.minImageExtent.width || extent.width > caps.maxImageExtent.width ||
            extent.height < caps.minImageExtent.height || extent.height > caps.maxImageExtent.height) {
            std::swap(extent.width, extent.height);
        }
    }

    // 图像数量取 minImageCount + 1：多一张可减少"等上一帧渲染完"的停顿；
    // maxImageCount 为 0 表示无上限，否则不能越界。
    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount) {
        imageCount = caps.maxImageCount;
    }

    // VkSwapchainCreateInfoKHR 总装：把上面选好的参数全部装进去。
    VkSwapchainCreateInfoKHR createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    createInfo.surface = surface_;
    // minImageCount 是"至少几张"，驱动实际可能给更多。
    createInfo.minImageCount = imageCount;
    createInfo.imageFormat = surfaceFormat.format;
    createInfo.imageColorSpace = surfaceFormat.colorSpace;
    createInfo.imageExtent = extent;
    // imageArrayLayers 非 VR/立体渲染恒为 1。
    createInfo.imageArrayLayers = 1;
    // imageUsage 标明图像用途：我们直接渲染进去，所以是 COLOR_ATTACHMENT。
    createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    // preTransform 用 surface 当前变换（如手机旋转），交给系统处理而不是自己转。
    createInfo.preTransform = caps.currentTransform;
    // 记录下来：recordCommandBuffer 的投影与裁剪要按它做旋转补偿，
    // 否则折叠屏内屏竖持（currentTransform=ROTATE_90）时画面是横的。
    surfaceTransform_ = caps.currentTransform;
    logMessage(platform_, LogLevel::Info, "evk", "swapchain extent=%ux%u transform=%d",
               extent.width, extent.height, static_cast<int>(caps.currentTransform));
    // compositeAlpha 不透明：不与系统里其它内容做 alpha 合成。
    createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    createInfo.presentMode = presentMode;
    // clipped = true 允许驱动优化掉被遮挡像素的呈现开销。
    createInfo.clipped = VK_TRUE;
    // oldSwapchain 重建时可传旧交换链加速过渡；这里从头建，给空句柄。
    createInfo.oldSwapchain = VK_NULL_HANDLE;

    uint32_t queueFamilyIndices[2]; // 下面填充

    // 重新取一次队列族索引，确保 swapchain 的共享模式正确。
    uint32_t qCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &qCount, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &qCount, qf.data());
    // 找出图形族与呈现族索引，供下面决定图像共享模式。
    uint32_t gFamily = 0, pFamily = 0;
    for (uint32_t i = 0; i < qCount; ++i) {
        if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) gFamily = i;
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(physicalDevice_, i, surface_, &present);
        if (present) pFamily = i;
    }

    // 如果图形和呈现队列族分开，swapchain 图像需要并发共享。
    // 两族不同时图像会在两条队列间被使用：CONCURRENT 允许多族并发访问、免显式所有权转移，
    // 代价是驱动不能做独占优化；这种模式需要列出共享的队列族。
    if (gFamily != pFamily) {
        queueFamilyIndices[0] = gFamily;
        queueFamilyIndices[1] = pFamily;
        createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        createInfo.queueFamilyIndexCount = 2;
        createInfo.pQueueFamilyIndices = queueFamilyIndices;
    } else {
        // 同一族兼任时 EXCLUSIVE 更高效：驱动可做独占优化，也是手机上最常见的路径。
        createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    // 创建交换链；失败多为所选参数组合不被驱动支持。
    if (vkCreateSwapchainKHR(device_, &createInfo, nullptr, &swapchain_) != VK_SUCCESS) {
        logMessage(platform_, LogLevel::Error, "evk", "vkCreateSwapchainKHR failed");
        return false;
    }

    // 创建后再取回真正的图像句柄列表（数量可能与请求值不同，仍是两次调用惯用法）。
    vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount, nullptr);
    swapchainImages_.resize(imageCount);
    vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount, swapchainImages_.data());

    // 记住格式与尺寸：后面 renderPass、pipeline、framebuffer、viewport 都要用。
    swapchainImageFormat_ = surfaceFormat.format;
    swapchainExtent_ = extent;
    return true;
}

bool Renderer::createImageViews() {
    // 每个 swapchain 图像都要创建一个 2D 颜色视图。
    // VkImage 只是显存里的原始数据，VkImageView 描述"如何解读它"（格式/维度/范围）；
    // renderPass 与 framebuffer 引用的是视图而不是图像本身。
    swapchainImageViews_.resize(swapchainImages_.size());
    for (size_t i = 0; i < swapchainImages_.size(); ++i) {
        VkImageViewCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        createInfo.image = swapchainImages_[i];
        // 按 2D 图像、swapchain 自身格式来解读。
        createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        createInfo.format = swapchainImageFormat_;
        // components 是 RGBA 通道重排（swizzle）；IDENTITY 表示不重排、原样读写。
        createInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        createInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        createInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        createInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
        // subresourceRange 限定视图覆盖的范围：颜色层面、1 个 mip 级别、1 个数组层，即整张图。
        createInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        createInfo.subresourceRange.baseMipLevel = 0;
        createInfo.subresourceRange.levelCount = 1;
        createInfo.subresourceRange.baseArrayLayer = 0;
        createInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(device_, &createInfo, nullptr, &swapchainImageViews_[i]) != VK_SUCCESS) {
            logMessage(platform_, LogLevel::Error, "evk", "vkCreateImageView failed");
            return false;
        }
    }
    return true;
}

bool Renderer::createRenderPass() {
    // 这个三角形示例只需要一个颜色附件：清屏、绘制、呈现即可。
    // 附件描述：renderPass 不绑定具体图像，只声明"有一个什么样的附件、怎么用"。
    VkAttachmentDescription colorAttachment{};
    // 格式必须与 swapchain 图像一致，否则后面创建 framebuffer 会失败。
    colorAttachment.format = swapchainImageFormat_;
    // 单采样，不做 MSAA。
    colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    // loadOp = CLEAR：render pass 开始时把附件清成 clearValue，每帧清屏就靠它。
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    // storeOp = STORE：渲染结束后保留结果；要呈现到屏幕，必须存。
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    // 没有模板附件，stencil 操作直接 DONT_CARE。
    colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    // initialLayout = UNDEFINED：不关心旧内容（反正要清屏），允许驱动直接丢弃它。
    colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    // finalLayout = PRESENT_SRC_KHR：渲染完图像要交给呈现引擎，布局必须转成它。
    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    // 附件引用：subpass 不直接用附件，而是引用其下标（0 即上面的 colorAttachment），
    // 并指定渲染期间它处于 COLOR_ATTACHMENT_OPTIMAL 布局（最适合作为渲染目标）。
    VkAttachmentReference colorRef{};
    colorRef.attachment = 0;
    colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    // 唯一的 subpass：跑图形管线、写 1 个颜色附件。
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;

    // subpass 依赖：描述"render pass 之外到我们的 subpass"的执行与内存依赖，
    // Vulkan 靠它自动插入布局转换与同步，避免图像还没 acquire 完就被写入。
    VkSubpassDependency dependency{};
    // srcSubpass = EXTERNAL 指 render pass 之前的操作（这里是 acquire/上一帧呈现）。
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    // dstSubpass = 0 就是我们的第一个（也是唯一一个）subpass。
    dependency.dstSubpass = 0;
    // src 侧等到 COLOR_ATTACHMENT_OUTPUT 阶段；srcAccessMask = 0 表示无需等内存可见。
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.srcAccessMask = 0;
    // dst 侧：我们的 subpass 要在颜色输出阶段执行写操作（COLOR_ATTACHMENT_WRITE）。
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    // 总装：1 个附件、1 个 subpass、1 条依赖，最小可用 render pass。
    VkRenderPassCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    createInfo.attachmentCount = 1;
    createInfo.pAttachments = &colorAttachment;
    createInfo.subpassCount = 1;
    createInfo.pSubpasses = &subpass;
    createInfo.dependencyCount = 1;
    createInfo.pDependencies = &dependency;

    if (vkCreateRenderPass(device_, &createInfo, nullptr, &renderPass_) != VK_SUCCESS) {
        logMessage(platform_, LogLevel::Error, "evk", "vkCreateRenderPass failed");
        return false;
    }
    return true;
}

VkShaderModule Renderer::createShaderModule(const uint32_t* code, size_t codeSize) {
    // shader 在 Vulkan 里是预编译的 SPIR-V 字节码；shader module 只是它的薄包装。
    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    // codeSize 单位是字节（不是 uint32 个数）；pCode 要求 4 字节对齐。
    createInfo.codeSize = codeSize;
    createInfo.pCode = code;

    // 这里开销很小：SPIR-V 到 GPU 指令的真正编译发生在管线创建时。
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device_, &createInfo, nullptr, &module) != VK_SUCCESS) {
        logMessage(platform_, LogLevel::Error, "evk", "vkCreateShaderModule failed");
        return VK_NULL_HANDLE;
    }
    return module;
}

bool Renderer::createGraphicsPipeline() {
    // 这个示例的管线是固定的。viewport 和 scissor 保持动态，所以缩放时只需重录命令缓冲。
    // 从内嵌字节码建 vert / frag 两个 shader module；管线建完即可销毁（见函数尾）。
    VkShaderModule vertModule = createShaderModule(
        assets::ui_vert_spv, sizeof(assets::ui_vert_spv));
    VkShaderModule fragModule = createShaderModule(
        assets::ui_frag_spv, sizeof(assets::ui_frag_spv));

    // 任一 shader module 创建失败都无法继续。
    if (vertModule == VK_NULL_HANDLE || fragModule == VK_NULL_HANDLE) {
        return false;
    }

    // 顶点着色器阶段：逐顶点执行，本例把像素坐标乘 mvp 转到 NDC。
    VkPipelineShaderStageCreateInfo vertStage{};
    vertStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    // stage 指明该模块在管线的哪个可编程阶段工作。
    vertStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertStage.module = vertModule;
    // pName 是 shader 入口函数名，SPIR-V 里固定叫 "main"。
    vertStage.pName = "main";

    // 片段着色器阶段：逐像素执行，输出插值后的顶点色。
    VkPipelineShaderStageCreateInfo fragStage{};
    fragStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fragStage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragStage.module = fragModule;
    fragStage.pName = "main";

    // 两个阶段装进数组，管线创建时引用。
    VkPipelineShaderStageCreateInfo stages[] = {vertStage, fragStage};

    // 顶点数据是 UiVertex 交错布局：先像素坐标(vec2)，再 RGBA 颜色(vec4)。
    // binding 描述"顶点缓冲怎么喂"：binding 0 对应 vkCmdBindVertexBuffers 的槽位 0。
    VkVertexInputBindingDescription bindingDesc{};
    bindingDesc.binding = 0;
    // stride = 24 字节：UiVertex = vec2 位置 + vec4 颜色 = 6 个 float，交错存放。
    bindingDesc.stride = sizeof(float) * 6;
    // inputRate = VERTEX 表示逐顶点推进（另一种是 INSTANCE，用于实例化渲染）。
    bindingDesc.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    // attribute 描述 shader 里每个输入变量在顶点数据里的位置与格式。
    VkVertexInputAttributeDescription attrs[2];
    // attribute 0 取自 binding 0，对应 shader 里 layout(location=0) 的 vec2 位置。
    attrs[0].binding = 0;
    attrs[0].location = 0;
    // R32G32_SFLOAT 即两个 float；offset 0 表示位置在顶点结构体开头。
    attrs[0].format = VK_FORMAT_R32G32_SFLOAT;
    attrs[0].offset = 0;
    // attribute 1 对应 layout(location=1) 的 vec4 颜色，格式 R32G32B32A32_SFLOAT。
    attrs[1].binding = 0;
    attrs[1].location = 1;
    attrs[1].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    // offset = 8 字节：颜色紧跟在两个位置 float 之后。
    attrs[1].offset = sizeof(float) * 2;

    // 顶点输入状态 = binding + attribute 的总装，管线的固定功能阶段之一。
    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &bindingDesc;
    vertexInput.vertexAttributeDescriptionCount = 2;
    vertexInput.pVertexAttributeDescriptions = attrs;

    // 输入装配：告诉管线顶点流怎么组成图元。
    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    // TRIANGLE_LIST：每 3 个顶点一个独立三角形（UI 矩形就是两个三角形拼的）。
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    // primitiveRestart 是条带拓扑的重启索引功能，列表拓扑用不到。
    inputAssembly.primitiveRestartEnable = VK_FALSE;

    // viewport 把 NDC 映射到 framebuffer 像素；这里先按当前尺寸填一份占位，
    // 真正生效的是录制命令时 vkCmdSetViewport 设置的那份（动态状态）。
    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(swapchainExtent_.width);
    viewport.height = static_cast<float>(swapchainExtent_.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;

    // scissor 把绘制裁剪到矩形内；同样只是占位，每批绘制时动态设置。
    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = swapchainExtent_;

    // viewport/scissor 的数量在建管线时就固定（即使值是动态的），所以指针仍要给。
    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.pViewports = &viewport;
    viewportState.scissorCount = 1;
    viewportState.pScissors = &scissor;

    // 光栅化器：把三角形变成片段。
    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    // depthClamp 会把越界深度钳到 [0,1] 而非裁掉（需设备特性），2D 不需要。
    rasterizer.depthClampEnable = VK_FALSE;
    // rasterizerDiscard 会完全跳过光栅化（变换反馈场景用），正常渲染必须 FALSE。
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    // FILL 是实心填充；LINE 线框 / POINT 点模式需要额外设备特性。
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    // lineWidth 非 1.0 需要 wideLines 特性，填默认 1.0。
    rasterizer.lineWidth = 1.0f;
    // 不做面剔除：UI 三角形顶点顺序不统一，剔除反而会丢内容。
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    // frontFace 在剔除关闭时无实际影响，给个合法值即可。
    rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;
    // depthBias 用于阴影贴图类深度偏移，2D 关闭。
    rasterizer.depthBiasEnable = VK_FALSE;

    // 多重采样状态：单采样（每像素 1 个样本），相当于关闭 MSAA。
    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.sampleShadingEnable = VK_FALSE;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // 颜色混合附件状态：描述这一个颜色附件的写入与混合方式。
    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    // colorWriteMask 全开 RGBA 四个通道的写入。
    colorBlendAttachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    // 标准 SRC_ALPHA 预乘外的普通 alpha 混合，UI 半透明背景需要。
    colorBlendAttachment.blendEnable = VK_TRUE;
    // 颜色通道公式：src×SRC_ALPHA + dst×(1-SRC_ALPHA)，标准 srcOver 合成。
    colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    // alpha 通道公式：src×1 + dst×(1-srcAlpha)，让叠层后的不透明度正确累计。
    colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;

    // 全局混合状态：可挂多个附件的混合配置，我们只有 1 个附件。
    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    // logicOp 是按位逻辑混合（老 OpenGL 功能），与普通混合互斥，关掉。
    colorBlending.logicOpEnable = VK_FALSE;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlendAttachment;

    // 动态状态列表：声明哪些状态建管线时不固化、留到录制命令时再设。
    VkDynamicState dynamicStates[] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
    };

    // 保持 viewport 和 scissor 为动态状态；swapchain 的尺寸可能运行时变化。
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynamicStates;

    // push constant：像素坐标 → NDC 的正交投影矩阵（mat4 mvp）。
    // push constant 是 CPU 直接塞给 shader 的小块数据（规范保底 128 字节），
    // 比 uniform buffer 轻量，适合每帧/每批都变的小矩阵；64 字节正好一个 mat4。
    VkPushConstantRange pushConstant{};
    // stageFlags = VERTEX：只有顶点着色器能读到它。
    pushConstant.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushConstant.offset = 0;
    pushConstant.size = 64;

    // 管线布局 = shader 的"接口签名"：声明它会用到哪些 descriptor 与 push constant。
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushConstant;

    // 布局创建失败也要先销毁两个 shader module 再返回，避免泄漏。
    if (vkCreatePipelineLayout(device_, &layoutInfo, nullptr, &pipelineLayout_) != VK_SUCCESS) {
        logMessage(platform_, LogLevel::Error, "evk", "vkCreatePipelineLayout failed");
        vkDestroyShaderModule(device_, vertModule, nullptr);
        vkDestroyShaderModule(device_, fragModule, nullptr);
        return false;
    }

    // 最后把上面所有阶段状态总装进 VkGraphicsPipelineCreateInfo。
    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = stages;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDynamicState = &dynamicState;
    // layout / renderPass / subpass 把管线绑定到接口签名和第 0 个 subpass；
    // 管线与 render pass 的兼容性在创建期就会被校验。
    pipelineInfo.layout = pipelineLayout_;
    pipelineInfo.renderPass = renderPass_;
    pipelineInfo.subpass = 0;

    // vkCreateGraphicsPipelines 是最贵的调用之一：这里触发 SPIR-V 到 GPU 指令的真正编译；
    // 第二参数是管线缓存（可加速重复创建），VK_NULL_HANDLE 表示不用。
    if (vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &graphicsPipeline_) != VK_SUCCESS) {
        logMessage(platform_, LogLevel::Error, "evk", "vkCreateGraphicsPipelines failed");
        vkDestroyShaderModule(device_, vertModule, nullptr);
        vkDestroyShaderModule(device_, fragModule, nullptr);
        return false;
    }

    // 管线建完后 SPIR-V 已被"消化"，shader module 可即刻销毁，这是规范允许的用法。
    vkDestroyShaderModule(device_, vertModule, nullptr);
    vkDestroyShaderModule(device_, fragModule, nullptr);
    return true;
}

bool Renderer::createFramebuffers() {
    // 每个 swapchain 图像视图都对应一个 framebuffer。
    // 每张图像配一个；渲染时按 acquire 到的 imageIndex 选用对应的那一个。
    swapchainFramebuffers_.resize(swapchainImageViews_.size());
    for (size_t i = 0; i < swapchainImageViews_.size(); ++i) {
        // framebuffer 把 render pass 声明的"抽象附件"绑定到具体 imageView；
        // 数组顺序要与 render pass 的附件下标一一对应。
        VkImageView attachments[] = {swapchainImageViews_[i]};

        VkFramebufferCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        // 指定兼容的 render pass（通常就是同一个）。
        createInfo.renderPass = renderPass_;
        createInfo.attachmentCount = 1;
        createInfo.pAttachments = attachments;
        // 宽高按 swapchain 尺寸；layers 非立体渲染恒为 1。
        createInfo.width = swapchainExtent_.width;
        createInfo.height = swapchainExtent_.height;
        createInfo.layers = 1;

        if (vkCreateFramebuffer(device_, &createInfo, nullptr, &swapchainFramebuffers_[i]) != VK_SUCCESS) {
            logMessage(platform_, LogLevel::Error, "evk", "vkCreateFramebuffer failed");
            return false;
        }
    }
    return true;
}

bool Renderer::createCommandPool() {
    // command pool 允许命令缓冲每帧重置并重新录制。
    // 命令池要从某个队列族分配命令缓冲：先再查一次图形族索引。
    uint32_t qCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &qCount, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &qCount, qf.data());

    // 找第一个带 GRAPHICS 位的族。
    uint32_t graphicsFamily = 0;
    for (uint32_t i = 0; i < qCount; ++i) {
        if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            graphicsFamily = i;
            break;
        }
    }

    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    // RESET_COMMAND_BUFFER_BIT：允许单独 vkResetCommandBuffer 重录某条缓冲；
    // 我们每帧都要重录命令，必须开这个位（否则只能整池重置）。
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    // 池绑定队列族：从这里分配的命令缓冲只能提交到该族的队列。
    poolInfo.queueFamilyIndex = graphicsFamily;

    if (vkCreateCommandPool(device_, &poolInfo, nullptr, &commandPool_) != VK_SUCCESS) {
        logMessage(platform_, LogLevel::Error, "evk", "vkCreateCommandPool failed");
        return false;
    }
    return true;
}

bool Renderer::createVertexBuffer() {
    // 动态顶点缓冲：容量 kVertexCapacity 个 UiVertex，每帧按需上传。
    // HOST_VISIBLE|HOST_COHERENT 内存，省去 staging buffer。
    // 一次性按上限分配，避免每帧重新分配显存。
    VkDeviceSize bufferSize = sizeof(ui::UiVertex) * kVertexCapacity;

    // VkBuffer 只是"一块多大、干什么用"的描述对象，本身不带内存。
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = bufferSize;
    // usage = VERTEX_BUFFER：稍后要用 vkCmdBindVertexBuffers 绑它。
    bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    // 只有图形队列用它，EXCLUSIVE 独占即可。
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(device_, &bufferInfo, nullptr, &vertexBuffer_) != VK_SUCCESS) {
        logMessage(platform_, LogLevel::Error, "evk", "vkCreateBuffer failed");
        return false;
    }

    // 创建后问驱动：这块 buffer 需要多大、什么对齐、允许哪些内存类型。
    VkMemoryRequirements memReq;
    vkGetBufferMemoryRequirements(device_, vertexBuffer_, &memReq);

    // Vulkan 不自动配内存：要显式 vkAllocateMemory 再 bind；
    // 内存类型必须同时满足 buffer 的要求（memoryTypeBits）与我们的属性要求。
    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;
    // HOST_VISIBLE：CPU 可直接 map 写入；HOST_COHERENT：写完 GPU 立即可见、免手动 flush，
    // 每帧上传少量顶点时最省事，可以省掉 staging buffer。
    allocInfo.memoryTypeIndex = findMemoryType(memReq.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    // 按需求尺寸和选定类型分配设备内存。
    if (vkAllocateMemory(device_, &allocInfo, nullptr, &vertexBufferMemory_) != VK_SUCCESS) {
        logMessage(platform_, LogLevel::Error, "evk", "vkAllocateMemory failed");
        return false;
    }

    // 把内存绑到 buffer 上（偏移 0），此后 buffer 才真正可用。
    vkBindBufferMemory(device_, vertexBuffer_, vertexBufferMemory_, 0);
    return true;
}

void Renderer::uploadVertices(const ui::UiVertex* data, uint32_t count) {
    // 超出预分配容量就截断并告警：宁可丢一部分 UI，也不能越界写显存。
    if (count > kVertexCapacity) {
        logMessage(platform_, LogLevel::Warn, "evk",
            "vertex count %u exceeds capacity %u, truncated", count, kVertexCapacity);
        count = kVertexCapacity;
    }
    // 空帧直接返回，连 map 都省了。
    if (count == 0) {
        return;
    }
    // map → memcpy → unmap 三步：HOST_VISIBLE 内存可映射到 CPU 地址空间直接写；
    // HOST_COHERENT 保证拷完 GPU 就能读到，无需手动 flush。
    void* mapped = nullptr;
    vkMapMemory(device_, vertexBufferMemory_, 0, sizeof(ui::UiVertex) * count, 0, &mapped);
    std::memcpy(mapped, data, sizeof(ui::UiVertex) * count);
    vkUnmapMemory(device_, vertexBufferMemory_);
}

bool Renderer::createCommandBuffers() {
    // 每个 in-flight 帧配一个主命令缓冲，提交逻辑更简单。
    // kMaxFramesInFlight = 2：CPU 最多领先 GPU 两帧，每帧一条缓冲互不干扰。
    commandBuffers_.resize(kMaxFramesInFlight);

    // 从命令池一次性批量分配。
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = commandPool_;
    // PRIMARY：可直接提交到队列（SECONDARY 只能被 primary 缓冲调用）。
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = static_cast<uint32_t>(commandBuffers_.size());

    if (vkAllocateCommandBuffers(device_, &allocInfo, commandBuffers_.data()) != VK_SUCCESS) {
        logMessage(platform_, LogLevel::Error, "evk", "vkAllocateCommandBuffers failed");
        return false;
    }
    return true;
}

bool Renderer::createSyncObjects() {
    // 双缓冲能让 CPU 和 GPU 有一点重叠，但不会无限堆积帧。
    // 两类同步原语：semaphore 做 GPU 内部阶段间等待（acquire → 渲染 → 呈现），
    // fence 做 CPU 等 GPU；每帧各一套，互不打架。
    imageAvailableSemaphores_.resize(kMaxFramesInFlight);
    renderFinishedSemaphores_.resize(kMaxFramesInFlight);
    inFlightFences_.resize(kMaxFramesInFlight);

    // semaphore 创建无需标志位，初始为未触发状态。
    VkSemaphoreCreateInfo semInfo{};
    semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    // fence 建成 SIGNALED 初始态：第一帧 render() 的 vkWaitForFences 能立即通过，
    // 否则会永久等在一个从没人 signal 过的 fence 上。
    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    // 逐帧创建两个 semaphore + 一个 fence。
    for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
        if (vkCreateSemaphore(device_, &semInfo, nullptr, &imageAvailableSemaphores_[i]) != VK_SUCCESS ||
            vkCreateSemaphore(device_, &semInfo, nullptr, &renderFinishedSemaphores_[i]) != VK_SUCCESS ||
            vkCreateFence(device_, &fenceInfo, nullptr, &inFlightFences_[i]) != VK_SUCCESS) {
            logMessage(platform_, LogLevel::Error, "evk", "createSyncObjects failed");
            return false;
        }
    }
    return true;
}

void Renderer::cleanupSwapchain() {
    // 这些资源和 swapchain 的格式、尺寸绑定，所以要一起重建。
    // framebuffer 依赖 imageView 与尺寸，先销。
    for (auto fb : swapchainFramebuffers_) {
        vkDestroyFramebuffer(device_, fb, nullptr);
    }
    swapchainFramebuffers_.clear();

    // 管线、布局、renderPass 都按 swapchain 格式/尺寸配置，一并重建。
    vkDestroyPipeline(device_, graphicsPipeline_, nullptr);
    vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
    vkDestroyRenderPass(device_, renderPass_, nullptr);

    // imageView 与 swapchain 本体最后销。
    for (auto view : swapchainImageViews_) {
        vkDestroyImageView(device_, view, nullptr);
    }
    swapchainImageViews_.clear();

    vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
}

void Renderer::recreateSwapchain() {
    // 先等 GPU 空闲，释放依赖 swapchain 的状态，再从头重建。
    // 重建期间不能有任何帧在飞：先 vkDeviceWaitIdle 让 GPU 完全空闲。
    vkDeviceWaitIdle(device_);
    // 然后按 initialize() 里"依赖 swapchain 的那一段"原序重建；
    // 命令池、顶点缓冲、同步对象与尺寸无关，不用动。
    cleanupSwapchain();
    createSwapchain();
    createImageViews();
    createRenderPass();
    createGraphicsPipeline();
    createFramebuffers();
}

void Renderer::setSize(uint32_t width, uint32_t height) {
    // 真正的重建会在 render() 里做，那时更安全。
    // 只记尺寸并置标志位：本函数可能在渲染途中被平台线程调用，
    // 直接重建会撞上在飞的帧，所以延迟到 render() 的安全点再做。
    width_ = width;
    height_ = height;
    framebufferResized_ = true;
}

void Renderer::requestSwapchainRebuild() {
    // 平台知道 surface 变了，但还不知道准确尺寸时用这个。
    // 同样只置标志位，尺寸留给 render() 重建时重新查询。
    framebufferResized_ = true;
}

bool Renderer::render(const ui::Canvas& canvas) {
    // 尺寸变化（旋转等）时先重建 swapchain 再画帧：否则本帧会按旧 swapchainExtent_
    // 投影、画进旧尺寸的图像，呈现出去的是变形/裁剪的画面；本渲染器是按需模型，
    // 之后没有新帧覆盖，错误会一直挂到下次事件。
    //
    // 整个画帧流程包在有限次重试里：surface 刚变化时驱动状态尚未落定——
    // 重建查到的 surface 能力可能还是旧尺寸（滞后一拍），acquire/present 也会报
    // OUT_OF_DATE / SUBOPTIMAL。按需渲染没有"下一帧"来自我修正（持续渲染的引擎
    // 靠下一帧自然收敛），所以本帧内立刻重建重画，直到交换链与 surface 匹配。
    for (uint32_t attempt = 0; attempt < kMaxFrameAttempts; ++attempt) {
        if (framebufferResized_) {
            framebufferResized_ = false;
            recreateSwapchain();
        }
        // 每帧流程：等待 fence、获取图像、录制命令、提交执行、最后呈现。
        // ① 等本帧槽位的 fence：确保它上一轮的渲染已完成，
        // 这把 CPU 领先 GPU 的帧数限制在 kMaxFramesInFlight 以内，防止无限堆积。
        vkWaitForFences(device_, 1, &inFlightFences_[currentFrame_], VK_TRUE, UINT64_MAX);

        // ② acquire：向交换链申请下一张可写图像；图像就绪时 GPU 会 signal imageAvailableSemaphore。
        uint32_t imageIndex = 0;
        VkResult result = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX,
            imageAvailableSemaphores_[currentFrame_], VK_NULL_HANDLE, &imageIndex);

        // 这一帧还没来得及渲染，swapchain 就已经失效了。
        // OUT_OF_DATE 说明交换链与 surface 已不匹配（尺寸/旋转变化）：
        // 立刻重建并重试本帧（旧实现直接放弃本帧，按需模型下画面会整帧丢失）。
        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            recreateSwapchain();
            continue;
        // SUBOPTIMAL 也算拿到图像（只是与 surface 不再完全匹配），照常渲染，present 后再重建。
        } else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
            logMessage(platform_, LogLevel::Error, "evk", "vkAcquireNextImageKHR failed: %d", result);
            return false;
        }

        // acquire 成功后先把本帧顶点写进动态缓冲，再录命令。空 canvas 跳过上传，
        // 命令录制阶段也不会有 draw，只剩清屏帧。
        // ③ 上传顶点：赶在命令录制前把数据写进 HOST_VISIBLE 缓冲。
        if (!canvas.vertices().empty()) {
            uploadVertices(canvas.vertices().data(), static_cast<uint32_t>(canvas.vertices().size()));
        }

        // ④ reset fence 必须在确认本帧一定会提交之后做：若提前 reset 又中途 return，
        // 下一帧 vkWaitForFences 会永远等不到 signal（死锁）。
        vkResetFences(device_, 1, &inFlightFences_[currentFrame_]);

        // 命令缓冲不能"局部修改"，每帧 reset 后整段重录。
        vkResetCommandBuffer(commandBuffers_[currentFrame_], 0);
        recordCommandBuffer(commandBuffers_[currentFrame_], imageIndex, canvas);

        // ⑤ 组装提交信息：等待条件 + 命令缓冲 + 完成信号。
        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

        // waitSemaphore：等 acquire 完成（图像真归我们了）才允许写它。
        VkSemaphore waitSemaphores[] = {imageAvailableSemaphores_[currentFrame_]};
        // waitStages = COLOR_ATTACHMENT_OUTPUT：只在"写颜色"这个管线阶段前等待，
        // 之前的阶段（如顶点着色）可以提前跑，提高并行度。
        VkPipelineStageFlags waitStages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
        submitInfo.waitSemaphoreCount = 1;
        submitInfo.pWaitSemaphores = waitSemaphores;
        submitInfo.pWaitDstStageMask = waitStages;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &commandBuffers_[currentFrame_];

        // signalSemaphore：渲染完成后 signal renderFinished，交给 present 等。
        VkSemaphore signalSemaphores[] = {renderFinishedSemaphores_[currentFrame_]};
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = signalSemaphores;

        // 提交到图形队列；末尾的 fence 在这批命令全部执行完后由 GPU 置位，
        // 正是步骤①等待的那个信号。
        if (vkQueueSubmit(graphicsQueue_, 1, &submitInfo, inFlightFences_[currentFrame_]) != VK_SUCCESS) {
            logMessage(platform_, LogLevel::Error, "evk", "vkQueueSubmit failed");
            return false;
        }

        // ⑥ present：把渲染好的图像交回交换链排队上屏。
        VkPresentInfoKHR presentInfo{};
        presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        // present 要等 renderFinished：渲染没完成不能显示，顺序由 semaphore 在 GPU 上保证。
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = signalSemaphores;
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = &swapchain_;
        presentInfo.pImageIndices = &imageIndex;

        result = vkQueuePresentKHR(presentQueue_, &presentInfo);
        // present 阶段也可能提示 surface 和 swapchain 已经不匹配。
        // OUT_OF_DATE / SUBOPTIMAL 说明本帧是按不匹配的交换链画的（典型的如旋转后
        // 第一次重建拿到了旧尺寸）：置标志位并重试，下一趟循环开头重建、用同一
        // canvas 重画，用户看到的直接就是修正后的画面。
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
            framebufferResized_ = true;
            continue;
        } else if (result != VK_SUCCESS) {
            logMessage(platform_, LogLevel::Error, "evk", "vkQueuePresentKHR failed: %d", result);
            return false;
        }

        // ⑦ 轮转帧槽位 0→1→0→1：下一帧换用另一套 semaphore / fence / 命令缓冲。
        currentFrame_ = (currentFrame_ + 1) % kMaxFramesInFlight;
        return true;
    }

    // 重试耗尽（surface 持续变化中，如快速连续旋转）：放弃本帧不算失败，
    // 后续尺寸事件还会触发渲染，届时继续收敛。
    logMessage(platform_, LogLevel::Warn, "evk",
               "render: swapchain still out of date after %u attempts, frame skipped",
               kMaxFrameAttempts);
    return true;
}

void Renderer::recordCommandBuffer(VkCommandBuffer cmd, uint32_t imageIndex, const ui::Canvas& canvas) {
    // 把一帧的绘制命令录进可复用的主命令缓冲。
    // begin 开始录制：缓冲进入"录制态"，之后的 vkCmd* 调用都被录进去。
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(cmd, &beginInfo);

    // render pass begin 信息：用哪个 pass、渲染到哪个 framebuffer、范围多大。
    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = renderPass_;
    // framebuffer 按 acquire 到的 imageIndex 选：画到将要上屏的那张图。
    renderPassInfo.framebuffer = swapchainFramebuffers_[imageIndex];
    // renderArea 限定渲染区域；全屏渲染就是整个 swapchain 尺寸。
    renderPassInfo.renderArea.offset = {0, 0};
    renderPassInfo.renderArea.extent = swapchainExtent_;

    // 清屏色：深蓝灰 (0.06, 0.06, 0.09)，作用于 render pass 里 loadOp=CLEAR 的附件。
    VkClearValue clearColor = {{{0.06f, 0.06f, 0.09f, 1.0f}}};
    renderPassInfo.clearValueCount = 1;
    renderPassInfo.pClearValues = &clearColor;

    // INLINE 表示命令直接录在这条 primary 缓冲里（另一种是执行 secondary 缓冲）。
    vkCmdBeginRenderPass(cmd, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

    // 绑定图形管线：之后的 draw 都用这套着色器与固定功能状态。
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, graphicsPipeline_);

    // 按当前 swapchain 尺寸组一份 viewport（NDC 到像素的映射）。
    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(swapchainExtent_.width);
    viewport.height = static_cast<float>(swapchainExtent_.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    // 管线把 viewport 和 scissor 设成动态状态，所以每帧都要重新设置。
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    // 绑定顶点缓冲到槽位 0（对应管线 vertex input 的 binding 0），偏移 0。
    VkBuffer vertexBuffers[] = {vertexBuffer_};
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(cmd, 0, 1, vertexBuffers, offsets);

    // 布局/视觉尺寸（app 坐标空间 = 用户实际看到的方向）。
    const float bufferW = static_cast<float>(swapchainExtent_.width);
    const float bufferH = static_cast<float>(swapchainExtent_.height);
    // preTransform 为 90/270 度时，createSwapchain() 已保证 buffer 使用旋转前
    // 尺寸；这里把宽高换回用户实际看到的布局尺寸，并补偿呈现变换。
    const bool rotate90or270 = (surfaceTransform_ == VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR ||
                                surfaceTransform_ == VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR);
    const bool compensate = surfaceTransform_ != VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    const bool dimsSwapped = rotate90or270;
    const float visualW = dimsSwapped ? bufferH : bufferW;
    const float visualH = dimsSwapped ? bufferW : bufferH;

    // 像素坐标 → NDC 的正交投影（原点在左上角，y 向下，基于视觉尺寸）。
    // 注意 glm::ortho 第三/四参数是 bottom/top：Vulkan 正 viewport 高度下
    // NDC +y 朝 framebuffer 下方，要 y 向下需传 bottom=0、top=height。
    // 效果：窗口左上映射到 NDC (-1,-1)、右下映射到 (1,1)；z 用不到，给 [-1,1] 即可。
    glm::mat4 mvp = glm::ortho(0.0f, visualW, 0.0f, visualH, -1.0f, 1.0f);

    // 需要补偿时（见上）：呈现时系统把 buffer 按 currentTransform 旋转上屏，
    // 这里预先把 NDC 反向旋转，上屏后内容回到正立方向。
    // 90/180/270 在 NDC 平面内是精确换轴（glm::mat4 按列填充）：
    if (compensate && surfaceTransform_ == VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR) {
        // (x, y) -> (-y, x)
        const glm::mat4 rot( 0.0f, 1.0f, 0.0f, 0.0f,
                            -1.0f, 0.0f, 0.0f, 0.0f,
                             0.0f, 0.0f, 1.0f, 0.0f,
                             0.0f, 0.0f, 0.0f, 1.0f);
        mvp = rot * mvp;
    } else if (compensate && surfaceTransform_ == VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR) {
        // (x, y) -> (-x, -y)
        const glm::mat4 rot(-1.0f,  0.0f, 0.0f, 0.0f,
                             0.0f, -1.0f, 0.0f, 0.0f,
                             0.0f,  0.0f, 1.0f, 0.0f,
                             0.0f,  0.0f, 0.0f, 1.0f);
        mvp = rot * mvp;
    } else if (compensate && surfaceTransform_ == VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR) {
        // (x, y) -> (y, -x)
        const glm::mat4 rot(0.0f, -1.0f, 0.0f, 0.0f,
                            1.0f,  0.0f, 0.0f, 0.0f,
                            0.0f,  0.0f, 1.0f, 0.0f,
                            0.0f,  0.0f, 0.0f, 1.0f);
        mvp = rot * mvp;
    }

    // 逐批绘制：每批共享一个 clip，裁剪矩形取 clip 与 swapchain 的交集。
    for (const auto& batch : canvas.batches()) {
        // 空批直接跳过，避免无意义的 draw。
        if (batch.vertexCount == 0) {
            continue;
        }
        // 把正交投影矩阵经 push constant 发给顶点着色器；每批推一次（内容相同），开销可忽略。
        vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT,
                           0, sizeof(mvp), &mvp);

        // clip 与屏幕矩形求交（在视觉空间进行，与 app 布局坐标一致）。
        ui::Rect clip = ui::Rect::intersect(batch.clip, {0.0f, 0.0f, visualW, visualH});

        // 需要补偿时，把视觉空间的 clip 旋转到 buffer 像素空间（与上面的 NDC 补偿同向）：
        // 90°: (x,y)->(H-y-h, x)，宽高互换；180°: 两轴各自翻转；
        // 270°: (x,y)->(y, W-x-w)，是 90° 的逆。
        // 无需补偿时 buffer 空间即视觉空间，直接用 clip。
        ui::Rect bclip;
        if (compensate && surfaceTransform_ == VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR) {
            bclip = {visualH - clip.y - clip.h, clip.x, clip.h, clip.w};
        } else if (compensate && surfaceTransform_ == VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR) {
            bclip = {visualW - clip.x - clip.w, visualH - clip.y - clip.h, clip.w, clip.h};
        } else if (compensate && surfaceTransform_ == VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR) {
            bclip = {clip.y, visualW - clip.x - clip.w, clip.h, clip.w};
        } else {
            bclip = clip;
        }

        // 转 int32 时 clamp：offset 不小于 0，且 offset+extent 不超出 swapchain。
        // 左上角 clamp 到 >= 0：scissor 的 offset 不能为负。
        int32_t ox = std::max(0, static_cast<int32_t>(bclip.x));
        int32_t oy = std::max(0, static_cast<int32_t>(bclip.y));
        // 右下角 clamp 到 swapchain 尺寸内，再减去 offset 得到宽高。
        int32_t ex = std::min(static_cast<int32_t>(bclip.x + bclip.w),
                              static_cast<int32_t>(swapchainExtent_.width)) - ox;
        int32_t ey = std::min(static_cast<int32_t>(bclip.y + bclip.h),
                              static_cast<int32_t>(swapchainExtent_.height)) - oy;
        // 交集为空（批完全在屏外）就跳过。
        if (ex <= 0 || ey <= 0) {
            continue;
        }

        // 转成 VkRect2D（offset 有符号、extent 无符号）并设为动态 scissor：
        // 这就是 UI 裁剪（clip）的硬件实现。
        VkRect2D scissor{};
        scissor.offset = {ox, oy};
        scissor.extent = {static_cast<uint32_t>(ex), static_cast<uint32_t>(ey)};
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        // 画这一批：firstVertex 定位到该批在总顶点数组里的起始偏移，
        // 所有批共享同一块顶点缓冲，靠 firstVertex 分段。
        vkCmdDraw(cmd, batch.vertexCount, 1, batch.firstVertex, 0);
    }

    // 结束 render pass：同时触发附件布局转换到 PRESENT_SRC_KHR。
    vkCmdEndRenderPass(cmd);

    // 结束录制，缓冲才可提交。
    vkEndCommandBuffer(cmd);
}

uint32_t Renderer::findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) {
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

} // namespace evk
