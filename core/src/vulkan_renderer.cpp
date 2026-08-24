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

// 关键宏：让 GLM 的投影矩阵把深度映射到 Vulkan 的 NDC 范围 [0,1]，
// 而不是 OpenGL 默认的 [-1,1]；必须在包含任何 glm 头之前定义才生效。
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
// glm 核心类型（mat4 等），用于构造正交投影矩阵。
#include <glm/glm.hpp>
// glm::ortho 所在的矩阵变换头。
#include <glm/gtc/matrix_transform.hpp>

namespace evk {

/// 单次 render() 内"重建交换链 → 重画"的最大重试次数（见 render() 注释）。
constexpr uint32_t kMaxFrameAttempts = 3;

Renderer::Renderer(IPlatform* platform, gpu::ITextureSource* textureSource)
    : context_(platform),
      textureCache_(context_, textureSource),
      swapchain_(context_),
      uiPipeline_(context_) {}

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
    // ④ 执行设施：命令池、顶点缓冲、命令缓冲、同步原语，与 swapchain 尺寸无关。
    if (!createCommandPool()) return false;
    if (!createVertexBuffers()) return false;
    if (!createCommandBuffers()) return false;
    if (!createSyncObjects()) return false;
    return true;
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

        // 管线与 swapchain 相关资源（framebuffer / renderPass / imageView / swapchain）
        // 各归其模块集中清理。
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
        EVK_LOGE("vkCreateCommandPool failed");
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
        EVK_LOGE("vertex buffer vkCreateBuffer failed");
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
        EVK_LOGE("vertex buffer vkAllocateMemory failed");
        vkDestroyBuffer(device, newBuffer, nullptr);
        return false;
    }

    if (vkBindBufferMemory(device, newBuffer, newMemory, 0) != VK_SUCCESS) {
        EVK_LOGE("vertex buffer vkBindBufferMemory failed");
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
        EVK_LOGE("vertex buffer vkMapMemory failed");
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
        EVK_LOGE("vkAllocateCommandBuffers failed");
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
            EVK_LOGE("createSyncObjects failed");
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
    // 管线配置跟随 swapchain 格式/尺寸/采样数，与 swapchain 资源同批销毁重建。
    uiPipeline_.destroy();
    swapchain_.cleanup();
    swapchain_.create();
    uiPipeline_.create(swapchain_.renderPass(), swapchain_.extent(),
                       swapchain_.msaaSamples(),
                       textureCache_.descriptorSetLayout());
    swapchain_.createFramebuffers();
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
            EVK_LOGE("vkAcquireNextImageKHR failed: {}", static_cast<int>(result));
            return false;
        }

        // acquire 成功后先把本帧顶点写进动态缓冲，再录命令。空 canvas 跳过上传，
        // 命令录制阶段也不会有 draw，只剩清屏帧。
        // ③ 上传顶点：赶在命令录制前把数据写进 HOST_VISIBLE 缓冲。
        if (!canvas.vertices().empty()) {
            if (!uploadVertices(canvas.vertices().data(),
                                static_cast<uint32_t>(canvas.vertices().size()),
                                currentFrame_)) {
                EVK_LOGE("UI vertex upload failed");
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
            EVK_LOGE("vkQueueSubmit failed");
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
            EVK_LOGE("vkQueuePresentKHR failed: {}", static_cast<int>(result));
            return false;
        }

        // ⑦ 轮转帧槽位 0→1→0→1：下一帧换用另一套 semaphore / fence / 命令缓冲。
        currentFrame_ = (currentFrame_ + 1) % gpu::kMaxFramesInFlight;
        return true;
    }

    // 重试耗尽（surface 持续变化中，如快速连续旋转）：放弃本帧不算失败，
    // 后续尺寸事件还会触发渲染，届时继续收敛。
    EVK_LOGW("render: swapchain still out of date after {} attempts, frame skipped",
             kMaxFrameAttempts);
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

    // render pass begin 信息：用哪个 pass、渲染到哪个 framebuffer、范围多大。
    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = swapchain_.renderPass();
    // framebuffer 按 acquire 到的 imageIndex 选：画到将要上屏的那张图。
    renderPassInfo.framebuffer = swapchain_.framebuffer(imageIndex);
    // renderArea 限定渲染区域；全屏渲染就是整个 swapchain 尺寸。
    renderPassInfo.renderArea.offset = {0, 0};
    renderPassInfo.renderArea.extent = extent;

    // 清屏色：深蓝灰 (0.06, 0.06, 0.09)，作用于 render pass 里 loadOp=CLEAR 的附件。
    // MSAA 开启时有两个附件（[0] 呈现图、[1] 多采样图）都是 CLEAR，按附件下标
    // 顺序各给一份；用同一个颜色，resolve 平均后落到屏幕的底色一致。
    VkClearValue clearColors[2] = {
        {{{0.06f, 0.06f, 0.09f, 1.0f}}},
        {{{0.06f, 0.06f, 0.09f, 1.0f}}},
    };
    renderPassInfo.clearValueCount =
        msaaSamples != VK_SAMPLE_COUNT_1_BIT ? 2 : 1;
    renderPassInfo.pClearValues = clearColors;

    // INLINE 表示命令直接录在这条 primary 缓冲里（另一种是执行 secondary 缓冲）。
    vkCmdBeginRenderPass(cmd, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

    // 绑定图形管线：之后的 draw 都用这套着色器与固定功能状态。
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, uiPipeline_.pipeline());

    // 按当前 swapchain 尺寸组一份 viewport（NDC 到像素的映射）。
    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(extent.width);
    viewport.height = static_cast<float>(extent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    // 管线把 viewport 和 scissor 设成动态状态，所以每帧都要重新设置。
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    // 绑定顶点缓冲到槽位 0（对应管线 vertex input 的 binding 0），偏移 0。
    VkBuffer vertexBuffers[] = {vertexBuffers_[currentFrame_]};
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(cmd, 0, 1, vertexBuffers, offsets);

    // 布局/视觉尺寸（app 坐标空间 = 用户实际看到的方向）。
    const float bufferW = static_cast<float>(extent.width);
    const float bufferH = static_cast<float>(extent.height);
    // preTransform 为 90/270 度时，createSwapchain() 已保证 buffer 使用旋转前
    // 尺寸；这里把宽高换回用户实际看到的布局尺寸，并补偿呈现变换。
    const bool rotate90or270 = (surfaceTransform == VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR ||
                                surfaceTransform == VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR);
    const bool compensate = surfaceTransform != VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
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
    if (compensate && surfaceTransform == VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR) {
        // (x, y) -> (-y, x)
        const glm::mat4 rot( 0.0f, 1.0f, 0.0f, 0.0f,
                            -1.0f, 0.0f, 0.0f, 0.0f,
                             0.0f, 0.0f, 1.0f, 0.0f,
                             0.0f, 0.0f, 0.0f, 1.0f);
        mvp = rot * mvp;
    } else if (compensate && surfaceTransform == VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR) {
        // (x, y) -> (-x, -y)
        const glm::mat4 rot(-1.0f,  0.0f, 0.0f, 0.0f,
                             0.0f, -1.0f, 0.0f, 0.0f,
                             0.0f,  0.0f, 1.0f, 0.0f,
                             0.0f,  0.0f, 0.0f, 1.0f);
        mvp = rot * mvp;
    } else if (compensate && surfaceTransform == VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR) {
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
        vkCmdPushConstants(cmd, uiPipeline_.layout(), VK_SHADER_STAGE_VERTEX_BIT,
                           0, sizeof(mvp), &mvp);

        // clip 与屏幕矩形求交（在视觉空间进行，与 app 布局坐标一致）。
        ui::Rect clip = ui::Rect::intersect(batch.clip, {0.0f, 0.0f, visualW, visualH});

        // 需要补偿时，把视觉空间的 clip 旋转到 buffer 像素空间（与上面的 NDC 补偿同向）：
        // 90°: (x,y)->(H-y-h, x)，宽高互换；180°: 两轴各自翻转；
        // 270°: (x,y)->(y, W-x-w)，是 90° 的逆。
        // 无需补偿时 buffer 空间即视觉空间，直接用 clip。
        ui::Rect bclip;
        if (compensate && surfaceTransform == VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR) {
            bclip = {visualH - clip.y - clip.h, clip.x, clip.h, clip.w};
        } else if (compensate && surfaceTransform == VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR) {
            bclip = {visualW - clip.x - clip.w, visualH - clip.y - clip.h, clip.w, clip.h};
        } else if (compensate && surfaceTransform == VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR) {
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
                              static_cast<int32_t>(extent.width)) - ox;
        int32_t ey = std::min(static_cast<int32_t>(bclip.y + bclip.h),
                              static_cast<int32_t>(extent.height)) - oy;
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

        // 绑定本批纹理：0 = 白纹理（纯色/渐变），n = 数据源第 n 号纹理。
        // 同一 (clip, 纹理) 的绘制已在 Canvas 侧合并，这里每批一次绑定。
        VkDescriptorSet set = textureCache_.descriptorFor(batch.textureId);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, uiPipeline_.layout(),
                                0, 1, &set, 0, nullptr);

        // 画这一批：firstVertex 定位到该批在总顶点数组里的起始偏移，
        // 所有批共享同一块顶点缓冲，靠 firstVertex 分段。
        vkCmdDraw(cmd, batch.vertexCount, 1, batch.firstVertex, 0);
    }

    // 结束 render pass：同时触发附件布局转换到 PRESENT_SRC_KHR。
    vkCmdEndRenderPass(cmd);

    // 结束录制，缓冲才可提交。
    vkEndCommandBuffer(cmd);
}

} // namespace evk
