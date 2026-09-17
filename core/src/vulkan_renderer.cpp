/**
 * @file vulkan_renderer.cpp
 * @brief 极简自包含 Vulkan 渲染器实现：渲染 ui::Canvas 收集的 2D UI 几何。
 *
 * GPU 设施分属 gpu/ 子模块（VulkanContext / Swapchain / UiPipeline /
 * TextureCache）；本文件只保留帧编排与执行设施：命令池、动态顶点缓冲、
 * 同步原语，以及 acquire → 录制 → 提交 → present 的 render 主流程。
 */
// Renderer 主头文件：声明本文件要实现的所有方法与句柄成员。
#include "evk/vulkan_renderer.h"
// EVK_LOGI/W/E 日志宏：spdlog 封装，全局统一的日志入口。
#include "evk/log.h"

// std::max / std::min：裁剪矩形的边界 clamp 要用。
#include <algorithm>
// std::memcpy：把顶点数据拷进映射内存。
#include <cstring>
#include <limits>
// std::vector：承接 Vulkan 枚举惯用法返回的列表。
#include <vector>

// glm 核心类型（mat4 等）与 glm::ortho：投影矩阵构建。
// （GLM_FORCE_DEPTH_ZERO_TO_ONE 已在 vulkan_renderer.h 顶部统一定义。）
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace evk {

/// 单次 render() 内"重建交换链 → 重画"的最大重试次数（见 render() 注释）。
constexpr uint32_t kMaxFrameAttempts = 3;

Renderer::Renderer(IPlatform* platform, gpu::ITextureSource* textureSource)
    : context_(platform),
      textureCache_(context_, textureSource),
      swapchain_(context_),
      uiPipeline_(context_),
      offscreenEffects_(context_) {}

Renderer::~Renderer() {
    // 析构兜底调 shutdown()；内部对空句柄有判断，重复调用也安全。
    shutdown();
}

bool Renderer::initialize() {
    // 按依赖顺序搭建 Vulkan 栈。每一步都依赖前一步，所以任何一环失败都会立刻停止启动。
    // ① 地基：instance / surface / 物理与逻辑设备（gpu::VulkanContext）。
    if (!context_.initialize()) return false;
    // ② 纹理设施：descriptor 布局是管线布局的输入（shader 的 set=0），必须先于管线。
    if (!textureCache_.initialize()) return false;
    // ③ 交换链及其下游：imageView / MSAA 颜色图 / renderPass 全部依赖 swapchain 的
    // 格式与尺寸，窗口尺寸变化时这一段连同管线整体重建（见 recreateSwapchain）。
    if (!swapchain_.create()) return false;
    if (!uiPipeline_.create(swapchain_.renderPass(), swapchain_.extent(),
                            swapchain_.msaaSamples(),
                            textureCache_.descriptorSetLayout())) return false;
    if (!swapchain_.createFramebuffers()) return false;
    // 离屏效果设施（背景模糊）：尺寸/格式/采样数依赖 swapchain，失败仅降级
    // （模糊标记批被忽略，内容照画），不阻断启动。
    if (!createOffscreenEffects()) {
        EVK_LOGW("renderer", "offscreen_unavailable phase=initialize blur=disabled");
    }
    // ④ 执行设施：命令池、顶点缓冲、命令缓冲、同步原语，与 swapchain 尺寸无关。
    if (!createCommandPool()) return false;
    if (!createVertexBuffers()) return false;
    if (!createCommandBuffers()) return false;
    if (!createSyncObjects()) return false;
    return true;
}

/// 离屏效果设施创建：视觉尺寸（90/270° 与 buffer 宽高互换）+ swapchain
/// 格式/采样数；管线复用 UiPipeline 的共享 layout，挂在主/场景 pass 上。
bool Renderer::createOffscreenEffects() {
    const VkExtent2D extent = swapchain_.extent();
    const VkSurfaceTransformFlagBitsKHR transform = swapchain_.surfaceTransform();
    const bool swap = transform == VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR ||
                      transform == VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR;
    const VkExtent2D visual = swap ? VkExtent2D{extent.height, extent.width}
                                   : extent;
    if (!offscreenEffects_.create(visual, swapchain_.imageFormat(),
                                  swapchain_.msaaSamples(),
                                  textureCache_.descriptorSetLayout(),
                                  uiPipeline_.layout())) {
        return false;
    }
    return offscreenEffects_.createPipelines(swapchain_.renderPass(),
                                             uiPipeline_.layout(),
                                             swapchain_.msaaSamples());
}

void Renderer::shutdown() {
    // 按相反顺序释放资源。设备必须先空闲，才能销毁任何仍在飞行中的对象。
    // 初始化可能中途失败退出，device 仍是空句柄，先判一下。
    const VkDevice device = context_.device();
    if (device != VK_NULL_HANDLE) {
        // 先等 GPU 跑完所有在飞工作：销毁仍在被 GPU 使用的对象是未定义行为。
        vkDeviceWaitIdle(device);

        // 同步原语：fence 与两类 semaphore，每帧一套。
        for (auto fence : inFlightFences_) vkDestroyFence(device, fence, nullptr);
        for (auto sem : renderFinishedSemaphores_) vkDestroySemaphore(device, sem, nullptr);
        for (auto sem : imageAvailableSemaphores_) vkDestroySemaphore(device, sem, nullptr);

        // 每个 in-flight 帧有自己的动态顶点缓冲。
        destroyVertexBuffers();

        // 销毁命令池会连带释放从中分配的所有命令缓冲。
        vkDestroyCommandPool(device, commandPool_, nullptr);

        // 纹理设施（atlas 页 / 白纹理 / 采样器 / descriptor 池与布局 / 上传中转缓冲）。
        textureCache_.shutdown();

        // 离屏效果、管线与 swapchain 相关资源各归其模块集中清理。
        offscreenEffects_.destroy();
        uiPipeline_.destroy();
        swapchain_.cleanup();
    }

    // device / surface / debug messenger / instance 归 VulkanContext 收尾（幂等）。
    context_.shutdown();

    // 清空所有持有句柄的 vector，避免残留已销毁的句柄值。
    inFlightFences_.clear();
    renderFinishedSemaphores_.clear();
    imageAvailableSemaphores_.clear();
    commandBuffers_.clear();
}

bool Renderer::createCommandPool() {
    const VkDevice device = context_.device();

    // command pool 允许命令缓冲每帧重置并重新录制。
    // 命令池要从某个队列族分配命令缓冲：先再查一次图形族索引。
    uint32_t qCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(context_.physicalDevice(), &qCount, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qCount);
    vkGetPhysicalDeviceQueueFamilyProperties(context_.physicalDevice(), &qCount, qf.data());

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

    if (vkCreateCommandPool(device, &poolInfo, nullptr, &commandPool_) != VK_SUCCESS) {
        EVK_LOGE("renderer", "create_command_pool_failed");
        return false;
    }
    return true;
}

bool Renderer::createVertexBuffers() {
    vertexBuffers_.assign(gpu::kMaxFramesInFlight, VK_NULL_HANDLE);
    vertexBufferMemorys_.assign(gpu::kMaxFramesInFlight, VK_NULL_HANDLE);
    vertexBufferCapacities_.assign(gpu::kMaxFramesInFlight, 0);
    for (uint32_t frameSlot = 0; frameSlot < gpu::kMaxFramesInFlight; ++frameSlot) {
        if (!createVertexBuffer(frameSlot, kInitialVertexCapacity)) {
            destroyVertexBuffers();
            return false;
        }
    }
    return true;
}

bool Renderer::createVertexBuffer(uint32_t frameSlot, uint32_t capacity) {
    const VkDevice device = context_.device();

    if (frameSlot >= vertexBuffers_.size() || capacity == 0) {
        return false;
    }
    // 每个帧槽独占一块 HOST_VISIBLE|HOST_COHERENT 缓冲，容量按需扩展。
    const VkDeviceSize bufferSize = sizeof(ui::UiVertex) *
                                    static_cast<VkDeviceSize>(capacity);
    VkBuffer newBuffer = VK_NULL_HANDLE;
    VkDeviceMemory newMemory = VK_NULL_HANDLE;

    // VkBuffer 只是"一块多大、干什么用"的描述对象，本身不带内存。
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = bufferSize;
    // usage = VERTEX_BUFFER：稍后要用 vkCmdBindVertexBuffers 绑它。
    bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    // 只有图形队列用它，EXCLUSIVE 独占即可。
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(device, &bufferInfo, nullptr, &newBuffer) != VK_SUCCESS) {
        EVK_LOGE("renderer", "create_vertex_buffer_failed operation=create_buffer");
        return false;
    }

    // 创建后问驱动：这块 buffer 需要多大、什么对齐、允许哪些内存类型。
    VkMemoryRequirements memReq;
    vkGetBufferMemoryRequirements(device, newBuffer, &memReq);

    // Vulkan 不自动配内存：要显式 vkAllocateMemory 再 bind；
    // 内存类型必须同时满足 buffer 的要求（memoryTypeBits）与我们的属性要求。
    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;
    // HOST_VISIBLE：CPU 可直接 map 写入；HOST_COHERENT：写完 GPU 立即可见、免手动 flush，
    // 每帧上传少量顶点时最省事，可以省掉 staging buffer。
    allocInfo.memoryTypeIndex = context_.findMemoryType(memReq.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    // 按需求尺寸和选定类型分配设备内存。
    if (vkAllocateMemory(device, &allocInfo, nullptr, &newMemory) != VK_SUCCESS) {
        EVK_LOGE("renderer", "create_vertex_buffer_failed operation=allocate_memory");
        vkDestroyBuffer(device, newBuffer, nullptr);
        return false;
    }

    if (vkBindBufferMemory(device, newBuffer, newMemory, 0) != VK_SUCCESS) {
        EVK_LOGE("renderer", "create_vertex_buffer_failed operation=bind_memory");
        vkFreeMemory(device, newMemory, nullptr);
        vkDestroyBuffer(device, newBuffer, nullptr);
        return false;
    }
    if (vertexBuffers_[frameSlot] != VK_NULL_HANDLE) {
        vkDestroyBuffer(device, vertexBuffers_[frameSlot], nullptr);
    }
    if (vertexBufferMemorys_[frameSlot] != VK_NULL_HANDLE) {
        vkFreeMemory(device, vertexBufferMemorys_[frameSlot], nullptr);
    }
    vertexBuffers_[frameSlot] = newBuffer;
    vertexBufferMemorys_[frameSlot] = newMemory;
    vertexBufferCapacities_[frameSlot] = capacity;
    return true;
}

void Renderer::destroyVertexBuffers() {
    const VkDevice device = context_.device();
    for (VkBuffer buffer : vertexBuffers_) {
        if (buffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(device, buffer, nullptr);
        }
    }
    for (VkDeviceMemory memory : vertexBufferMemorys_) {
        if (memory != VK_NULL_HANDLE) {
            vkFreeMemory(device, memory, nullptr);
        }
    }
    vertexBuffers_.clear();
    vertexBufferMemorys_.clear();
    vertexBufferCapacities_.clear();
}

bool Renderer::uploadVertices(const ui::UiVertex* data, uint32_t count,
                              uint32_t frameSlot) {
    if (count == 0) {
        return true;
    }
    if (!data || frameSlot >= vertexBuffers_.size()) {
        return false;
    }
    if (count > vertexBufferCapacities_[frameSlot]) {
        uint32_t capacity = std::max(kInitialVertexCapacity,
                                     vertexBufferCapacities_[frameSlot]);
        while (capacity < count) {
            if (capacity > std::numeric_limits<uint32_t>::max() / 2) {
                capacity = count;
                break;
            }
            capacity *= 2;
        }
        if (!createVertexBuffer(frameSlot, capacity)) {
            return false;
        }
    }

    const VkDeviceSize byteSize = sizeof(ui::UiVertex) *
                                  static_cast<VkDeviceSize>(count);
    void* mapped = nullptr;
    if (vkMapMemory(context_.device(), vertexBufferMemorys_[frameSlot], 0,
                    byteSize, 0, &mapped) != VK_SUCCESS) {
        EVK_LOGE("renderer", "vertex_upload_failed operation=map_memory");
        return false;
    }
    std::memcpy(mapped, data, static_cast<size_t>(byteSize));
    vkUnmapMemory(context_.device(), vertexBufferMemorys_[frameSlot]);
    return true;
}

bool Renderer::createCommandBuffers() {
    // 每个 in-flight 帧配一个主命令缓冲，提交逻辑更简单。
    // kMaxFramesInFlight = 2：CPU 最多领先 GPU 两帧，每帧一条缓冲互不干扰。
    commandBuffers_.resize(gpu::kMaxFramesInFlight);

    // 从命令池一次性批量分配。
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = commandPool_;
    // PRIMARY：可直接提交到队列（SECONDARY 只能被 primary 缓冲调用）。
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = static_cast<uint32_t>(commandBuffers_.size());

    if (vkAllocateCommandBuffers(context_.device(), &allocInfo, commandBuffers_.data()) != VK_SUCCESS) {
        EVK_LOGE("renderer", "allocate_command_buffers_failed");
        return false;
    }
    return true;
}

bool Renderer::createSyncObjects() {
    const VkDevice device = context_.device();

    // 双缓冲能让 CPU 和 GPU 有一点重叠，但不会无限堆积帧。
    // 两类同步原语：semaphore 做 GPU 内部阶段间等待（acquire → 渲染 → 呈现），
    // fence 做 CPU 等 GPU；每帧各一套，互不打架。
    imageAvailableSemaphores_.resize(gpu::kMaxFramesInFlight);
    renderFinishedSemaphores_.resize(gpu::kMaxFramesInFlight);
    inFlightFences_.resize(gpu::kMaxFramesInFlight);

    // semaphore 创建无需标志位，初始为未触发状态。
    VkSemaphoreCreateInfo semInfo{};
    semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    // fence 建成 SIGNALED 初始态：第一帧 render() 的 vkWaitForFences 能立即通过，
    // 否则会永久等在一个从没人 signal 过的 fence 上。
    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    // 逐帧创建两个 semaphore + 一个 fence。
    for (uint32_t i = 0; i < gpu::kMaxFramesInFlight; ++i) {
        if (vkCreateSemaphore(device, &semInfo, nullptr, &imageAvailableSemaphores_[i]) != VK_SUCCESS ||
            vkCreateSemaphore(device, &semInfo, nullptr, &renderFinishedSemaphores_[i]) != VK_SUCCESS ||
            vkCreateFence(device, &fenceInfo, nullptr, &inFlightFences_[i]) != VK_SUCCESS) {
            EVK_LOGE("renderer", "create_sync_objects_failed frame_slot={}", i);
            return false;
        }
    }
    return true;
}

void Renderer::recreateSwapchain() {
    // 先等 GPU 空闲，释放依赖 swapchain 的状态，再从头重建。
    // 重建期间不能有任何帧在飞：先 vkDeviceWaitIdle 让 GPU 完全空闲。
    vkDeviceWaitIdle(context_.device());
    // 然后按 initialize() 里"依赖 swapchain 的那一段"原序重建；
    // 命令池、顶点缓冲、同步对象与尺寸无关，不用动。
    // 管线与离屏设施都跟随 swapchain 格式/尺寸/采样数，同批销毁重建。
    offscreenEffects_.destroy();
    uiPipeline_.destroy();
    swapchain_.cleanup();
    swapchain_.create();
    uiPipeline_.create(swapchain_.renderPass(), swapchain_.extent(),
                       swapchain_.msaaSamples(),
                       textureCache_.descriptorSetLayout());
    swapchain_.createFramebuffers();
    if (!createOffscreenEffects()) {
        EVK_LOGW("renderer", "offscreen_unavailable phase=recreate blur=disabled");
    }
}

void Renderer::setSize(uint32_t width, uint32_t height) {
    swapchain_.setSize(width, height);
}

void Renderer::requestSwapchainRebuild() {
    swapchain_.requestRebuild();
}

bool Renderer::render(const ui::Canvas& canvas) {
    // 尺寸变化（旋转等）时先重建 swapchain 再画帧：否则本帧会按旧 swapchain 尺寸
    // 投影、画进旧尺寸的图像，呈现出去的是变形/裁剪的画面；本渲染器是按需模型，
    // 之后没有新帧覆盖，错误会一直挂到下次事件。
    //
    // 整个画帧流程包在有限次重试里：surface 刚变化时驱动状态尚未落定——
    // 重建查到的 surface 能力可能还是旧尺寸（滞后一拍），acquire/present 也会报
    // OUT_OF_DATE / SUBOPTIMAL。按需渲染没有"下一帧"来自我修正（持续渲染的引擎
    // 靠下一帧自然收敛），所以本帧内立刻重建重画，直到交换链与 surface 匹配。
    for (uint32_t attempt = 0; attempt < kMaxFrameAttempts; ++attempt) {
        if (swapchain_.takeRebuildRequest()) {
            recreateSwapchain();
        }
        // 每帧流程：等待 fence、获取图像、录制命令、提交执行、最后呈现。
        // ① 等本帧槽位的 fence：确保它上一轮的渲染已完成，
        // 这把 CPU 领先 GPU 的帧数限制在 kMaxFramesInFlight 以内，防止无限堆积。
        vkWaitForFences(context_.device(), 1, &inFlightFences_[currentFrame_],
                        VK_TRUE, UINT64_MAX);

        // ② acquire：向交换链申请下一张可写图像；图像就绪时 GPU 会 signal imageAvailableSemaphore。
        uint32_t imageIndex = 0;
        VkResult result = vkAcquireNextImageKHR(context_.device(), swapchain_.handle(),
            UINT64_MAX, imageAvailableSemaphores_[currentFrame_], VK_NULL_HANDLE,
            &imageIndex);

        // 这一帧还没来得及渲染，swapchain 就已经失效了。
        // OUT_OF_DATE 说明交换链与 surface 已不匹配（尺寸/旋转变化）：
        // 立刻重建并重试本帧（旧实现直接放弃本帧，按需模型下画面会整帧丢失）。
        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            recreateSwapchain();
            continue;
        // SUBOPTIMAL 也算拿到图像（只是与 surface 不再完全匹配），照常渲染，present 后再重建。
        } else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
            EVK_LOGE("renderer", "acquire_image_failed result={}",
                     static_cast<int>(result));
            return false;
        }

        // acquire 成功后先把本帧顶点写进动态缓冲，再录命令。空 canvas 跳过上传，
        // 命令录制阶段也不会有 draw，只剩清屏帧。
        // ③ 上传顶点：赶在命令录制前把数据写进 HOST_VISIBLE 缓冲。
        if (!canvas.vertices().empty()) {
            if (!uploadVertices(canvas.vertices().data(),
                                static_cast<uint32_t>(canvas.vertices().size()),
                                currentFrame_)) {
                EVK_LOGE("renderer", "vertex_upload_failed operation=upload");
                return false;
            }
        }

        // ④ reset fence 必须在确认本帧一定会提交之后做：若提前 reset 又中途 return，
        // 下一帧 vkWaitForFences 会永远等不到 signal（死锁）。
        vkResetFences(context_.device(), 1, &inFlightFences_[currentFrame_]);

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
        if (vkQueueSubmit(context_.graphicsQueue(), 1, &submitInfo,
                          inFlightFences_[currentFrame_]) != VK_SUCCESS) {
            EVK_LOGE("renderer", "queue_submit_failed");
            return false;
        }

        // ⑥ present：把渲染好的图像交回交换链排队上屏。
        VkPresentInfoKHR presentInfo{};
        presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        // present 要等 renderFinished：渲染没完成不能显示，顺序由 semaphore 在 GPU 上保证。
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = signalSemaphores;
        presentInfo.swapchainCount = 1;
        VkSwapchainKHR swapchainHandle = swapchain_.handle();
        presentInfo.pSwapchains = &swapchainHandle;
        presentInfo.pImageIndices = &imageIndex;

        result = vkQueuePresentKHR(context_.presentQueue(), &presentInfo);
        // present 阶段也可能提示 surface 和 swapchain 已经不匹配。
        // OUT_OF_DATE / SUBOPTIMAL 说明本帧是按不匹配的交换链画的（典型的如旋转后
        // 第一次重建拿到了旧尺寸）：置标志位并重试，下一趟循环开头重建、用同一
        // canvas 重画，用户看到的直接就是修正后的画面。
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
            swapchain_.requestRebuild();
            continue;
        } else if (result != VK_SUCCESS) {
            EVK_LOGE("renderer", "queue_present_failed result={}",
                     static_cast<int>(result));
            return false;
        }

        // ⑦ 轮转帧槽位 0→1→0→1：下一帧换用另一套 semaphore / fence / 命令缓冲。
        currentFrame_ = (currentFrame_ + 1) % gpu::kMaxFramesInFlight;
        return true;
    }

    // 重试耗尽（surface 持续变化中，如快速连续旋转）：放弃本帧不算失败，
    // 后续尺寸事件还会触发渲染，届时继续收敛。
    EVK_LOGW("renderer", "frame_skipped reason=swapchain_out_of_date attempts={}",
             kMaxFrameAttempts);
    return true;
}

namespace {

/// push constant 的 CPU 侧布局：mvp 64B + effectRect 16B + effectParams 16B
/// + effectExtra 16B = 112B；主/SDF 管线只用前 96B，离屏全屏管线用满。
struct PushConstants {
    glm::mat4 mvp;
    float effectRect[4];   ///< 效果/目标矩形 x,y,w,h（视觉像素）
    float effectParams[4]; ///< SDF：radius,blur,mode；模糊：uv 步长 stepU,stepV
    float effectExtra[4];  ///< 全屏管线：texelSize（1/W, 1/H）
};

} // namespace

void Renderer::barrierImage(VkCommandBuffer cmd, VkImage image,
                            VkImageLayout oldLayout, VkImageLayout newLayout,
                            VkAccessFlags srcAccess, VkAccessFlags dstAccess,
                            VkPipelineStageFlags srcStage,
                            VkPipelineStageFlags dstStage) {
    // 单张图像的布局/内存屏障：离屏各段之间的同步都走它（render pass 的
    // 隐式外部依赖阶段太宽，显式屏障语义最直白）。
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1,
                         &barrier);
}

void Renderer::recordBatchRange(VkCommandBuffer cmd, const ui::Canvas& canvas,
                                size_t begin, size_t end, const glm::mat4& mvp,
                                VkExtent2D targetExtent,
                                VkSurfaceTransformFlagBitsKHR transform) {
    // 起始统一切回主管线（前一段可能是模糊/合成管线）；范围内按需切 SDF。
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      uiPipeline_.pipeline(gpu::PipelineKind::kMain));
    gpu::PipelineKind boundPipeline = gpu::PipelineKind::kMain;

    // 按渲染目标尺寸组 viewport（NDC 到像素的映射），每段重设。
    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(targetExtent.width);
    viewport.height = static_cast<float>(targetExtent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    // 绑定顶点缓冲到槽位 0（对应管线 vertex input 的 binding 0），偏移 0。
    VkBuffer vertexBuffers[] = {vertexBuffers_[currentFrame_]};
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(cmd, 0, 1, vertexBuffers, offsets);

    // 视觉尺寸（app 坐标空间 = 用户实际看到的方向）：呈现旋转 90/270 时
    // 与目标 buffer 宽高互换；离屏场景按视觉方向绘制（transform=IDENTITY）。
    const float bufferW = static_cast<float>(targetExtent.width);
    const float bufferH = static_cast<float>(targetExtent.height);
    const bool rotate90or270 =
        transform == VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR ||
        transform == VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR;
    const bool compensate = transform != VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    const float visualW = rotate90or270 ? bufferH : bufferW;
    const float visualH = rotate90or270 ? bufferW : bufferH;

    const auto& batches = canvas.batches();
    for (size_t i = begin; i < end; ++i) {
        const auto& batch = batches[i];
        // 空批直接跳过；kBlur 标记批由离屏路径处理（模糊未启用/降级时忽略）。
        if (batch.vertexCount == 0 || batch.effect == ui::BatchEffect::kBlur) {
            continue;
        }
        const bool sdf = batch.effect == ui::BatchEffect::kSdf;
        const gpu::PipelineKind kind =
            sdf ? gpu::PipelineKind::kSdf : gpu::PipelineKind::kMain;
        if (kind != boundPipeline) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              uiPipeline_.pipeline(kind));
            boundPipeline = kind;
        }

        // 每批推一次 push constant（mvp 恒定，开销可忽略）；SDF 批追加 32B
        // 效果参数（矩形/半径/羽化/模式），片元阶段据此算距离场覆盖率。
        if (sdf && batch.paramsIndex < canvas.effectParams().size()) {
            const ui::EffectParams& p = canvas.effectParams()[batch.paramsIndex];
            PushConstants pc{};
            pc.mvp = mvp;
            pc.effectRect[0] = p.x; pc.effectRect[1] = p.y;
            pc.effectRect[2] = p.w; pc.effectRect[3] = p.h;
            pc.effectParams[0] = p.radius; pc.effectParams[1] = p.blur;
            pc.effectParams[2] = p.mode;   pc.effectParams[3] = p.pad;
            vkCmdPushConstants(cmd, uiPipeline_.layout(),
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, 96, &pc);
        } else {
            vkCmdPushConstants(cmd, uiPipeline_.layout(), VK_SHADER_STAGE_VERTEX_BIT,
                               0, sizeof(mvp), &mvp);
        }

        // clip 与可视矩形求交（在视觉空间进行，与 app 布局坐标一致）。
        ui::Rect clip = ui::Rect::intersect(batch.clip, {0.0f, 0.0f, visualW, visualH});

        // 需要补偿时，把视觉空间的 clip 旋转到 buffer 像素空间（与 NDC 补偿同向）：
        // 90°: (x,y)->(H-y-h, x)，宽高互换；180°: 两轴各自翻转；270° 为 90° 逆。
        ui::Rect bclip;
        if (compensate && transform == VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR) {
            bclip = {visualH - clip.y - clip.h, clip.x, clip.h, clip.w};
        } else if (compensate && transform == VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR) {
            bclip = {visualW - clip.x - clip.w, visualH - clip.y - clip.h, clip.w, clip.h};
        } else if (compensate && transform == VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR) {
            bclip = {clip.y, visualW - clip.x - clip.w, clip.h, clip.w};
        } else {
            bclip = clip;
        }

        // 转 int32 时 clamp：offset 不小于 0，且 offset+extent 不超出渲染目标。
        int32_t ox = std::max(0, static_cast<int32_t>(bclip.x));
        int32_t oy = std::max(0, static_cast<int32_t>(bclip.y));
        int32_t ex = std::min(static_cast<int32_t>(bclip.x + bclip.w),
                              static_cast<int32_t>(targetExtent.width)) - ox;
        int32_t ey = std::min(static_cast<int32_t>(bclip.y + bclip.h),
                              static_cast<int32_t>(targetExtent.height)) - oy;
        if (ex <= 0 || ey <= 0) {
            continue;
        }

        VkRect2D scissor{};
        scissor.offset = {ox, oy};
        scissor.extent = {static_cast<uint32_t>(ex), static_cast<uint32_t>(ey)};
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        // 绑定本批纹理：0 = 白纹理（纯色/渐变），n = 数据源第 n 号纹理。
        VkDescriptorSet set = textureCache_.descriptorFor(batch.textureId);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, uiPipeline_.layout(),
                                0, 1, &set, 0, nullptr);

        // 画这一批：firstVertex 定位到该批在总顶点数组里的起始偏移。
        vkCmdDraw(cmd, batch.vertexCount, 1, batch.firstVertex, 0);
    }
}

bool Renderer::recordBlurBackdrop(VkCommandBuffer cmd,
                                  const ui::EffectParams& params) {
    const VkExtent2D visual = offscreenEffects_.extent();
    const float sigma = std::min(std::max(params.blur, 0.0f), 32.0f);

    // 模糊覆盖矩形：region 外扩 2σ 余量（V 通道向上/下采样需要），钳到图内。
    const float margin = sigma * 2.0f;
    ui::Rect blurRect = {params.x - margin, params.y - margin,
                         params.w + margin * 2.0f, params.h + margin * 2.0f};
    blurRect = ui::Rect::intersect(
        blurRect, {0.0f, 0.0f, static_cast<float>(visual.width),
                   static_cast<float>(visual.height)});
    if (blurRect.w <= 0.0f || blurRect.h <= 0.0f) {
        // 区域出屏：不模糊不合成；场景 resolve 尚未被动过，调用方直接重开
        // 续画段即可。
        return false;
    }

    // ① 场景 resolve 图：颜色附件 → 可采样。
    barrierImage(cmd, offscreenEffects_.sceneImage(),
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

    // ② H/V 两趟可分离高斯（pingA ← 场景，pingB ← pingA），全部在 1/4
    // 降采样空间进行：σ 同步 ÷4（欠采样残影与带宽一起省掉）；采样输入用
    // 归一化 uv（源图尺寸不同不影响），合成时线性过滤放大回视觉尺寸。
    const VkExtent2D ping = offscreenEffects_.pingExtent();
    const float pingTexelW = 1.0f / static_cast<float>(ping.width);
    const float pingTexelH = 1.0f / static_cast<float>(ping.height);
    const glm::mat4 mvpPing = glm::ortho(
        0.0f, static_cast<float>(ping.width), 0.0f,
        static_cast<float>(ping.height), -1.0f, 1.0f);
    ui::Rect blurRectPing = {blurRect.x * 0.25f, blurRect.y * 0.25f,
                             blurRect.w * 0.25f, blurRect.h * 0.25f};
    blurRectPing = ui::Rect::intersect(
        blurRectPing, {0.0f, 0.0f, static_cast<float>(ping.width),
                       static_cast<float>(ping.height)});
    const float stepPing = sigma / 12.0f; // 5 采样核按 σ≈3 标定，且已 ÷4
    for (int passIndex = 0; passIndex < 2; ++passIndex) {
        const bool vertical = passIndex == 1;
        if (vertical) {
            // pingA 写→读同步（布局已由上一 pass 尾部转好，这里纯内存依赖）。
            barrierImage(cmd, offscreenEffects_.pingImage(false),
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                         VK_ACCESS_SHADER_READ_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        }

        VkRenderPassBeginInfo blurInfo{};
        blurInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        blurInfo.renderPass = offscreenEffects_.blurPass();
        blurInfo.framebuffer = offscreenEffects_.blurFramebuffer(vertical);
        blurInfo.renderArea.offset = {0, 0};
        blurInfo.renderArea.extent = ping;
        vkCmdBeginRenderPass(cmd, &blurInfo, VK_SUBPASS_CONTENTS_INLINE);

        VkViewport viewport{};
        viewport.width = static_cast<float>(ping.width);
        viewport.height = static_cast<float>(ping.height);
        viewport.maxDepth = 1.0f;
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        VkRect2D scissor{};
        scissor.offset = {std::max(0, static_cast<int32_t>(blurRectPing.x)),
                          std::max(0, static_cast<int32_t>(blurRectPing.y))};
        scissor.extent = {static_cast<uint32_t>(blurRectPing.w),
                          static_cast<uint32_t>(blurRectPing.h)};
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          offscreenEffects_.blurPipeline());
        PushConstants pc{};
        pc.mvp = mvpPing;
        pc.effectRect[0] = blurRectPing.x; pc.effectRect[1] = blurRectPing.y;
        pc.effectRect[2] = blurRectPing.w; pc.effectRect[3] = blurRectPing.h;
        pc.effectParams[0] = vertical ? 0.0f : stepPing * pingTexelW;
        pc.effectParams[1] = vertical ? stepPing * pingTexelH : 0.0f;
        pc.effectExtra[0] = pingTexelW; pc.effectExtra[1] = pingTexelH;
        vkCmdPushConstants(cmd, uiPipeline_.layout(),
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(pc), &pc);
        VkDescriptorSet input =
            vertical ? offscreenEffects_.pingSet(false)
                     : offscreenEffects_.sceneSet();
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                uiPipeline_.layout(), 0, 1, &input, 0, nullptr);
        vkCmdDraw(cmd, 6, 1, 0, 0);
        vkCmdEndRenderPass(cmd);
    }

    // pingB 写→读同步（随后续画段的合成 quad 采样它）。
    barrierImage(cmd, offscreenEffects_.pingImage(true),
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

    // ③ 场景 resolve 图回到颜色附件布局：续画段（loadOp=LOAD）接着画。
    barrierImage(cmd, offscreenEffects_.sceneImage(),
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    return true;
}

void Renderer::recordCommandBuffer(VkCommandBuffer cmd, uint32_t imageIndex, const ui::Canvas& canvas) {
    const VkExtent2D extent = swapchain_.extent();
    const VkSampleCountFlagBits msaaSamples = swapchain_.msaaSamples();
    const VkSurfaceTransformFlagBitsKHR surfaceTransform = swapchain_.surfaceTransform();

    // 把一帧的绘制命令录进可复用的主命令缓冲。
    // begin 开始录制：缓冲进入"录制态"，之后的 vkCmd* 调用都被录进去。
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(cmd, &beginInfo);

    // 纹理上传先行：数据源长出新纹理（atlas 页/业务位图）就补建
    // GPU 对象，待上传的纹理（含首次的白纹理）经中转缓冲整张拷入。
    // 必须发生在 render pass 之外。
    textureCache_.ensureStoreTextures();
    textureCache_.uploadPendingTextures(cmd, currentFrame_);

    // 布局/视觉尺寸（app 坐标空间 = 用户实际看到的方向）。
    const float bufferW = static_cast<float>(extent.width);
    const float bufferH = static_cast<float>(extent.height);
    // preTransform 为 90/270 度时，createSwapchain() 已保证 buffer 使用旋转前
    // 尺寸；这里把宽高换回用户实际看到的布局尺寸，并补偿呈现变换。
    const bool rotate90or270 = (surfaceTransform == VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR ||
                                surfaceTransform == VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR);
    const bool compensate = surfaceTransform != VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    const float visualW = rotate90or270 ? bufferH : bufferW;
    const float visualH = rotate90or270 ? bufferW : bufferH;

    // 像素坐标 → NDC 的正交投影（原点在左上角，y 向下，基于视觉尺寸）。
    // 注意 glm::ortho 第三/四参数是 bottom/top：Vulkan 正 viewport 高度下
    // NDC +y 朝 framebuffer 下方，要 y 向下需传 bottom=0、top=height。
    glm::mat4 mvpDirect = glm::ortho(0.0f, visualW, 0.0f, visualH, -1.0f, 1.0f);

    // 呈现旋转补偿：系统把 buffer 按 currentTransform 旋转上屏，这里预先把
    // NDC 反向旋转，上屏后内容回到正立方向（90/180/270 是精确换轴）。
    if (compensate && surfaceTransform == VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR) {
        // (x, y) -> (-y, x)
        const glm::mat4 rot( 0.0f, 1.0f, 0.0f, 0.0f,
                            -1.0f, 0.0f, 0.0f, 0.0f,
                             0.0f, 0.0f, 1.0f, 0.0f,
                             0.0f, 0.0f, 0.0f, 1.0f);
        mvpDirect = rot * mvpDirect;
    } else if (compensate && surfaceTransform == VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR) {
        // (x, y) -> (-x, -y)
        const glm::mat4 rot(-1.0f,  0.0f, 0.0f, 0.0f,
                             0.0f, -1.0f, 0.0f, 0.0f,
                             0.0f,  0.0f, 1.0f, 0.0f,
                             0.0f,  0.0f, 0.0f, 1.0f);
        mvpDirect = rot * mvpDirect;
    } else if (compensate && surfaceTransform == VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR) {
        // (x, y) -> (y, -x)
        const glm::mat4 rot(0.0f, -1.0f, 0.0f, 0.0f,
                            1.0f,  0.0f, 0.0f, 0.0f,
                            0.0f,  0.0f, 1.0f, 0.0f,
                            0.0f,  0.0f, 0.0f, 1.0f);
        mvpDirect = rot * mvpDirect;
    }
    // 离屏场景按视觉方向绘制（不做补偿），纯正交投影；补偿集中在末段 blit。
    const glm::mat4 mvpScene = glm::ortho(0.0f, visualW, 0.0f, visualH, -1.0f, 1.0f);

    // 主 render pass（swapchain）的开始：直通路径与离屏末段 blit 共用。
    auto beginMainPass = [&] {
        VkRenderPassBeginInfo renderPassInfo{};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        renderPassInfo.renderPass = swapchain_.renderPass();
        renderPassInfo.framebuffer = swapchain_.framebuffer(imageIndex);
        renderPassInfo.renderArea.offset = {0, 0};
        renderPassInfo.renderArea.extent = extent;
        // 清屏色：深蓝灰；MSAA 时两个附件（呈现图 + 多采样图）各一份。
        VkClearValue clearColors[2] = {
            {{{0.06f, 0.06f, 0.09f, 1.0f}}},
            {{{0.06f, 0.06f, 0.09f, 1.0f}}},
        };
        renderPassInfo.clearValueCount =
            msaaSamples != VK_SAMPLE_COUNT_1_BIT ? 2 : 1;
        renderPassInfo.pClearValues = clearColors;
        vkCmdBeginRenderPass(cmd, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
    };

    const auto& batches = canvas.batches();

    if (!canvas.hasBlur() || !offscreenEffects_.ready()) {
        // 直通路径：内容直画 swapchain（SDF 批在批内换管线，零离屏开销）。
        beginMainPass();
        recordBatchRange(cmd, canvas, 0, batches.size(), mvpDirect, extent,
                         surfaceTransform);
        // 结束 render pass：同时触发附件布局转换到 PRESENT_SRC_KHR。
        vkCmdEndRenderPass(cmd);
        vkEndCommandBuffer(cmd);
        return;
    }

    // ---- 离屏路径：帧内含背景模糊，整帧先画进场景纹理 ----
    const VkExtent2D visualExtent = {static_cast<uint32_t>(visualW),
                                     static_cast<uint32_t>(visualH)};
    VkClearValue sceneClear[2] = {
        {{{0.06f, 0.06f, 0.09f, 1.0f}}},
        {{{0.06f, 0.06f, 0.09f, 1.0f}}},
    };
    auto beginScenePass = [&](bool load) {
        VkRenderPassBeginInfo info{};
        info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        info.renderPass = offscreenEffects_.scenePass(load);
        info.framebuffer = offscreenEffects_.sceneFramebuffer();
        info.renderArea.offset = {0, 0};
        info.renderArea.extent = visualExtent;
        info.clearValueCount = msaaSamples != VK_SAMPLE_COUNT_1_BIT ? 2 : 1;
        info.pClearValues = sceneClear;
        vkCmdBeginRenderPass(cmd, &info, VK_SUBPASS_CONTENTS_INLINE);
    };

    size_t segmentBegin = 0;
    // 跨帧同步：离屏三图（场景 resolve + ping-pong）是单实例，而渲染双帧
    // 在飞——上一帧末段 blit/合成可能仍在 GPU 上采样这些图，本帧场景段
    // 若直接覆写就会产生重影（滚动时尤其明显）。帧首插一道同布局执行
    // 屏障（同队列屏障对先前提交的命令生效），把本帧写入排在其采样之后。
    if (offscreenUsedOnce_) {
        for (int i = 0; i < 3; ++i) {
            VkImage image = i == 0 ? offscreenEffects_.sceneImage()
                                   : offscreenEffects_.pingImage(i == 2);
            barrierImage(cmd, image,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_ACCESS_SHADER_READ_BIT,
                         VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        }
    }
    offscreenUsedOnce_ = true;
    beginScenePass(false);
    for (size_t i = 0; i < batches.size(); ++i) {
        const auto& batch = batches[i];
        if (batch.effect != ui::BatchEffect::kBlur ||
            batch.paramsIndex >= canvas.effectParams().size()) {
            continue;
        }
        const ui::EffectParams& p = canvas.effectParams()[batch.paramsIndex];
        // ① marker 之前的内容画进场景（本段在场景 pass 内）。
        recordBatchRange(cmd, canvas, segmentBegin, i, mvpScene, visualExtent,
                         VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR);
        vkCmdEndRenderPass(cmd);
        // ② 高斯 ping-pong（pass 外）；区域出屏则只续画不合成。
        const bool blurred = recordBlurBackdrop(cmd, p);
        // ③ 续画段重开（LOAD 保留场景内容），先合成模糊结果再画后续。
        beginScenePass(true);
        if (blurred) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              offscreenEffects_.sdfBlitPipeline());
            VkViewport viewport{};
            viewport.width = static_cast<float>(visualExtent.width);
            viewport.height = static_cast<float>(visualExtent.height);
            viewport.maxDepth = 1.0f;
            vkCmdSetViewport(cmd, 0, 1, &viewport);

            PushConstants pc{};
            pc.mvp = mvpScene;
            pc.effectRect[0] = p.x; pc.effectRect[1] = p.y;
            pc.effectRect[2] = p.w; pc.effectRect[3] = p.h;
            pc.effectParams[0] = p.radius; pc.effectParams[1] = 0.0f;
            pc.effectParams[2] = 2.0f; // SDF mode=2：模糊合成（过渡带外移）
            pc.effectExtra[0] = 1.0f / visualW; pc.effectExtra[1] = 1.0f / visualH;
            vkCmdPushConstants(cmd, uiPipeline_.layout(),
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(pc), &pc);

            ui::Rect compClip = ui::Rect::intersect(batch.clip,
                                                    {0.0f, 0.0f, visualW, visualH});
            VkRect2D scissor{};
            scissor.offset = {std::max(0, static_cast<int32_t>(compClip.x)),
                              std::max(0, static_cast<int32_t>(compClip.y))};
            scissor.extent = {static_cast<uint32_t>(compClip.w),
                              static_cast<uint32_t>(compClip.h)};
            vkCmdSetScissor(cmd, 0, 1, &scissor);

            VkDescriptorSet blurred = offscreenEffects_.pingSet(true);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    uiPipeline_.layout(), 0, 1, &blurred, 0,
                                    nullptr);
            vkCmdDraw(cmd, 6, 1, 0, 0);
        }
        segmentBegin = i + 1;
    }
    // marker 之后的剩余内容画进场景末段。
    recordBatchRange(cmd, canvas, segmentBegin, batches.size(), mvpScene,
                     visualExtent, VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR);
    vkCmdEndRenderPass(cmd);

    // ④ 场景图转可采样，末段全屏 blit 上屏（旋转补偿在 mvpDirect 里）。
    barrierImage(cmd, offscreenEffects_.sceneImage(),
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

    beginMainPass();
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      offscreenEffects_.blitPipeline());
    VkViewport viewport{};
    viewport.width = static_cast<float>(extent.width);
    viewport.height = static_cast<float>(extent.height);
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    VkRect2D fullScissor{};
    fullScissor.extent = extent;
    vkCmdSetScissor(cmd, 0, 1, &fullScissor);

    PushConstants pc{};
    pc.mvp = mvpDirect;
    pc.effectRect[0] = 0.0f; pc.effectRect[1] = 0.0f;
    pc.effectRect[2] = visualW; pc.effectRect[3] = visualH;
    pc.effectExtra[0] = 1.0f / visualW; pc.effectExtra[1] = 1.0f / visualH;
    vkCmdPushConstants(cmd, uiPipeline_.layout(),
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(pc), &pc);
    VkDescriptorSet sceneSet = offscreenEffects_.sceneSet();
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            uiPipeline_.layout(), 0, 1, &sceneSet, 0, nullptr);
    vkCmdDraw(cmd, 6, 1, 0, 0);
    vkCmdEndRenderPass(cmd);

    // 结束录制，缓冲才可提交。
    vkEndCommandBuffer(cmd);
}

} // namespace evk
