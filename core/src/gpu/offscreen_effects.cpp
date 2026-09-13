/**
 * @file offscreen_effects.cpp
 * @brief 离屏效果设施实现：场景纹理（MSAA + resolve）、高斯 ping-pong、
 *        render pass 组与无顶点输入小管线。
 */
#include "evk/gpu/offscreen_effects.h"

// VulkanContext：设备句柄、显存类型查找与 shader module 创建。
#include "evk/gpu/vulkan_context.h"
// 预编译 SPIR-V：blur_*（fullscreen.vert + blur.frag）是本模块主力；
// ui_frag / ui_sdf_frag 复用为末段 blit 与圆角合成的片元着色器。
#include "evk/assets/ui_shaders.h"
#include "evk/assets/ui_sdf_shaders.h"
#include "evk/assets/blur_shaders.h"
// EVK_LOGE 日志宏。
#include "evk/log.h"

// std::max：ping-pong 降采样尺寸下限钳制。
#include <algorithm>

namespace evk::gpu {

OffscreenEffects::OffscreenEffects(VulkanContext& context) : context_(context) {}

bool OffscreenEffects::create(VkExtent2D visualExtent, VkFormat format,
                              VkSampleCountFlagBits msaaSamples,
                              VkDescriptorSetLayout textureSetLayout,
                              VkPipelineLayout sharedLayout) {
    destroy();
    extent_ = visualExtent;
    msaaSamples_ = msaaSamples;

    // 场景 resolve 图：单采样、可采样（离屏内容的落地与模糊输入）。
    const VkImageUsageFlags kSceneUsage =
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (!createImage(extent_, format, VK_SAMPLE_COUNT_1_BIT, kSceneUsage,
                     &sceneResolveImage_, &sceneResolveMemory_,
                     &sceneResolveView_)) {
        return false;
    }
    // MSAA 场景图：内容跨段存活（load 段续画），不能 TRANSIENT。
    if (msaaSamples_ != VK_SAMPLE_COUNT_1_BIT &&
        !createImage(extent_, format, msaaSamples_,
                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, &sceneMsaaImage_,
                     &sceneMsaaMemory_, &sceneMsaaView_)) {
        return false;
    }
    // 模糊 ping-pong：单采样、可采样、1/4 降采样（σ 随之 ÷4，小核够用且省带宽）。
    pingExtent_ = {std::max(1u, extent_.width / 4),
                   std::max(1u, extent_.height / 4)};
    for (int i = 0; i < 2; ++i) {
        if (!createImage(pingExtent_, format, VK_SAMPLE_COUNT_1_BIT, kSceneUsage,
                         &pingImage_[i], &pingMemory_[i], &pingView_[i])) {
            return false;
        }
    }

    if (!createScenePasses(format, msaaSamples_) || !createBlurPass(format) ||
        !createFramebuffers() || !createDescriptors(textureSetLayout)) {
        return false;
    }
    // 管线依赖主 render pass 句柄，由调用方随后经 createPipelines 完成。
    return true;
}

bool OffscreenEffects::createImage(VkExtent2D extent, VkFormat format,
                                   VkSampleCountFlagBits samples,
                                   VkImageUsageFlags usage, VkImage* image,
                                   VkDeviceMemory* memory, VkImageView* view) {
    const VkDevice device = context_.device();

    // 图像 → 查显存需求 → 分配（设备本地）→ 绑定 → 建视图，与纹理同套路。
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent = {extent.width, extent.height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = samples;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = usage;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device, &imageInfo, nullptr, image) != VK_SUCCESS) {
        EVK_LOGE("offscreen vkCreateImage failed");
        return false;
    }

    VkMemoryRequirements memReq;
    vkGetImageMemoryRequirements(device, *image, &memReq);
    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex = context_.findMemoryType(
        memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(device, &allocInfo, nullptr, memory) != VK_SUCCESS ||
        vkBindImageMemory(device, *image, *memory, 0) != VK_SUCCESS) {
        EVK_LOGE("offscreen image memory failed");
        return false;
    }

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = *image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;
    if (vkCreateImageView(device, &viewInfo, nullptr, view) != VK_SUCCESS) {
        EVK_LOGE("offscreen vkCreateImageView failed");
        return false;
    }
    return true;
}

bool OffscreenEffects::createScenePasses(VkFormat format,
                                         VkSampleCountFlagBits msaaSamples) {
    const bool msaa = msaaSamples != VK_SAMPLE_COUNT_1_BIT;
    for (int load = 0; load < 2; ++load) {
        // 附件 [0]：resolve 图（单采样落地）；[1]：MSAA 场景图（真正的绘制目标）。
        // 段内画进 MSAA 图，段尾 resolve 到 [0]；load 段保留已有内容续画。
        // resolve 图在段内不被读取（只在段间被模糊采样），loadOp 恒 DONT_CARE。
        VkAttachmentDescription resolveAttachment{};
        resolveAttachment.format = format;
        resolveAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
        resolveAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        resolveAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        resolveAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        resolveAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        // load 段开始时它刚被模糊采样完（SHADER_READ 已屏障回 COLOR_ATTACHMENT）；
        // 首段（clear）不关心旧内容。
        resolveAttachment.initialLayout =
            load ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                 : VK_IMAGE_LAYOUT_UNDEFINED;
        resolveAttachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkAttachmentDescription msaaAttachment{};
        msaaAttachment.format = format;
        msaaAttachment.samples = msaaSamples;
        msaaAttachment.loadOp = load ? VK_ATTACHMENT_LOAD_OP_LOAD
                                     : VK_ATTACHMENT_LOAD_OP_CLEAR;
        // 内容跨段存活（后续 load 段续画），必须 STORE。
        msaaAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        msaaAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        msaaAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        msaaAttachment.initialLayout =
            load ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                 : VK_IMAGE_LAYOUT_UNDEFINED;
        msaaAttachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        // 单采样时场景图本身就是 resolve 图：一个附件身兼绘制与采样。
        VkAttachmentDescription singleAttachment = resolveAttachment;
        singleAttachment.loadOp = load ? VK_ATTACHMENT_LOAD_OP_LOAD
                                       : VK_ATTACHMENT_LOAD_OP_CLEAR;

        VkAttachmentDescription attachments[2] = {singleAttachment, msaaAttachment};

        VkAttachmentReference colorRef{};
        colorRef.attachment = msaa ? 1 : 0;
        colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkAttachmentReference resolveRef{};
        resolveRef.attachment = 0;
        resolveRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorRef;
        subpass.pResolveAttachments = msaa ? &resolveRef : nullptr;

        VkSubpassDependency dependency{};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0;
        dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.srcAccessMask = 0;
        dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        createInfo.attachmentCount = msaa ? 2 : 1;
        createInfo.pAttachments = attachments;
        createInfo.subpassCount = 1;
        createInfo.pSubpasses = &subpass;
        createInfo.dependencyCount = 1;
        createInfo.pDependencies = &dependency;

        VkRenderPass* target = load ? &sceneLoadPass_ : &sceneClearPass_;
        if (vkCreateRenderPass(context_.device(), &createInfo, nullptr, target) !=
            VK_SUCCESS) {
            EVK_LOGE("offscreen scene render pass failed");
            return false;
        }
    }
    return true;
}

bool OffscreenEffects::createBlurPass(VkFormat format) {
    // 模糊通道：整幅目标被覆盖写入，loadOp DONT_CARE；写完即被下一趟采样，
    // finalLayout 直接转 SHADER_READ_ONLY。
    VkAttachmentDescription attachment{};
    attachment.format = format;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

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
    createInfo.pAttachments = &attachment;
    createInfo.subpassCount = 1;
    createInfo.pSubpasses = &subpass;
    createInfo.dependencyCount = 1;
    createInfo.pDependencies = &dependency;

    if (vkCreateRenderPass(context_.device(), &createInfo, nullptr, &blurPass_) !=
        VK_SUCCESS) {
        EVK_LOGE("offscreen blur render pass failed");
        return false;
    }
    return true;
}

bool OffscreenEffects::createFramebuffers() {
    const VkDevice device = context_.device();
    const bool msaa = msaaSamples_ != VK_SAMPLE_COUNT_1_BIT;

    VkImageView sceneAttachments[] = {sceneResolveView_, sceneMsaaView_};
    VkFramebufferCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    createInfo.renderPass = sceneClearPass_;
    createInfo.attachmentCount = msaa ? 2 : 1;
    createInfo.pAttachments = sceneAttachments;
    createInfo.width = extent_.width;
    createInfo.height = extent_.height;
    createInfo.layers = 1;
    if (vkCreateFramebuffer(device, &createInfo, nullptr, &sceneFramebuffer_) !=
        VK_SUCCESS) {
        EVK_LOGE("offscreen scene framebuffer failed");
        return false;
    }

    for (int i = 0; i < 2; ++i) {
        createInfo.renderPass = blurPass_;
        createInfo.attachmentCount = 1;
        createInfo.pAttachments = &pingView_[i];
        createInfo.width = pingExtent_.width;
        createInfo.height = pingExtent_.height;
        if (vkCreateFramebuffer(device, &createInfo, nullptr,
                                &pingFramebuffer_[i]) != VK_SUCCESS) {
            EVK_LOGE("offscreen blur framebuffer failed");
            return false;
        }
    }
    return true;
}

bool OffscreenEffects::createDescriptors(
    VkDescriptorSetLayout textureSetLayout) {
    const VkDevice device = context_.device();

    // 独立采样器：线性过滤 + clamp-to-edge（模糊边缘不折回），不用 mip。
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.maxLod = 0.0f;
    if (vkCreateSampler(device, &samplerInfo, nullptr, &sampler_) != VK_SUCCESS) {
        EVK_LOGE("offscreen vkCreateSampler failed");
        return false;
    }

    // 独立小池：场景图 + pingA/pingB 共 3 个 set（留 1 余量）。
    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = 4;
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 4;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &descriptorPool_) !=
        VK_SUCCESS) {
        EVK_LOGE("offscreen vkCreateDescriptorPool failed");
        return false;
    }

    VkDescriptorSetLayout layouts[3] = {textureSetLayout, textureSetLayout,
                                        textureSetLayout};
    VkDescriptorSet sets[3];
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = descriptorPool_;
    allocInfo.descriptorSetCount = 3;
    allocInfo.pSetLayouts = layouts;
    if (vkAllocateDescriptorSets(device, &allocInfo, sets) != VK_SUCCESS) {
        EVK_LOGE("offscreen vkAllocateDescriptorSets failed");
        return false;
    }
    sceneSet_ = sets[0];
    pingSet_[0] = sets[1];
    pingSet_[1] = sets[2];

    VkImageView views[3] = {sceneResolveView_, pingView_[0], pingView_[1]};
    for (int i = 0; i < 3; ++i) {
        VkDescriptorImageInfo imageInfo{};
        imageInfo.sampler = sampler_;
        imageInfo.imageView = views[i];
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = sets[i];
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &imageInfo;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    }
    return true;
}

bool OffscreenEffects::createPipelines(VkRenderPass mainRenderPass,
                                       VkPipelineLayout sharedLayout,
                                       VkSampleCountFlagBits msaaSamples) {
    const VkDevice device = context_.device();

    VkShaderModule fullscreenVert = context_.createShaderModule(
        assets::blur_vert_spv, sizeof(assets::blur_vert_spv));
    VkShaderModule blurFrag = context_.createShaderModule(
        assets::blur_frag_spv, sizeof(assets::blur_frag_spv));
    VkShaderModule uiFrag = context_.createShaderModule(
        assets::ui_frag_spv, sizeof(assets::ui_frag_spv));
    VkShaderModule sdfFrag = context_.createShaderModule(
        assets::ui_sdf_frag_spv, sizeof(assets::ui_sdf_frag_spv));
    if (fullscreenVert == VK_NULL_HANDLE || blurFrag == VK_NULL_HANDLE ||
        uiFrag == VK_NULL_HANDLE || sdfFrag == VK_NULL_HANDLE) {
        return false;
    }

    // 无顶点输入：quad 顶点由 gl_VertexIndex 在 shader 内生成。
    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkViewport viewport{};
    viewport.width = static_cast<float>(extent_.width);
    viewport.height = static_cast<float>(extent_.height);
    viewport.maxDepth = 1.0f;
    VkRect2D scissor{};
    scissor.extent = extent_;
    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.pViewports = &viewport;
    viewportState.scissorCount = 1;
    viewportState.pScissors = &scissor;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;

    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blendAttachment.blendEnable = VK_TRUE;
    blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
    VkPipelineColorBlendStateCreateInfo blending{};
    blending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blending.attachmentCount = 1;
    blending.pAttachments = &blendAttachment;

    VkDynamicState dynamicStates[] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
    };
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynamicStates;

    auto createOne = [&](VkShaderModule frag, VkRenderPass pass,
                         VkSampleCountFlagBits samples, bool blend,
                         VkPipeline* out) -> bool {
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = fullscreenVert;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = frag;
        stages[1].pName = "main";

        multisampling.rasterizationSamples = samples;
        blendAttachment.blendEnable = blend ? VK_TRUE : VK_FALSE;

        VkGraphicsPipelineCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        info.stageCount = 2;
        info.pStages = stages;
        info.pVertexInputState = &vertexInput;
        info.pInputAssemblyState = &inputAssembly;
        info.pViewportState = &viewportState;
        info.pRasterizationState = &rasterizer;
        info.pMultisampleState = &multisampling;
        info.pColorBlendState = &blending;
        info.pDynamicState = &dynamicState;
        info.layout = sharedLayout;
        info.renderPass = pass;
        info.subpass = 0;
        return vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info,
                                         nullptr, out) == VK_SUCCESS;
    };

    // 模糊（ping-pong 单采样、覆盖写不需要混合）；圆角合成回场景（MSAA，
    // 混合）；末段全屏 blit 上 swapchain 主通道（MSAA，混合无害）。
    const bool ok =
        createOne(blurFrag, blurPass_, VK_SAMPLE_COUNT_1_BIT, false,
                  &blurPipeline_) &&
        createOne(sdfFrag, sceneLoadPass_, msaaSamples, true,
                  &sdfBlitPipeline_) &&
        createOne(uiFrag, mainRenderPass, msaaSamples, true, &blitPipeline_);
    if (!ok) {
        EVK_LOGE("offscreen vkCreateGraphicsPipelines failed");
    }

    vkDestroyShaderModule(device, fullscreenVert, nullptr);
    vkDestroyShaderModule(device, blurFrag, nullptr);
    vkDestroyShaderModule(device, uiFrag, nullptr);
    vkDestroyShaderModule(device, sdfFrag, nullptr);
    if (ok) {
        ready_ = true;
    }
    return ok;
}

void OffscreenEffects::destroy() {
    const VkDevice device = context_.device();
    ready_ = false;

    auto destroyPipeline = [&](VkPipeline& p) {
        if (p != VK_NULL_HANDLE) { vkDestroyPipeline(device, p, nullptr); p = VK_NULL_HANDLE; }
    };
    destroyPipeline(blurPipeline_);
    destroyPipeline(sdfBlitPipeline_);
    destroyPipeline(blitPipeline_);

    if (descriptorPool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device, descriptorPool_, nullptr);
        descriptorPool_ = VK_NULL_HANDLE;
        sceneSet_ = VK_NULL_HANDLE;
        pingSet_[0] = pingSet_[1] = VK_NULL_HANDLE;
    }
    if (sampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(device, sampler_, nullptr);
        sampler_ = VK_NULL_HANDLE;
    }

    auto destroyFramebuffer = [&](VkFramebuffer& fb) {
        if (fb != VK_NULL_HANDLE) { vkDestroyFramebuffer(device, fb, nullptr); fb = VK_NULL_HANDLE; }
    };
    destroyFramebuffer(sceneFramebuffer_);
    destroyFramebuffer(pingFramebuffer_[0]);
    destroyFramebuffer(pingFramebuffer_[1]);

    auto destroyPass = [&](VkRenderPass& pass) {
        if (pass != VK_NULL_HANDLE) { vkDestroyRenderPass(device, pass, nullptr); pass = VK_NULL_HANDLE; }
    };
    destroyPass(sceneClearPass_);
    destroyPass(sceneLoadPass_);
    destroyPass(blurPass_);

    auto destroyImage = [&](VkImage& image, VkDeviceMemory& memory,
                            VkImageView& view) {
        if (view != VK_NULL_HANDLE) { vkDestroyImageView(device, view, nullptr); view = VK_NULL_HANDLE; }
        if (image != VK_NULL_HANDLE) { vkDestroyImage(device, image, nullptr); image = VK_NULL_HANDLE; }
        if (memory != VK_NULL_HANDLE) { vkFreeMemory(device, memory, nullptr); memory = VK_NULL_HANDLE; }
    };
    destroyImage(sceneMsaaImage_, sceneMsaaMemory_, sceneMsaaView_);
    destroyImage(sceneResolveImage_, sceneResolveMemory_, sceneResolveView_);
    for (int i = 0; i < 2; ++i) {
        destroyImage(pingImage_[i], pingMemory_[i], pingView_[i]);
    }
}

} // namespace evk::gpu
