/**
 * @file swapchain.cpp
 * @brief 交换链模块实现：swapchain、image view、MSAA 颜色图、render pass 与 framebuffer。
 */
#include "evk/gpu/swapchain.h"

// VulkanContext：设备 / surface / 平台尺寸查询与显存类型查找。
#include "evk/gpu/vulkan_context.h"
// EVK_LOGI/E 日志宏：spdlog 封装，全局统一的日志入口。
#include "evk/log.h"

// std::max / std::min / std::swap：交换链尺寸的边界 clamp 与 90/270 宽高互换。
#include <algorithm>
// std::vector：承接 Vulkan 枚举惯用法返回的列表。
#include <vector>

namespace evk::gpu {

Swapchain::Swapchain(VulkanContext& context) : context_(context) {}

bool Swapchain::create() {
    // MSAA 采样数只依赖物理设备属性，renderPass/framebuffer 创建前定下来即可；
    // 每次创建重查一次，resize 重建时结果不变。
    msaaSamples_ = pickMsaaSampleCount();
    if (!createSwapchain()) return false;
    if (!createImageViews()) return false;
    // MSAA 颜色图尺寸/格式跟随 swapchain，必须在 renderPass（声明采样数）与
    // framebuffer（挂视图）之前创建；失败时内部会降级为单采样，不判失败。
    createColorResources();
    if (!createRenderPass()) return false;
    return true;
}

void Swapchain::setSize(uint32_t width, uint32_t height) {
    // 真正的重建会在 render() 里做，那时更安全。
    // 只记尺寸并置标志位：本函数可能在渲染途中被平台线程调用，
    // 直接重建会撞上在飞的帧，所以延迟到 render() 的安全点再做。
    width_ = width;
    height_ = height;
    framebufferResized_ = true;
}

void Swapchain::requestRebuild() {
    // 平台知道 surface 变了，但还不知道准确尺寸时用这个。
    // 同样只置标志位，尺寸留给 render() 重建时重新查询。
    framebufferResized_ = true;
}

bool Swapchain::takeRebuildRequest() {
    const bool rebuild = framebufferResized_;
    framebufferResized_ = false;
    return rebuild;
}

bool Swapchain::createSwapchain() {
    const VkPhysicalDevice physicalDevice = context_.physicalDevice();
    const VkDevice device = context_.device();
    const VkSurfaceKHR surface = context_.surface();

    // 选择适合当前窗口的 surface 格式、present 模式和尺寸。
    // 三连查询之一：surface 能力（尺寸范围、图像数量范围、当前变换等）。
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, surface, &caps);

    // 先查询 surface 能力，再决定具体的 swapchain 参数。
    // 三连查询之二：surface 支持的像素格式 + 色彩空间列表。
    uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &formatCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    if (formatCount > 0) {
        vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &formatCount, formats.data());
    }

    // 三连查询之三：支持的 present 模式（图像以什么节奏换到屏幕）。
    uint32_t presentModeCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, surface, &presentModeCount, nullptr);
    std::vector<VkPresentModeKHR> presentModes(presentModeCount);
    if (presentModeCount > 0) {
        vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, surface, &presentModeCount, presentModes.data());
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
    // 例外：MoltenVK 在 layer 尚未布局（bounds 为 0）时上报 0×0——零尺寸交换链无效，
    // 与 UINT32_MAX 一样走平台尺寸分支。
    if (caps.currentExtent.width != UINT32_MAX &&
        caps.currentExtent.width > 0 && caps.currentExtent.height > 0) {
        extent = caps.currentExtent;
    } else {
        // UINT32_MAX 表示尺寸可由我们自由选（桌面窗口常见）：向平台要当前像素尺寸，
        // 再 clamp 到驱动允许的 [minImageExtent, maxImageExtent] 区间。
        context_.platform()->getSurfaceSize(&width_, &height_);
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
    createInfo.surface = surface;
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
    // 记录下来：Renderer 录制命令时的投影与裁剪要按它做旋转补偿，
    // 否则折叠屏内屏竖持（currentTransform=ROTATE_90）时画面是横的。
    surfaceTransform_ = caps.currentTransform;
    EVK_LOGI("swapchain extent={}x{} transform={}",
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
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &qCount, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &qCount, qf.data());
    // 找出图形族与呈现族索引，供下面决定图像共享模式。
    uint32_t gFamily = 0, pFamily = 0;
    for (uint32_t i = 0; i < qCount; ++i) {
        if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) gFamily = i;
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(physicalDevice, i, surface, &present);
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
    if (vkCreateSwapchainKHR(device, &createInfo, nullptr, &swapchain_) != VK_SUCCESS) {
        EVK_LOGE("vkCreateSwapchainKHR failed");
        return false;
    }

    // 创建后再取回真正的图像句柄列表（数量可能与请求值不同，仍是两次调用惯用法）。
    vkGetSwapchainImagesKHR(device, swapchain_, &imageCount, nullptr);
    swapchainImages_.resize(imageCount);
    vkGetSwapchainImagesKHR(device, swapchain_, &imageCount, swapchainImages_.data());

    // 记住格式与尺寸：后面 renderPass、pipeline、framebuffer、viewport 都要用。
    swapchainImageFormat_ = surfaceFormat.format;
    swapchainExtent_ = extent;
    return true;
}

bool Swapchain::createImageViews() {
    const VkDevice device = context_.device();

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

        if (vkCreateImageView(device, &createInfo, nullptr, &swapchainImageViews_[i]) != VK_SUCCESS) {
            EVK_LOGE("vkCreateImageView failed");
            return false;
        }
    }
    return true;
}

VkSampleCountFlagBits Swapchain::pickMsaaSampleCount() const {
    // 采样数上限是设备属性：颜色附件最多允许多少采样点/像素，按位掩码给出。
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(context_.physicalDevice(), &props);
    const VkSampleCountFlags supported =
        props.limits.framebufferColorSampleCounts;

    // 移动端 GPU 几乎都支持 4x；8x 显存带宽再翻倍而边缘质量收益很小，
    // 所以只在 4 → 2 里挑，都不支持就老老实实单采样（走无 resolve 的旧路径）。
    if (supported & VK_SAMPLE_COUNT_4_BIT) return VK_SAMPLE_COUNT_4_BIT;
    if (supported & VK_SAMPLE_COUNT_2_BIT) return VK_SAMPLE_COUNT_2_BIT;
    return VK_SAMPLE_COUNT_1_BIT;
}

bool Swapchain::createColorResources() {
    const VkDevice device = context_.device();

    // 单采样 = 未启用 MSAA，renderPass/framebuffer 都走旧的单附件路径。
    if (msaaSamples_ == VK_SAMPLE_COUNT_1_BIT) return true;

    // 多采样颜色图：与 swapchain 图像同尺寸、同格式，只是 samples = N、
    // 用途是"被绘制"（颜色附件）。resolve 由 subpass 自动完成，不需要
    // TRANSFER 用途位。创建套路与采样纹理相同：
    // 图像 → 查显存需求 → 分配（设备本地）→ 绑定 → 建视图。
    //
    // TRANSIENT 声明这张图只在 render pass 内存活（storeOp 已是 DONT_CARE），
    // 再配 LAZILY_ALLOCATED 显存：tile GPU（移动端主流）把多采样数据全程
    // 留在片上，4x 全屏 RGBA8 这块显存根本不必真实分配出来，省十几 MB 带宽与占用。
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = swapchainImageFormat_;
    imageInfo.extent = {swapchainExtent_.width, swapchainExtent_.height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = msaaSamples_;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                      VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device, &imageInfo, nullptr, &msaaColorImage_) != VK_SUCCESS) {
        EVK_LOGE("msaa vkCreateImage failed");
        msaaSamples_ = VK_SAMPLE_COUNT_1_BIT;
        return false;
    }

    VkMemoryRequirements memReq;
    vkGetImageMemoryRequirements(device, msaaColorImage_, &memReq);

    // 优先 DEVICE_LOCAL + LAZILY_ALLOCATED（自己扫内存类型；桌面端常没有
    // lazy 类型，那就退回不带 TRANSIENT 的普通设备本地显存——功能一样，
    // 只是少了省显存的优化）。
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(context_.physicalDevice(), &memProps);
    constexpr VkMemoryPropertyFlags kLazyLocal =
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT;
    uint32_t memoryType = UINT32_MAX;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((memReq.memoryTypeBits & (1u << i)) &&
            (memProps.memoryTypes[i].propertyFlags & kLazyLocal) == kLazyLocal) {
            memoryType = i;
            break;
        }
    }
    if (memoryType == UINT32_MAX) {
        vkDestroyImage(device, msaaColorImage_, nullptr);
        msaaColorImage_ = VK_NULL_HANDLE;
        imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        if (vkCreateImage(device, &imageInfo, nullptr, &msaaColorImage_) != VK_SUCCESS) {
            EVK_LOGE("msaa vkCreateImage (non-transient) failed");
            msaaSamples_ = VK_SAMPLE_COUNT_1_BIT;
            return false;
        }
        vkGetImageMemoryRequirements(device, msaaColorImage_, &memReq);
        memoryType = context_.findMemoryType(memReq.memoryTypeBits,
                                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    }

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex = memoryType;
    if (vkAllocateMemory(device, &allocInfo, nullptr, &msaaColorImageMemory_) != VK_SUCCESS) {
        EVK_LOGE("msaa vkAllocateMemory failed");
        vkDestroyImage(device, msaaColorImage_, nullptr);
        msaaColorImage_ = VK_NULL_HANDLE;
        msaaSamples_ = VK_SAMPLE_COUNT_1_BIT;
        return false;
    }
    if (vkBindImageMemory(device, msaaColorImage_, msaaColorImageMemory_, 0) != VK_SUCCESS) {
        EVK_LOGE("msaa vkBindImageMemory failed");
        vkFreeMemory(device, msaaColorImageMemory_, nullptr);
        vkDestroyImage(device, msaaColorImage_, nullptr);
        msaaColorImageMemory_ = VK_NULL_HANDLE;
        msaaColorImage_ = VK_NULL_HANDLE;
        msaaSamples_ = VK_SAMPLE_COUNT_1_BIT;
        return false;
    }

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = msaaColorImage_;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = swapchainImageFormat_;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;
    if (vkCreateImageView(device, &viewInfo, nullptr, &msaaColorImageView_) != VK_SUCCESS) {
        EVK_LOGE("msaa vkCreateImageView failed");
        vkDestroyImage(device, msaaColorImage_, nullptr);
        vkFreeMemory(device, msaaColorImageMemory_, nullptr);
        msaaColorImage_ = VK_NULL_HANDLE;
        msaaColorImageMemory_ = VK_NULL_HANDLE;
        msaaSamples_ = VK_SAMPLE_COUNT_1_BIT;
        return false;
    }
    return true;
}

bool Swapchain::createRenderPass() {
    // 附件描述：renderPass 不绑定具体图像，只声明"有一个什么样的附件、怎么用"。
    //
    // MSAA 开启时附件变成两个，各司其职：
    //   [0] swapchain 图像 —— 绘制结果的最终归宿，兼作 resolve 目标；
    //   [1] MSAA 多采样颜色图 —— 管线真正的绘制目标。
    // 绘制先写进多采样图（每个像素存 N 个采样点），subpass 结束时硬件把
    // resolve attachment（指向 [0]）对每像素 N 个采样自动平均成单采样值写回
    // swapchain 图像——这一步叫 resolve，圆弧/斜边边缘像素因此拿到 0~1 之间的
    // 渐变覆盖率，而不是"要么整像素要么没有"的硬边，锯齿由此消除。
    const bool msaaEnabled = msaaSamples_ != VK_SAMPLE_COUNT_1_BIT;

    // 附件 [0]：呈现目标。格式必须与 swapchain 图像一致，否则 framebuffer 创建会失败。
    VkAttachmentDescription presentAttachment{};
    presentAttachment.format = swapchainImageFormat_;
    presentAttachment.samples = VK_SAMPLE_COUNT_1_BIT; // resolve 后落地的图像是单采样
    // loadOp = CLEAR：render pass 开始时把附件清成 clearValue，每帧清屏就靠它。
    presentAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    // storeOp = STORE：渲染结束后保留结果；要呈现到屏幕，必须存。
    presentAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    // 没有模板附件，stencil 操作直接 DONT_CARE。
    presentAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    presentAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    // initialLayout = UNDEFINED：不关心旧内容（反正要清屏），允许驱动直接丢弃它。
    presentAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    // finalLayout = PRESENT_SRC_KHR：渲染完图像要交给呈现引擎，布局必须转成它。
    presentAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    // 附件 [1]：MSAA 多采样颜色图（仅 msaaEnabled 时使用）。
    VkAttachmentDescription msaaAttachment{};
    msaaAttachment.format = swapchainImageFormat_;
    msaaAttachment.samples = msaaSamples_; // 每像素 N 个采样点
    msaaAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    // storeOp = DONT_CARE：多采样图只在本 subpass 内被绘制+resolve 消费，
    // 之后内容再无用途，驱动可以不写回显存省带宽。
    msaaAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    msaaAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    msaaAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    msaaAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    // resolve 之后多采样图的使命结束，停留在通用的颜色附件布局即可（无需呈现）。
    msaaAttachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentDescription attachments[2] = {presentAttachment, msaaAttachment};

    // 附件引用：subpass 不直接用附件，而是引用其下标，并指定渲染期间的布局。
    // 颜色引用指向多采样图（关闭 MSAA 时就是 [0] 即 swapchain 本身）。
    VkAttachmentReference colorRef{};
    colorRef.attachment = msaaEnabled ? 1 : 0;
    colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    // resolve 引用指向 [0]：subpass 结束时把颜色附件的每像素 N 采样平均写进它。
    // 数组下标必须与 pColorAttachments 一一对应（这里各只有 1 个）。
    VkAttachmentReference resolveRef{};
    resolveRef.attachment = 0;
    resolveRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    // 唯一的 subpass：跑图形管线、写 1 个颜色附件（可选 + 1 个 resolve 目标）。
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;
    subpass.pResolveAttachments = msaaEnabled ? &resolveRef : nullptr;

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

    // 总装：MSAA 时 2 个附件（多采样图 + 呈现图），否则 1 个；subpass 与依赖各 1 条。
    VkRenderPassCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    createInfo.attachmentCount = msaaEnabled ? 2 : 1;
    createInfo.pAttachments = attachments;
    createInfo.subpassCount = 1;
    createInfo.pSubpasses = &subpass;
    createInfo.dependencyCount = 1;
    createInfo.pDependencies = &dependency;

    if (vkCreateRenderPass(context_.device(), &createInfo, nullptr, &renderPass_) != VK_SUCCESS) {
        EVK_LOGE("vkCreateRenderPass failed");
        return false;
    }
    return true;
}

bool Swapchain::createFramebuffers() {
    const VkDevice device = context_.device();

    // 每个 swapchain 图像视图都对应一个 framebuffer。
    // 每张图像配一个；渲染时按 acquire 到的 imageIndex 选用对应的那一个。
    swapchainFramebuffers_.resize(swapchainImageViews_.size());
    const bool msaaEnabled = msaaSamples_ != VK_SAMPLE_COUNT_1_BIT;
    for (size_t i = 0; i < swapchainImageViews_.size(); ++i) {
        // framebuffer 把 render pass 声明的"抽象附件"绑定到具体 imageView；
        // 数组顺序要与 render pass 的附件下标一一对应：
        // [0] 呈现/resolve 目标（swapchain 图像），[1] MSAA 多采样颜色图。
        VkImageView attachments[] = {swapchainImageViews_[i], msaaColorImageView_};

        VkFramebufferCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        // 指定兼容的 render pass（通常就是同一个）。
        createInfo.renderPass = renderPass_;
        createInfo.attachmentCount = msaaEnabled ? 2 : 1;
        createInfo.pAttachments = attachments;
        // 宽高按 swapchain 尺寸；layers 非立体渲染恒为 1。
        createInfo.width = swapchainExtent_.width;
        createInfo.height = swapchainExtent_.height;
        createInfo.layers = 1;

        if (vkCreateFramebuffer(device, &createInfo, nullptr, &swapchainFramebuffers_[i]) != VK_SUCCESS) {
            EVK_LOGE("vkCreateFramebuffer failed");
            return false;
        }
    }
    return true;
}

void Swapchain::cleanup() {
    const VkDevice device = context_.device();

    // 这些资源和 swapchain 的格式、尺寸绑定，所以要一起重建。
    // framebuffer 依赖 imageView 与尺寸，先销。
    for (auto fb : swapchainFramebuffers_) {
        vkDestroyFramebuffer(device, fb, nullptr);
    }
    swapchainFramebuffers_.clear();

    // renderPass 按 swapchain 格式/采样数配置，一并重建。
    // （管线归属 UiPipeline，由 Renderer 在同批重建里另行销毁。）
    if (renderPass_ != VK_NULL_HANDLE) {
        vkDestroyRenderPass(device, renderPass_, nullptr);
        renderPass_ = VK_NULL_HANDLE;
    }

    // MSAA 多采样颜色图：尺寸跟随 swapchain，同一批销毁重建。
    if (msaaColorImageView_ != VK_NULL_HANDLE) {
        vkDestroyImageView(device, msaaColorImageView_, nullptr);
        msaaColorImageView_ = VK_NULL_HANDLE;
    }
    if (msaaColorImage_ != VK_NULL_HANDLE) {
        vkDestroyImage(device, msaaColorImage_, nullptr);
        msaaColorImage_ = VK_NULL_HANDLE;
    }
    if (msaaColorImageMemory_ != VK_NULL_HANDLE) {
        vkFreeMemory(device, msaaColorImageMemory_, nullptr);
        msaaColorImageMemory_ = VK_NULL_HANDLE;
    }

    // imageView 与 swapchain 本体最后销。
    for (auto view : swapchainImageViews_) {
        vkDestroyImageView(device, view, nullptr);
    }
    swapchainImageViews_.clear();
    swapchainImages_.clear();

    if (swapchain_ != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }
}

} // namespace evk::gpu
