/**
 * @file ui_pipeline.cpp
 * @brief 唯一 UI 图形管线实现：pipeline layout 与 graphics pipeline 的创建。
 */
#include "evk/gpu/ui_pipeline.h"

// VulkanContext：设备句柄与 shader module 创建。
#include "evk/gpu/vulkan_context.h"
// 预编译 SPIR-V 字节码（assets::ui_vert_spv / ui_frag_spv），编译期内嵌进二进制，免运行时读文件。
#include "evk/assets/ui_shaders.h"
// EVK_LOGE 日志宏：spdlog 封装，全局统一的日志入口。
#include "evk/log.h"
// ui::UiVertex：顶点输入布局的 stride 来源。
#include "evk/ui/paint_canvas.h"

namespace evk::gpu {

UiPipeline::UiPipeline(VulkanContext& context) : context_(context) {}

bool UiPipeline::create(VkRenderPass renderPass, VkExtent2D extent,
                        VkSampleCountFlagBits msaaSamples,
                        VkDescriptorSetLayout descriptorSetLayout) {
    const VkDevice device = context_.device();

    // 这个示例的管线是固定的。viewport 和 scissor 保持动态，所以缩放时只需重录命令缓冲。
    // 从内嵌字节码建 vert / frag 两个 shader module；管线建完即可销毁（见函数尾）。
    VkShaderModule vertModule = context_.createShaderModule(
        assets::ui_vert_spv, sizeof(assets::ui_vert_spv));
    VkShaderModule fragModule = context_.createShaderModule(
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
    // stride = 32 字节：UiVertex = vec2 位置 + vec4 颜色 + vec2 纹理坐标 = 8 个 float。
    bindingDesc.stride = sizeof(ui::UiVertex);
    // inputRate = VERTEX 表示逐顶点推进（另一种是 INSTANCE，用于实例化渲染）。
    bindingDesc.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    // attribute 描述 shader 里每个输入变量在顶点数据里的位置与格式。
    VkVertexInputAttributeDescription attrs[3];
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
    // attribute 2 对应 layout(location=2) 的 vec2 纹理坐标（文字四边形指向 atlas）。
    attrs[2].binding = 0;
    attrs[2].location = 2;
    attrs[2].format = VK_FORMAT_R32G32_SFLOAT;
    attrs[2].offset = sizeof(float) * 6;

    // 顶点输入状态 = binding + attribute 的总装，管线的固定功能阶段之一。
    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &bindingDesc;
    vertexInput.vertexAttributeDescriptionCount = 3;
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
    viewport.width = static_cast<float>(extent.width);
    viewport.height = static_cast<float>(extent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;

    // scissor 把绘制裁剪到矩形内；同样只是占位，每批绘制时动态设置。
    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = extent;

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

    // 多重采样状态：rasterizationSamples 必须与 renderPass 附件声明的一致
    // （msaaSamples，移动端通常 4x）。每个像素跑 N 次覆盖测试，边缘像素
    // 得到 1/N 精度的覆盖率——这就是 MSAA 抗锯齿的全部魔法。
    // sampleShading 保持关闭：片段着色器每像素仍只跑一次（采样结果广播到
    // 各采样点），文字的灰度抗锯齿来自字形 atlas 本身，无需逐采样着色。
    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.sampleShadingEnable = VK_FALSE;
    multisampling.rasterizationSamples = msaaSamples;

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
    // set 0（组合图像采样，片元阶段）就是批次绑定的白纹理 / 字形 atlas。
    VkDescriptorSetLayout setLayouts[] = {descriptorSetLayout};
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = setLayouts;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushConstant;

    // 布局创建失败也要先销毁两个 shader module 再返回，避免泄漏。
    if (vkCreatePipelineLayout(device, &layoutInfo, nullptr, &pipelineLayout_) != VK_SUCCESS) {
        EVK_LOGE("vkCreatePipelineLayout failed");
        vkDestroyShaderModule(device, vertModule, nullptr);
        vkDestroyShaderModule(device, fragModule, nullptr);
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
    pipelineInfo.renderPass = renderPass;
    pipelineInfo.subpass = 0;

    // vkCreateGraphicsPipelines 是最贵的调用之一：这里触发 SPIR-V 到 GPU 指令的真正编译；
    // 第二参数是管线缓存（可加速重复创建），VK_NULL_HANDLE 表示不用。
    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &graphicsPipeline_) != VK_SUCCESS) {
        EVK_LOGE("vkCreateGraphicsPipelines failed");
        vkDestroyShaderModule(device, vertModule, nullptr);
        vkDestroyShaderModule(device, fragModule, nullptr);
        return false;
    }

    // 管线建完后 SPIR-V 已被"消化"，shader module 可即刻销毁，这是规范允许的用法。
    vkDestroyShaderModule(device, vertModule, nullptr);
    vkDestroyShaderModule(device, fragModule, nullptr);
    return true;
}

void UiPipeline::destroy() {
    const VkDevice device = context_.device();
    if (graphicsPipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, graphicsPipeline_, nullptr);
        graphicsPipeline_ = VK_NULL_HANDLE;
    }
    if (pipelineLayout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, pipelineLayout_, nullptr);
        pipelineLayout_ = VK_NULL_HANDLE;
    }
}

} // namespace evk::gpu
