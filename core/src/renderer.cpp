#include "evk/renderer.h"
#include "evk/assets/ui_shaders.h"

#include <algorithm>
#include <cstring>
#include <vector>

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace evk {

// ============================================================================
// 辅助函数
// ============================================================================

static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT /*type*/,
    const VkDebugUtilsMessengerCallbackDataEXT* data,
    void* userData) {

    auto* platform = static_cast<IPlatform*>(userData);
    // 将 Vulkan 校验消息严重级别映射到渲染器的日志级别。
    LogLevel level = LogLevel::Debug;
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        level = LogLevel::Error;
    } else if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        level = LogLevel::Warn;
    } else if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT) {
        level = LogLevel::Info;
    }
    logMessage(platform, level, "evk-validation", "%s", data->pMessage);
    return VK_FALSE;
}

static bool checkValidationLayerSupport(const std::vector<const char*>& layers) {
    uint32_t count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> available(count);
    vkEnumerateInstanceLayerProperties(&count, available.data());

    for (const char* layer : layers) {
        bool found = false;
        for (const auto& props : available) {
            if (std::strcmp(props.layerName, layer) == 0) {
                found = true;
                break;
            }
        }
        if (!found) return false;
    }
    return true;
}

static bool hasExtension(const std::vector<VkExtensionProperties>& props, const char* name) {
    for (const auto& p : props) {
        if (std::strcmp(p.extensionName, name) == 0) return true;
    }
    return false;
}

// ============================================================================
// 渲染器实现
// ============================================================================

Renderer::Renderer(IPlatform* platform) : platform_(platform) {}

Renderer::~Renderer() {
    shutdown();
}

bool Renderer::initialize() {
    // 按依赖顺序搭建 Vulkan 栈。每一步都依赖前一步，所以任何一环失败都会立刻停止启动。
    if (!createInstance()) return false;
    if (!createSurface()) return false;
    if (!pickPhysicalDevice()) return false;
    if (!createLogicalDevice()) return false;
    if (!createSwapchain()) return false;
    if (!createImageViews()) return false;
    if (!createRenderPass()) return false;
    if (!createGraphicsPipeline()) return false;
    if (!createFramebuffers()) return false;
    if (!createCommandPool()) return false;
    if (!createVertexBuffer()) return false;
    if (!createCommandBuffers()) return false;
    if (!createSyncObjects()) return false;
    return true;
}

void Renderer::shutdown() {
    // 按相反顺序释放资源。设备必须先空闲，才能销毁任何仍在飞行中的对象。
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);

        for (auto fence : inFlightFences_) vkDestroyFence(device_, fence, nullptr);
        for (auto sem : renderFinishedSemaphores_) vkDestroySemaphore(device_, sem, nullptr);
        for (auto sem : imageAvailableSemaphores_) vkDestroySemaphore(device_, sem, nullptr);

        vkDestroyBuffer(device_, vertexBuffer_, nullptr);
        vkFreeMemory(device_, vertexBufferMemory_, nullptr);

        vkDestroyCommandPool(device_, commandPool_, nullptr);

        cleanupSwapchain();

        vkDestroyDevice(device_, nullptr);
    }

    if (instance_ != VK_NULL_HANDLE) {
        if (debugMessenger_ != VK_NULL_HANDLE) {
            // 调试消息器归实例所有，所以要先销毁它。
            auto func = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
            if (func) func(instance_, debugMessenger_, nullptr);
        }
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
        vkDestroyInstance(instance_, nullptr);
    }

    inFlightFences_.clear();
    renderFinishedSemaphores_.clear();
    imageAvailableSemaphores_.clear();
    commandBuffers_.clear();
    swapchainFramebuffers_.clear();
    swapchainImageViews_.clear();
    swapchainImages_.clear();

    device_ = VK_NULL_HANDLE;
    physicalDevice_ = VK_NULL_HANDLE;
    surface_ = VK_NULL_HANDLE;
    instance_ = VK_NULL_HANDLE;
}

bool Renderer::createInstance() {
    // 实例创建主要是在探测能力：先选 surface 扩展，加载器支持时再加调试扩展。
    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "estarx_vulkan";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "estarx_vulkan";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_0;

    // 先放入通用 surface 扩展，再按平台和调试能力补充。
    std::vector<const char*> extensions = {
        VK_KHR_SURFACE_EXTENSION_NAME,
#ifdef __ANDROID__
        VK_KHR_ANDROID_SURFACE_EXTENSION_NAME,
#endif
    };

    uint32_t extCount = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &extCount, nullptr);
    std::vector<VkExtensionProperties> availableExts(extCount);
    vkEnumerateInstanceExtensionProperties(nullptr, &extCount, availableExts.data());

    bool useDebugUtils = hasExtension(availableExts, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    if (useDebugUtils) {
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

    // 校验层是可选的，只在构建配置要求时启用。
    std::vector<const char*> layers;
#ifdef EVK_ENABLE_VALIDATION
    layers.push_back("VK_LAYER_KHRONOS_validation");
    if (!checkValidationLayerSupport(layers)) {
        layers.clear();
    }
#endif

    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();
    createInfo.enabledLayerCount = static_cast<uint32_t>(layers.size());
    createInfo.ppEnabledLayerNames = layers.data();

    if (vkCreateInstance(&createInfo, nullptr, &instance_) != VK_SUCCESS) {
        logMessage(platform_, LogLevel::Error, "evk", "vkCreateInstance failed");
        return false;
    }

    if (useDebugUtils && !layers.empty()) {
        VkDebugUtilsMessengerCreateInfoEXT debugCreateInfo{};
        debugCreateInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        debugCreateInfo.messageSeverity =
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        debugCreateInfo.messageType =
            VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        debugCreateInfo.pfnUserCallback = debugCallback;
        debugCreateInfo.pUserData = platform_;

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
    return platform_->createVulkanSurface(instance_, &surface_);
}

bool Renderer::pickPhysicalDevice() {
    // 只保留既能绘制到 surface、又能 present swapchain 图像的设备。
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance_, &count, nullptr);
    if (count == 0) {
        logMessage(platform_, LogLevel::Error, "evk", "no Vulkan-capable GPU found");
        return false;
    }

    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance_, &count, devices.data());

    for (const auto& dev : devices) {
        uint32_t queueFamilyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &queueFamilyCount, nullptr);
        std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &queueFamilyCount, queueFamilies.data());

        int graphicsFamily = -1;
        int presentFamily = -1;
        // 图形队列和呈现队列可能属于不同的队列族。
        for (uint32_t i = 0; i < queueFamilyCount; ++i) {
            if (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                graphicsFamily = static_cast<int>(i);
            }
            VkBool32 presentSupport = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(dev, i, surface_, &presentSupport);
            if (presentSupport) {
                presentFamily = static_cast<int>(i);
            }
        }

        uint32_t extCount = 0;
        vkEnumerateDeviceExtensionProperties(dev, nullptr, &extCount, nullptr);
        std::vector<VkExtensionProperties> exts(extCount);
        vkEnumerateDeviceExtensionProperties(dev, nullptr, &extCount, exts.data());
        bool swapchainSupported = hasExtension(exts, VK_KHR_SWAPCHAIN_EXTENSION_NAME);

        if (graphicsFamily >= 0 && presentFamily >= 0 && swapchainSupported) {
            physicalDevice_ = dev;
            return true;
        }
    }

    logMessage(platform_, LogLevel::Error, "evk", "no suitable GPU found");
    return false;
}

bool Renderer::createLogicalDevice() {
    // 创建最小化的逻辑设备，并申请所需的队列族。
    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &queueFamilyCount, queueFamilies.data());

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
    std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
    std::vector<uint32_t> uniqueFamilies = {static_cast<uint32_t>(graphicsFamily)};
    if (static_cast<uint32_t>(presentFamily) != static_cast<uint32_t>(graphicsFamily)) {
        uniqueFamilies.push_back(static_cast<uint32_t>(presentFamily));
    }

    float queuePriority = 1.0f;
    for (uint32_t family : uniqueFamilies) {
        VkDeviceQueueCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        info.queueFamilyIndex = family;
        info.queueCount = 1;
        info.pQueuePriorities = &queuePriority;
        queueCreateInfos.push_back(info);
    }

    VkPhysicalDeviceFeatures features{};

    const char* deviceExtensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};

    VkDeviceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
    createInfo.pQueueCreateInfos = queueCreateInfos.data();
    createInfo.pEnabledFeatures = &features;
    createInfo.enabledExtensionCount = 1;
    createInfo.ppEnabledExtensionNames = deviceExtensions;

    if (vkCreateDevice(physicalDevice_, &createInfo, nullptr, &device_) != VK_SUCCESS) {
        logMessage(platform_, LogLevel::Error, "evk", "vkCreateDevice failed");
        return false;
    }

    vkGetDeviceQueue(device_, static_cast<uint32_t>(graphicsFamily), 0, &graphicsQueue_);
    vkGetDeviceQueue(device_, static_cast<uint32_t>(presentFamily), 0, &presentQueue_);
    return true;
}

bool Renderer::createSwapchain() {
    // 选择适合当前窗口的 surface 格式、present 模式和尺寸。
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice_, surface_, &caps);

    // 先查询 surface 能力，再决定具体的 swapchain 参数。
    uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_, surface_, &formatCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    if (formatCount > 0) {
        vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice_, surface_, &formatCount, formats.data());
    }

    uint32_t presentModeCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice_, surface_, &presentModeCount, nullptr);
    std::vector<VkPresentModeKHR> presentModes(presentModeCount);
    if (presentModeCount > 0) {
        vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice_, surface_, &presentModeCount, presentModes.data());
    }

    VkSurfaceFormatKHR surfaceFormat = formats[0];
    for (const auto& f : formats) {
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            surfaceFormat = f;
            break;
        }
    }

    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
    for (const auto& pm : presentModes) {
        // 驱动支持时优先用 Mailbox，可降低显示延迟。
        if (pm == VK_PRESENT_MODE_MAILBOX_KHR) {
            presentMode = pm;
            break;
        }
    }

    VkExtent2D extent;
    // 如果 surface 固定了尺寸，Vulkan 会直接给出精确的 extent。
    if (caps.currentExtent.width != UINT32_MAX) {
        extent = caps.currentExtent;
    } else {
        platform_->getSurfaceSize(&width_, &height_);
        extent.width = std::max(caps.minImageExtent.width, std::min(caps.maxImageExtent.width, width_));
        extent.height = std::max(caps.minImageExtent.height, std::min(caps.maxImageExtent.height, height_));
    }

    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount) {
        imageCount = caps.maxImageCount;
    }

    VkSwapchainCreateInfoKHR createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    createInfo.surface = surface_;
    createInfo.minImageCount = imageCount;
    createInfo.imageFormat = surfaceFormat.format;
    createInfo.imageColorSpace = surfaceFormat.colorSpace;
    createInfo.imageExtent = extent;
    createInfo.imageArrayLayers = 1;
    createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    createInfo.preTransform = caps.currentTransform;
    createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    createInfo.presentMode = presentMode;
    createInfo.clipped = VK_TRUE;
    createInfo.oldSwapchain = VK_NULL_HANDLE;

    uint32_t queueFamilyIndices[2]; // 下面填充

    // 重新取一次队列族索引，确保 swapchain 的共享模式正确。
    uint32_t qCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &qCount, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &qCount, qf.data());
    uint32_t gFamily = 0, pFamily = 0;
    for (uint32_t i = 0; i < qCount; ++i) {
        if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) gFamily = i;
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(physicalDevice_, i, surface_, &present);
        if (present) pFamily = i;
    }

    // 如果图形和呈现队列族分开，swapchain 图像需要并发共享。
    if (gFamily != pFamily) {
        queueFamilyIndices[0] = gFamily;
        queueFamilyIndices[1] = pFamily;
        createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        createInfo.queueFamilyIndexCount = 2;
        createInfo.pQueueFamilyIndices = queueFamilyIndices;
    } else {
        createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    if (vkCreateSwapchainKHR(device_, &createInfo, nullptr, &swapchain_) != VK_SUCCESS) {
        logMessage(platform_, LogLevel::Error, "evk", "vkCreateSwapchainKHR failed");
        return false;
    }

    vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount, nullptr);
    swapchainImages_.resize(imageCount);
    vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount, swapchainImages_.data());

    swapchainImageFormat_ = surfaceFormat.format;
    swapchainExtent_ = extent;
    return true;
}

bool Renderer::createImageViews() {
    // 每个 swapchain 图像都要创建一个 2D 颜色视图。
    swapchainImageViews_.resize(swapchainImages_.size());
    for (size_t i = 0; i < swapchainImages_.size(); ++i) {
        VkImageViewCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        createInfo.image = swapchainImages_[i];
        createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        createInfo.format = swapchainImageFormat_;
        createInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        createInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        createInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        createInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
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
    VkAttachmentDescription colorAttachment{};
    colorAttachment.format = swapchainImageFormat_;
    colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference colorRef{};
    colorRef.attachment = 0;
    colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;

    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.srcAccessMask = 0;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

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
    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = codeSize;
    createInfo.pCode = code;

    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device_, &createInfo, nullptr, &module) != VK_SUCCESS) {
        logMessage(platform_, LogLevel::Error, "evk", "vkCreateShaderModule failed");
        return VK_NULL_HANDLE;
    }
    return module;
}

bool Renderer::createGraphicsPipeline() {
    // 这个示例的管线是固定的。viewport 和 scissor 保持动态，所以缩放时只需重录命令缓冲。
    VkShaderModule vertModule = createShaderModule(
        assets::ui_vert_spv, sizeof(assets::ui_vert_spv));
    VkShaderModule fragModule = createShaderModule(
        assets::ui_frag_spv, sizeof(assets::ui_frag_spv));

    if (vertModule == VK_NULL_HANDLE || fragModule == VK_NULL_HANDLE) {
        return false;
    }

    VkPipelineShaderStageCreateInfo vertStage{};
    vertStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vertStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertStage.module = vertModule;
    vertStage.pName = "main";

    VkPipelineShaderStageCreateInfo fragStage{};
    fragStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fragStage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragStage.module = fragModule;
    fragStage.pName = "main";

    VkPipelineShaderStageCreateInfo stages[] = {vertStage, fragStage};

    // 顶点数据是 UiVertex 交错布局：先像素坐标(vec2)，再 RGBA 颜色(vec4)。
    VkVertexInputBindingDescription bindingDesc{};
    bindingDesc.binding = 0;
    bindingDesc.stride = sizeof(float) * 6;
    bindingDesc.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attrs[2];
    attrs[0].binding = 0;
    attrs[0].location = 0;
    attrs[0].format = VK_FORMAT_R32G32_SFLOAT;
    attrs[0].offset = 0;
    attrs[1].binding = 0;
    attrs[1].location = 1;
    attrs[1].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    attrs[1].offset = sizeof(float) * 2;

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &bindingDesc;
    vertexInput.vertexAttributeDescriptionCount = 2;
    vertexInput.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    inputAssembly.primitiveRestartEnable = VK_FALSE;

    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(swapchainExtent_.width);
    viewport.height = static_cast<float>(swapchainExtent_.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = swapchainExtent_;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.pViewports = &viewport;
    viewportState.scissorCount = 1;
    viewportState.pScissors = &scissor;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rasterizer.depthBiasEnable = VK_FALSE;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.sampleShadingEnable = VK_FALSE;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    // 标准 SRC_ALPHA 预乘外的普通 alpha 混合，UI 半透明背景需要。
    colorBlendAttachment.blendEnable = VK_TRUE;
    colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;

    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.logicOpEnable = VK_FALSE;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlendAttachment;

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
    VkPushConstantRange pushConstant{};
    pushConstant.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushConstant.offset = 0;
    pushConstant.size = 64;

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushConstant;

    if (vkCreatePipelineLayout(device_, &layoutInfo, nullptr, &pipelineLayout_) != VK_SUCCESS) {
        logMessage(platform_, LogLevel::Error, "evk", "vkCreatePipelineLayout failed");
        vkDestroyShaderModule(device_, vertModule, nullptr);
        vkDestroyShaderModule(device_, fragModule, nullptr);
        return false;
    }

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
    pipelineInfo.layout = pipelineLayout_;
    pipelineInfo.renderPass = renderPass_;
    pipelineInfo.subpass = 0;

    if (vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &graphicsPipeline_) != VK_SUCCESS) {
        logMessage(platform_, LogLevel::Error, "evk", "vkCreateGraphicsPipelines failed");
        vkDestroyShaderModule(device_, vertModule, nullptr);
        vkDestroyShaderModule(device_, fragModule, nullptr);
        return false;
    }

    vkDestroyShaderModule(device_, vertModule, nullptr);
    vkDestroyShaderModule(device_, fragModule, nullptr);
    return true;
}

bool Renderer::createFramebuffers() {
    // 每个 swapchain 图像视图都对应一个 framebuffer。
    swapchainFramebuffers_.resize(swapchainImageViews_.size());
    for (size_t i = 0; i < swapchainImageViews_.size(); ++i) {
        VkImageView attachments[] = {swapchainImageViews_[i]};

        VkFramebufferCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        createInfo.renderPass = renderPass_;
        createInfo.attachmentCount = 1;
        createInfo.pAttachments = attachments;
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
    uint32_t qCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &qCount, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &qCount, qf.data());

    uint32_t graphicsFamily = 0;
    for (uint32_t i = 0; i < qCount; ++i) {
        if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            graphicsFamily = i;
            break;
        }
    }

    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
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
    VkDeviceSize bufferSize = sizeof(ui::UiVertex) * kVertexCapacity;

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = bufferSize;
    bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(device_, &bufferInfo, nullptr, &vertexBuffer_) != VK_SUCCESS) {
        logMessage(platform_, LogLevel::Error, "evk", "vkCreateBuffer failed");
        return false;
    }

    VkMemoryRequirements memReq;
    vkGetBufferMemoryRequirements(device_, vertexBuffer_, &memReq);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex = findMemoryType(memReq.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    if (vkAllocateMemory(device_, &allocInfo, nullptr, &vertexBufferMemory_) != VK_SUCCESS) {
        logMessage(platform_, LogLevel::Error, "evk", "vkAllocateMemory failed");
        return false;
    }

    vkBindBufferMemory(device_, vertexBuffer_, vertexBufferMemory_, 0);
    return true;
}

void Renderer::uploadVertices(const ui::UiVertex* data, uint32_t count) {
    if (count > kVertexCapacity) {
        logMessage(platform_, LogLevel::Warn, "evk",
            "vertex count %u exceeds capacity %u, truncated", count, kVertexCapacity);
        count = kVertexCapacity;
    }
    if (count == 0) {
        return;
    }
    void* mapped = nullptr;
    vkMapMemory(device_, vertexBufferMemory_, 0, sizeof(ui::UiVertex) * count, 0, &mapped);
    std::memcpy(mapped, data, sizeof(ui::UiVertex) * count);
    vkUnmapMemory(device_, vertexBufferMemory_);
}

bool Renderer::createCommandBuffers() {
    // 每个 in-flight 帧配一个主命令缓冲，提交逻辑更简单。
    commandBuffers_.resize(kMaxFramesInFlight);

    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = commandPool_;
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
    imageAvailableSemaphores_.resize(kMaxFramesInFlight);
    renderFinishedSemaphores_.resize(kMaxFramesInFlight);
    inFlightFences_.resize(kMaxFramesInFlight);

    VkSemaphoreCreateInfo semInfo{};
    semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

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
    for (auto fb : swapchainFramebuffers_) {
        vkDestroyFramebuffer(device_, fb, nullptr);
    }
    swapchainFramebuffers_.clear();

    vkDestroyPipeline(device_, graphicsPipeline_, nullptr);
    vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
    vkDestroyRenderPass(device_, renderPass_, nullptr);

    for (auto view : swapchainImageViews_) {
        vkDestroyImageView(device_, view, nullptr);
    }
    swapchainImageViews_.clear();

    vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
}

void Renderer::recreateSwapchain() {
    // 先等 GPU 空闲，释放依赖 swapchain 的状态，再从头重建。
    vkDeviceWaitIdle(device_);
    cleanupSwapchain();
    createSwapchain();
    createImageViews();
    createRenderPass();
    createGraphicsPipeline();
    createFramebuffers();
}

void Renderer::setSize(uint32_t width, uint32_t height) {
    // 真正的重建会在 render() 里做，那时更安全。
    width_ = width;
    height_ = height;
    framebufferResized_ = true;
}

void Renderer::requestSwapchainRebuild() {
    // 平台知道 surface 变了，但还不知道准确尺寸时用这个。
    framebufferResized_ = true;
}

bool Renderer::render(const ui::Canvas& canvas) {
    // 每帧流程：等待 fence、获取图像、录制命令、提交执行、最后呈现。
    vkWaitForFences(device_, 1, &inFlightFences_[currentFrame_], VK_TRUE, UINT64_MAX);

    uint32_t imageIndex = 0;
    VkResult result = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX,
        imageAvailableSemaphores_[currentFrame_], VK_NULL_HANDLE, &imageIndex);

    // 这一帧还没来得及渲染，swapchain 就已经失效了。
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        recreateSwapchain();
        return true;
    } else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        logMessage(platform_, LogLevel::Error, "evk", "vkAcquireNextImageKHR failed: %d", result);
        return false;
    }

    // acquire 成功后先把本帧顶点写进动态缓冲，再录命令。空 canvas 跳过上传，
    // 命令录制阶段也不会有 draw，只剩清屏帧。
    if (!canvas.vertices().empty()) {
        uploadVertices(canvas.vertices().data(), static_cast<uint32_t>(canvas.vertices().size()));
    }

    vkResetFences(device_, 1, &inFlightFences_[currentFrame_]);

    vkResetCommandBuffer(commandBuffers_[currentFrame_], 0);
    recordCommandBuffer(commandBuffers_[currentFrame_], imageIndex, canvas);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

    VkSemaphore waitSemaphores[] = {imageAvailableSemaphores_[currentFrame_]};
    VkPipelineStageFlags waitStages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = waitSemaphores;
    submitInfo.pWaitDstStageMask = waitStages;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffers_[currentFrame_];

    VkSemaphore signalSemaphores[] = {renderFinishedSemaphores_[currentFrame_]};
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = signalSemaphores;

    if (vkQueueSubmit(graphicsQueue_, 1, &submitInfo, inFlightFences_[currentFrame_]) != VK_SUCCESS) {
        logMessage(platform_, LogLevel::Error, "evk", "vkQueueSubmit failed");
        return false;
    }

    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = signalSemaphores;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &swapchain_;
    presentInfo.pImageIndices = &imageIndex;

    result = vkQueuePresentKHR(presentQueue_, &presentInfo);
    // present 阶段也可能提示 surface 和 swapchain 已经不匹配。
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR || framebufferResized_) {
        framebufferResized_ = false;
        recreateSwapchain();
    } else if (result != VK_SUCCESS) {
        logMessage(platform_, LogLevel::Error, "evk", "vkQueuePresentKHR failed: %d", result);
        return false;
    }

    currentFrame_ = (currentFrame_ + 1) % kMaxFramesInFlight;
    return true;
}

void Renderer::recordCommandBuffer(VkCommandBuffer cmd, uint32_t imageIndex, const ui::Canvas& canvas) {
    // 把一帧的绘制命令录进可复用的主命令缓冲。
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(cmd, &beginInfo);

    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = renderPass_;
    renderPassInfo.framebuffer = swapchainFramebuffers_[imageIndex];
    renderPassInfo.renderArea.offset = {0, 0};
    renderPassInfo.renderArea.extent = swapchainExtent_;

    VkClearValue clearColor = {{{0.06f, 0.06f, 0.09f, 1.0f}}};
    renderPassInfo.clearValueCount = 1;
    renderPassInfo.pClearValues = &clearColor;

    vkCmdBeginRenderPass(cmd, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, graphicsPipeline_);

    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(swapchainExtent_.width);
    viewport.height = static_cast<float>(swapchainExtent_.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    // 管线把 viewport 和 scissor 设成动态状态，所以每帧都要重新设置。
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkBuffer vertexBuffers[] = {vertexBuffer_};
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(cmd, 0, 1, vertexBuffers, offsets);

    // 像素坐标 → NDC 的正交投影（原点在左上角，y 向下）。
    // 注意 glm::ortho 第三/四参数是 bottom/top：Vulkan 正 viewport 高度下
    // NDC +y 朝 framebuffer 下方，要 y 向下需传 bottom=0、top=height。
    glm::mat4 mvp = glm::ortho(0.0f, static_cast<float>(swapchainExtent_.width),
                               0.0f, static_cast<float>(swapchainExtent_.height),
                               -1.0f, 1.0f);

    // 逐批绘制：每批共享一个 clip，裁剪矩形取 clip 与 swapchain 的交集。
    for (const auto& batch : canvas.batches()) {
        if (batch.vertexCount == 0) {
            continue;
        }
        vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT,
                           0, sizeof(mvp), &mvp);

        float sw = static_cast<float>(swapchainExtent_.width);
        float sh = static_cast<float>(swapchainExtent_.height);
        ui::Rect clip = ui::Rect::intersect(batch.clip, {0.0f, 0.0f, sw, sh});

        // 转 int32 时 clamp：offset 不小于 0，且 offset+extent 不超出 swapchain。
        int32_t ox = std::max(0, static_cast<int32_t>(clip.x));
        int32_t oy = std::max(0, static_cast<int32_t>(clip.y));
        int32_t ex = std::min(static_cast<int32_t>(clip.x + clip.w),
                              static_cast<int32_t>(swapchainExtent_.width)) - ox;
        int32_t ey = std::min(static_cast<int32_t>(clip.y + clip.h),
                              static_cast<int32_t>(swapchainExtent_.height)) - oy;
        if (ex <= 0 || ey <= 0) {
            continue;
        }

        VkRect2D scissor{};
        scissor.offset = {ox, oy};
        scissor.extent = {static_cast<uint32_t>(ex), static_cast<uint32_t>(ey)};
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        vkCmdDraw(cmd, batch.vertexCount, 1, batch.firstVertex, 0);
    }

    vkCmdEndRenderPass(cmd);

    vkEndCommandBuffer(cmd);
}

uint32_t Renderer::findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    // 在物理设备的内存类型里找一个满足这些标志位的类型。
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &memProps);

    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((typeFilter & (1u << i)) &&
            (memProps.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    return 0;
}

} // namespace evk
