#include "internal.h"

namespace kumo::rhi::vulkan {

Ptr<RenderPipeline> VulkanDevice::createRenderPipeline(const RenderPipelineDesc& desc) {
    KUMO_ASSERT(desc.pushConstantSize <= 128);
    KUMO_ASSERT(desc.sampleCount == 1 || desc.sampleCount == 4);
    if (!desc.vertexShader || !desc.fragmentShader) {
        logError("createRenderPipeline requires vertex and fragment shaders");
        return nullptr;
    }

    std::vector<VkPipelineShaderStageCreateInfo> stages;
    auto* vertex = static_cast<VulkanShaderModule*>(desc.vertexShader.get());
    auto* fragment = static_cast<VulkanShaderModule*>(desc.fragmentShader.get());
    VkPipelineShaderStageCreateInfo vertexStage{};
    vertexStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vertexStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertexStage.module = vertex->handle();
    vertexStage.pName = vertex->entryPoint().c_str();
    stages.push_back(vertexStage);
    VkPipelineShaderStageCreateInfo fragmentStage{};
    fragmentStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fragmentStage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragmentStage.module = fragment->handle();
    fragmentStage.pName = fragment->entryPoint().c_str();
    stages.push_back(fragmentStage);

    std::vector<VkVertexInputBindingDescription> vertexBindings;
    std::vector<VkVertexInputAttributeDescription> vertexAttributes;
    for (std::size_t slot = 0; slot < desc.vertexBuffers.size(); ++slot) {
        const VertexBufferLayout& layout = desc.vertexBuffers[slot];
        VkVertexInputBindingDescription binding{};
        binding.binding = static_cast<std::uint32_t>(slot);
        binding.stride = layout.stride;
        binding.inputRate = layout.stepMode == VertexStepMode::Vertex
                                ? VK_VERTEX_INPUT_RATE_VERTEX
                                : VK_VERTEX_INPUT_RATE_INSTANCE;
        vertexBindings.push_back(binding);
        for (const VertexAttribute& attribute : layout.attributes) {
            VkVertexInputAttributeDescription attr{};
            attr.location = attribute.shaderLocation;
            attr.binding = static_cast<std::uint32_t>(slot);
            attr.format = toVk(attribute.format);
            attr.offset = attribute.offset;
            vertexAttributes.push_back(attr);
        }
    }
    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = static_cast<std::uint32_t>(vertexBindings.size());
    vertexInput.pVertexBindingDescriptions = vertexBindings.data();
    vertexInput.vertexAttributeDescriptionCount =
        static_cast<std::uint32_t>(vertexAttributes.size());
    vertexInput.pVertexAttributeDescriptions = vertexAttributes.data();

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = toVk(desc.topology);

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterization{};
    rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization.cullMode = desc.cullMode == CullMode::None    ? VK_CULL_MODE_NONE
                             : desc.cullMode == CullMode::Front ? VK_CULL_MODE_FRONT_BIT
                                                                : VK_CULL_MODE_BACK_BIT;
    // Negative-height viewport flips framebuffer winding; invert front face so
    // culling matches Metal.
    rasterization.frontFace = desc.frontFace == FrontFace::CCW ? VK_FRONT_FACE_CLOCKWISE
                                                               : VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterization.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = toVkSamples(desc.sampleCount);

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    const bool hasDepth = desc.depthStencil.format != TextureFormat::Undefined;
    depthStencil.depthTestEnable = hasDepth ? VK_TRUE : VK_FALSE;
    depthStencil.depthWriteEnable = desc.depthStencil.depthWriteEnabled ? VK_TRUE : VK_FALSE;
    depthStencil.depthCompareOp = toVk(desc.depthStencil.depthCompare);

    std::vector<VkPipelineColorBlendAttachmentState> blendAttachments;
    for (std::size_t i = 0; i < desc.colorFormats.size(); ++i) {
        VkPipelineColorBlendAttachmentState blend{};
        blend.blendEnable = VK_FALSE;
        blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                               VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        blendAttachments.push_back(blend);
    }
    VkPipelineColorBlendStateCreateInfo colorBlend{};
    colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.attachmentCount = static_cast<std::uint32_t>(blendAttachments.size());
    colorBlend.pAttachments = blendAttachments.data();

    const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamicStates;

    std::vector<VkDescriptorSetLayout> setLayouts;
    for (const Ptr<BindGroupLayout>& bindGroupLayout : desc.bindGroupLayouts) {
        setLayouts.push_back(
            bindGroupLayout ? static_cast<VulkanBindGroupLayout*>(bindGroupLayout.get())->handle()
                            : emptySetLayout_);
    }
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pushRange.offset = 0;
    pushRange.size = desc.pushConstantSize;
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = static_cast<std::uint32_t>(setLayouts.size());
    layoutInfo.pSetLayouts = setLayouts.data();
    layoutInfo.pushConstantRangeCount = desc.pushConstantSize > 0 ? 1u : 0u;
    layoutInfo.pPushConstantRanges = desc.pushConstantSize > 0 ? &pushRange : nullptr;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    if (vkCreatePipelineLayout(device_, &layoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS) {
        logError("createRenderPipeline: pipeline layout failed");
        return nullptr;
    }

    std::vector<VkFormat> colorFormats;
    for (TextureFormat format : desc.colorFormats) {
        colorFormats.push_back(toVk(format));
    }
    VkPipelineRenderingCreateInfo renderingInfo{};
    renderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    renderingInfo.colorAttachmentCount = static_cast<std::uint32_t>(colorFormats.size());
    renderingInfo.pColorAttachmentFormats = colorFormats.data();
    renderingInfo.depthAttachmentFormat =
        hasDepth ? toVk(desc.depthStencil.format) : VK_FORMAT_UNDEFINED;

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.pNext = &renderingInfo;
    pipelineInfo.stageCount = static_cast<std::uint32_t>(stages.size());
    pipelineInfo.pStages = stages.data();
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterization;
    pipelineInfo.pMultisampleState = &multisample;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlend;
    pipelineInfo.pDynamicState = &dynamic;
    pipelineInfo.layout = pipelineLayout;
    pipelineInfo.renderPass = VK_NULL_HANDLE;

    VkPipeline pipeline = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline) !=
        VK_SUCCESS) {
        logError("createRenderPipeline failed");
        vkDestroyPipelineLayout(device_, pipelineLayout, nullptr);
        return nullptr;
    }
    return std::make_shared<VulkanRenderPipeline>(device_, pipeline, pipelineLayout,
                                                  desc.sampleCount);
}

Ptr<ComputePipeline> VulkanDevice::createComputePipeline(const ComputePipelineDesc& desc) {
    KUMO_ASSERT(desc.pushConstantSize <= 128);
    // Zero threadgroup dimensions hang the GPU on backends that dispatch with them.
    KUMO_ASSERT(desc.workgroupSizeX > 0 && desc.workgroupSizeY > 0 && desc.workgroupSizeZ > 0);
    if (!desc.shader) {
        logError("createComputePipeline requires a compute shader");
        return nullptr;
    }
    auto* shader = static_cast<VulkanShaderModule*>(desc.shader.get());

    std::vector<VkDescriptorSetLayout> setLayouts;
    for (const Ptr<BindGroupLayout>& bindGroupLayout : desc.bindGroupLayouts) {
        setLayouts.push_back(
            bindGroupLayout ? static_cast<VulkanBindGroupLayout*>(bindGroupLayout.get())->handle()
                            : emptySetLayout_);
    }
    // Compute pipeline layouts need the COMPUTE stage flag on their push range.
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushRange.offset = 0;
    pushRange.size = desc.pushConstantSize;
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = static_cast<std::uint32_t>(setLayouts.size());
    layoutInfo.pSetLayouts = setLayouts.data();
    layoutInfo.pushConstantRangeCount = desc.pushConstantSize > 0 ? 1u : 0u;
    layoutInfo.pPushConstantRanges = desc.pushConstantSize > 0 ? &pushRange : nullptr;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    if (vkCreatePipelineLayout(device_, &layoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS) {
        logError("createComputePipeline: pipeline layout failed");
        return nullptr;
    }

    VkPipelineShaderStageCreateInfo stage{};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = shader->handle();
    stage.pName = shader->entryPoint().c_str();

    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage = stage;
    pipelineInfo.layout = pipelineLayout;
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline) !=
        VK_SUCCESS) {
        logError("createComputePipeline failed");
        vkDestroyPipelineLayout(device_, pipelineLayout, nullptr);
        return nullptr;
    }
    return std::make_shared<VulkanComputePipeline>(device_, pipeline, pipelineLayout);
}

} // namespace kumo::rhi::vulkan
