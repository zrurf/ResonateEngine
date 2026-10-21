/* The RHI operations over the Vulkan core in context.cpp: resource creation
   and destruction, uploads, and the pass draw path. VMA is implemented in this
   translation unit, the only one that needs it. */

#define VMA_IMPLEMENTATION
#include <vma/vk_mem_alloc.h>

#include "context.h"

#include <cstddef>
#include <cstring>
#include <iostream>

namespace resonate::render_vulkan
{
namespace
{

/* A dead handle, or one retired before this call, is E_INVALID rather than a
   read of somebody else's slot. */
constexpr uint32_t kRetiredKindNone = 0;

uint32_t bufferUsageToVulkan(uint32_t usage)
{
    uint32_t flags = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if ((usage & RESONATE_RENDER_BUFFER_VERTEX) != 0U)
    {
        flags |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    }
    if ((usage & RESONATE_RENDER_BUFFER_INDEX) != 0U)
    {
        flags |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    }
    if ((usage & RESONATE_RENDER_BUFFER_UNIFORM) != 0U)
    {
        flags |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    }
    if ((usage & RESONATE_RENDER_BUFFER_STORAGE) != 0U)
    {
        flags |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    }
    if ((usage & RESONATE_RENDER_BUFFER_TRANSFER_SRC) != 0U)
    {
        flags |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    }
    return flags;
}

uint32_t imageUsageToVulkan(uint32_t usage)
{
    uint32_t flags = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if ((usage & RESONATE_RENDER_IMAGE_COLOR_ATTACHMENT) != 0U)
    {
        flags |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    }
    if ((usage & RESONATE_RENDER_IMAGE_DEPTH_ATTACHMENT) != 0U)
    {
        flags |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    }
    if ((usage & RESONATE_RENDER_IMAGE_SAMPLED) != 0U)
    {
        flags |= VK_IMAGE_USAGE_SAMPLED_BIT;
    }
    return flags;
}

/* Texel sizes of the formats the device supports; block-encoded formats are
   not among them. */
uint32_t formatBytesOf(ResonateRenderFormat format)
{
    switch (format)
    {
        case RESONATE_RENDER_FORMAT_R16G16B16A16_SFLOAT:
            return 8U;
        case RESONATE_RENDER_FORMAT_R32G32_SFLOAT:
            return 8U;
        case RESONATE_RENDER_FORMAT_R32G32B32_SFLOAT:
            return 12U;
        case RESONATE_RENDER_FORMAT_D32_SFLOAT:
            return 4U;
        case RESONATE_RENDER_FORMAT_R8G8B8A8_UNORM:
        case RESONATE_RENDER_FORMAT_R8G8B8A8_SRGB:
        case RESONATE_RENDER_FORMAT_B8G8R8A8_SRGB:
        default:
            return 4U;
    }
}

} // namespace

ResonateStatus Context::createBuffer(const ResonateRenderBufferDesc& desc,
                                     ResonateRenderBuffer* out)
{
    if (device_ == VK_NULL_HANDLE || desc.size_bytes == 0U || desc.usage == 0U)
    {
        return device_ == VK_NULL_HANDLE ? RESONATE_E_STATE : RESONATE_E_INVALID;
    }

    BufferSlot* slot = nullptr;
    const uint32_t index = buffers_.acquire(&slot);
    *slot = BufferSlot{};
    slot->size = desc.size_bytes;

    /* A storage buffer takes a table slot; its handle keeps the pool index.
       Without one it could never be named by a shader. */
    if ((desc.usage & RESONATE_RENDER_BUFFER_STORAGE) != 0U)
    {
        uint32_t table_slot = 0;
        if (!acquireTableSlot(free_storage_slots_, table_slot))
        {
            buffers_.retire(index);
            note(device_failed_, "the storage table is full");
            return RESONATE_E_INVALID;
        }
        slot->table_slot = table_slot;
        slot->in_table = true;
    }

    VkBufferCreateInfo buffer{};
    buffer.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer.size = desc.size_bytes;
    buffer.usage = bufferUsageToVulkan(desc.usage);
    buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocation{};
    if (desc.host_visible != 0U)
    {
        allocation.usage = VMA_MEMORY_USAGE_AUTO;
        allocation.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                           VMA_ALLOCATION_CREATE_MAPPED_BIT;
    }
    else
    {
        allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    }

    VmaAllocationInfo allocation_info{};
    if (vmaCreateBuffer(allocator_, &buffer, &allocation, &slot->buffer, &slot->allocation,
                        &allocation_info) != VK_SUCCESS)
    {
        if (slot->in_table)
        {
            releaseTableSlot(free_storage_slots_, slot->table_slot);
        }
        buffers_.retire(index);
        std::fprintf(stderr, "render-vulkan: buffer creation failed (%llu bytes, usage %x)\n",
                     static_cast<unsigned long long>(desc.size_bytes), desc.usage);
        return RESONATE_E_INTERNAL;
    }
    slot->mapped = allocation_info.pMappedData;
    slot->alive = true;
    const uint64_t handle = handleValue(slot->generation, index);
    if (slot->in_table)
    {
        writeBufferSlot(slot->table_slot, slot->buffer, desc.size_bytes);
    }
    *out = ResonateRenderBuffer{handle};
    return RESONATE_OK;
}

void Context::destroyBuffer(ResonateRenderBuffer handle)
{
    if (handle.value == 0 || device_ == VK_NULL_HANDLE)
    {
        return;
    }
    BufferSlot* slot = buffers_.live(handleIndex(handle.value), handleGeneration(handle.value));
    if (slot == nullptr)
    {
        return;
    }
    frames_[frame_ % kFramesInFlight].graveyard.push_back(
        Retired{kRetiredBuffer, static_cast<void*>(slot->buffer), slot->allocation});
    if (slot->in_table)
    {
        /* The slot is reused by the next storage buffer, which writes its own
           binding; nothing samples a retired one. */
        releaseTableSlot(free_storage_slots_, slot->table_slot);
        slot->in_table = false;
    }
    buffers_.retire(handleIndex(handle.value));
}

ResonateStatus Context::createImage(const ResonateRenderImageDesc& desc, ResonateRenderImage* out)
{
    if (device_ == VK_NULL_HANDLE)
    {
        return RESONATE_E_STATE;
    }
    if (desc.format == RESONATE_RENDER_FORMAT_UNKNOWN || desc.width == 0U || desc.height == 0U ||
        desc.usage == 0U)
    {
        return RESONATE_E_INVALID;
    }

    ImageSlot* slot = nullptr;
    const uint32_t index = images_.acquire(&slot);
    *slot = ImageSlot{};
    if ((desc.usage & RESONATE_RENDER_IMAGE_SAMPLED) != 0U)
    {
        uint32_t table_slot = 0;
        if (!acquireTableSlot(free_image_slots_, table_slot))
        {
            images_.retire(index);
            note(device_failed_, "the image table is full");
            return RESONATE_E_INVALID;
        }
        slot->table_slot = table_slot;
        slot->in_table = true;
    }
    slot->format = formatOf(desc.format);
    slot->format_id = desc.format;
    slot->bytes_per_pixel = formatBytesOf(desc.format);
    slot->width = desc.width;
    slot->height = desc.height;
    slot->depth = (desc.usage & RESONATE_RENDER_IMAGE_DEPTH_ATTACHMENT) != 0U;

    VkImageCreateInfo image{};
    image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image.imageType = VK_IMAGE_TYPE_2D;
    image.format = slot->format;
    image.extent = VkExtent3D{desc.width, desc.height, 1U};
    image.mipLevels = 1;
    image.arrayLayers = 1;
    image.samples = VK_SAMPLE_COUNT_1_BIT;
    image.tiling = VK_IMAGE_TILING_OPTIMAL;
    image.usage = imageUsageToVulkan(desc.usage);
    image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo allocation{};
    allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

    if (vmaCreateImage(allocator_, &image, &allocation, &slot->image, &slot->allocation, nullptr) !=
        VK_SUCCESS)
    {
        if (slot->in_table)
        {
            releaseTableSlot(free_image_slots_, slot->table_slot);
        }
        images_.retire(index);
        note(device_failed_, "image creation failed");
        return RESONATE_E_INTERNAL;
    }

    VkImageViewCreateInfo view{};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = slot->image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = slot->format;
    view.subresourceRange.aspectMask =
        slot->depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    view.subresourceRange.levelCount = 1;
    view.subresourceRange.layerCount = 1;
    if (vkCreateImageView(device_, &view, nullptr, &slot->view) != VK_SUCCESS)
    {
        vmaDestroyImage(allocator_, slot->image, slot->allocation);
        if (slot->in_table)
        {
            releaseTableSlot(free_image_slots_, slot->table_slot);
        }
        images_.retire(index);
        note(device_failed_, "image view creation failed");
        return RESONATE_E_INTERNAL;
    }
    slot->alive = true;
    if (slot->in_table)
    {
        /* The field is a tail addition; an older writer's struct stops short
           of it and means linear. */
        const bool nearest =
            desc.struct_size >= offsetof(ResonateRenderImageDesc, nearest) + sizeof(uint32_t) &&
            desc.nearest != 0U;
        writeImageSlot(slot->table_slot, slot->view, nearest);
    }
    *out = ResonateRenderImage{handleValue(slot->generation, index)};
    return RESONATE_OK;
}

void Context::destroyImage(ResonateRenderImage handle)
{
    if (handle.value == 0 || device_ == VK_NULL_HANDLE)
    {
        return;
    }
    ImageSlot* slot = images_.live(handleIndex(handle.value), handleGeneration(handle.value));
    if (slot == nullptr)
    {
        return;
    }
    Frame& frame = frames_[frame_ % kFramesInFlight];
    /* A cached framebuffer holds this image's view; it goes first, in the same
       deferred batch as the view and the image. */
    for (auto entry = framebuffer_cache_.begin(); entry != framebuffer_cache_.end();)
    {
        if (entry->first.first == handle.value || entry->first.second == handle.value)
        {
            frame.graveyard.push_back(
                Retired{kRetiredFramebuffer, static_cast<void*>(entry->second), nullptr});
            entry = framebuffer_cache_.erase(entry);
        }
        else
        {
            ++entry;
        }
    }
    frame.graveyard.push_back(Retired{kRetiredImageView, static_cast<void*>(slot->view), nullptr});
    frame.graveyard.push_back(
        Retired{kRetiredImage, static_cast<void*>(slot->image), slot->allocation});
    if (slot->in_table)
    {
        /* Reused by the next sampled image, which writes its own binding. */
        releaseTableSlot(free_image_slots_, slot->table_slot);
        slot->in_table = false;
    }
    images_.retire(handleIndex(handle.value));
}

ResonateStatus Context::createSampler(const ResonateRenderSamplerDesc& desc,
                                      ResonateRenderSampler* out)
{
    if (device_ == VK_NULL_HANDLE)
    {
        return RESONATE_E_STATE;
    }

    const VkFilter filter =
        desc.mag_filter == RESONATE_RENDER_FILTER_LINEAR ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    const VkSamplerAddressMode mode = desc.address_mode == RESONATE_RENDER_ADDRESS_CLAMP
                                          ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE
                                          : VK_SAMPLER_ADDRESS_MODE_REPEAT;

    SamplerSlot* slot = nullptr;
    const uint32_t index = samplers_.acquire(&slot);
    *slot = SamplerSlot{};

    VkSamplerCreateInfo sampler{};
    sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.minFilter = filter;
    sampler.magFilter = filter;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = mode;
    sampler.addressModeV = mode;
    sampler.addressModeW = mode;
    sampler.maxLod = 1.0f;
    if (vkCreateSampler(device_, &sampler, nullptr, &slot->sampler) != VK_SUCCESS)
    {
        samplers_.retire(index);
        note(device_failed_, "sampler creation failed");
        return RESONATE_E_INTERNAL;
    }
    slot->alive = true;
    *out = ResonateRenderSampler{handleValue(slot->generation, index)};
    return RESONATE_OK;
}

void Context::destroySampler(ResonateRenderSampler handle)
{
    if (handle.value == 0 || device_ == VK_NULL_HANDLE)
    {
        return;
    }
    SamplerSlot* slot = samplers_.live(handleIndex(handle.value), handleGeneration(handle.value));
    if (slot == nullptr)
    {
        return;
    }
    frames_[frame_ % kFramesInFlight].graveyard.push_back(
        Retired{kRetiredSampler, static_cast<void*>(slot->sampler), nullptr});
    samplers_.retire(handleIndex(handle.value));
}

ResonateStatus Context::createShader(ResonateRenderShaderStage stage, const void* spirv,
                                     uint64_t size_bytes, ResonateRenderShader* out)
{
    if (device_ == VK_NULL_HANDLE)
    {
        return RESONATE_E_STATE;
    }
    if (spirv == nullptr || size_bytes < 16U || (size_bytes % 4U) != 0U)
    {
        return RESONATE_E_INVALID;
    }

    static const uint32_t kSpirvMagic = 0x07230203u;
    uint32_t magic = 0;
    std::memcpy(&magic, spirv, sizeof(magic));
    if (magic != kSpirvMagic)
    {
        return RESONATE_E_INVALID;
    }

    ShaderSlot* slot = nullptr;
    const uint32_t index = shaders_.acquire(&slot);
    *slot = ShaderSlot{};

    VkShaderModuleCreateInfo module{};
    module.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    module.codeSize = size_bytes;
    module.pCode = static_cast<const uint32_t*>(spirv);
    if (vkCreateShaderModule(device_, &module, nullptr, &slot->module) != VK_SUCCESS)
    {
        shaders_.retire(index);
        note(device_failed_, "shader module creation failed");
        return RESONATE_E_INTERNAL;
    }
    slot->alive = true;
    *out = ResonateRenderShader{handleValue(slot->generation, index)};
    return RESONATE_OK;
}

void Context::destroyShader(ResonateRenderShader handle)
{
    if (handle.value == 0 || device_ == VK_NULL_HANDLE)
    {
        return;
    }
    ShaderSlot* slot = shaders_.live(handleIndex(handle.value), handleGeneration(handle.value));
    if (slot == nullptr)
    {
        return;
    }
    frames_[frame_ % kFramesInFlight].graveyard.push_back(
        Retired{kRetiredShader, static_cast<void*>(slot->module), nullptr});
    shaders_.retire(handleIndex(handle.value));
}

ResonateStatus Context::createPipeline(const ResonateRenderGraphicsPipelineDesc& desc,
                                       ResonateRenderPipeline* out)
{
    if (device_ == VK_NULL_HANDLE)
    {
        return RESONATE_E_STATE;
    }
    ShaderSlot* vertex = shaders_.live(handleIndex(desc.vertex_shader.value),
                                       handleGeneration(desc.vertex_shader.value));
    ShaderSlot* fragment = shaders_.live(handleIndex(desc.fragment_shader.value),
                                         handleGeneration(desc.fragment_shader.value));
    if (vertex == nullptr || fragment == nullptr || desc.stream_count > 1)
    {
        return RESONATE_E_INVALID;
    }
    if (desc.color_format == RESONATE_RENDER_FORMAT_UNKNOWN && swapchain_ == VK_NULL_HANDLE &&
        !offscreen_)
    {
        /* UNKNOWN names the canvas's image format; there is no target to
           name. */
        return RESONATE_E_STATE;
    }

    if (pipeline_layout_ == VK_NULL_HANDLE)
    {
        /* Set 0 is the resource table, shared by every pipeline; constants ride
           the 128-byte push-constant block. */
        VkPushConstantRange range{};
        range.stageFlags =
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
        range.offset = 0;
        range.size = kMaxConstants;

        VkPipelineLayoutCreateInfo layout{};
        layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layout.setLayoutCount = 1;
        layout.pSetLayouts = &table_layout_;
        layout.pushConstantRangeCount = 1;
        layout.pPushConstantRanges = &range;
        const VkResult created = vkCreatePipelineLayout(device_, &layout, nullptr, &pipeline_layout_);
        if (created != VK_SUCCESS)
        {
            std::fprintf(stderr, "render-vulkan: pipeline layout creation failed (%d)\n",
                         static_cast<int>(created));
            return RESONATE_E_INTERNAL;
        }
    }

    /* UNKNOWN in the pass key resolves to the swapchain format. */
    VkRenderPass pass = VK_NULL_HANDLE;
    if (renderPassFor(desc.color_format, desc.depth_format, &pass) != RESONATE_OK)
    {
        return RESONATE_E_INTERNAL;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex->module;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment->module;
    stages[1].pName = "main";

    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = desc.stream_count != 0U ? desc.streams[0].stride_bytes : 0U;
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    /* Slang emits attribute locations directly; the vertex formats map onto
       the color formats' numeric shapes. */
    std::vector<VkVertexInputAttributeDescription> attributes;
    if (desc.stream_count != 0U)
    {
        attributes.reserve(desc.streams[0].attribute_count);
        for (uint32_t i = 0; i < desc.streams[0].attribute_count; ++i)
        {
            const ResonateRenderVertexAttribute& attribute = desc.streams[0].attributes[i];
            VkVertexInputAttributeDescription converted{};
            converted.location = attribute.location;
            converted.binding = 0;
            converted.format = formatOf(attribute.format);
            converted.offset = attribute.offset_bytes;
            attributes.push_back(converted);
        }
    }

    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input.vertexBindingDescriptionCount = desc.stream_count != 0U ? 1U : 0U;
    vertex_input.pVertexBindingDescriptions = desc.stream_count != 0U ? &binding : nullptr;
    vertex_input.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributes.size());
    vertex_input.pVertexAttributeDescriptions = attributes.data();

    VkPipelineInputAssemblyStateCreateInfo assembly{};
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = desc.cull_back != 0U ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;

    VkPipelineDepthStencilStateCreateInfo depth_state{};
    depth_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_state.depthTestEnable =
        desc.depth_format != RESONATE_RENDER_FORMAT_UNKNOWN && desc.depth_test != 0U ? VK_TRUE
                                                                                     : VK_FALSE;
    depth_state.depthWriteEnable =
        desc.depth_format != RESONATE_RENDER_FORMAT_UNKNOWN && desc.depth_write != 0U ? VK_TRUE
                                                                                      : VK_FALSE;
    depth_state.depthCompareOp = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    /* A depth-only pass has no color attachment, so the blend state names
       none either. */
    const bool colorless = desc.color_format == RESONATE_RENDER_FORMAT_NONE;
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = colorless ? 0U : 1U;
    blend.pAttachments = colorless ? nullptr : &blend_attachment;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkDynamicState dynamics[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamics;

    VkGraphicsPipelineCreateInfo pipeline{};
    pipeline.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline.stageCount = 2;
    pipeline.pStages = stages;
    pipeline.pVertexInputState = &vertex_input;
    pipeline.pInputAssemblyState = &assembly;
    pipeline.pViewportState = &viewport;
    pipeline.pRasterizationState = &raster;
    pipeline.pDepthStencilState = &depth_state;
    pipeline.pColorBlendState = &blend;
    pipeline.pMultisampleState = &multisample;
    pipeline.pDynamicState = &dynamic;
    pipeline.layout = pipeline_layout_;
    pipeline.renderPass = pass;

    PipelineSlot* slot = nullptr;
    const uint32_t index = pipelines_.acquire(&slot);
    *slot = PipelineSlot{};
    const VkResult created =
        vkCreateGraphicsPipelines(device_, pipeline_cache_, 1, &pipeline, nullptr, &slot->pipeline);
    if (created != VK_SUCCESS)
    {
        pipelines_.retire(index);
        std::fprintf(stderr, "render-vulkan: graphics pipeline creation failed (%d)\n",
                     static_cast<int>(created));
        return RESONATE_E_INTERNAL;
    }
    slot->alive = true;
    slot->compute = false;
    *out = ResonateRenderPipeline{handleValue(slot->generation, index)};
    return RESONATE_OK;
}

void Context::destroyPipeline(ResonateRenderPipeline handle)
{
    if (handle.value == 0 || device_ == VK_NULL_HANDLE)
    {
        return;
    }
    PipelineSlot* slot = pipelines_.live(handleIndex(handle.value), handleGeneration(handle.value));
    if (slot == nullptr)
    {
        return;
    }
    frames_[frame_ % kFramesInFlight].graveyard.push_back(
        Retired{kRetiredPipeline, static_cast<void*>(slot->pipeline), nullptr});
    pipelines_.retire(handleIndex(handle.value));
}

ResonateStatus Context::uploadBuffer(ResonateRenderBuffer handle, uint64_t offset, const void* data,
                                     uint64_t size_bytes)
{
    if (device_ == VK_NULL_HANDLE)
    {
        return RESONATE_E_STATE;
    }
    if (data == nullptr || size_bytes == 0U)
    {
        return RESONATE_E_INVALID;
    }
    BufferSlot* slot = buffers_.live(handleIndex(handle.value), handleGeneration(handle.value));
    if (slot == nullptr || offset > slot->size || size_bytes > slot->size - offset)
    {
        std::fprintf(stderr, "render-vulkan: buffer upload refused\n");
        return RESONATE_E_INVALID;
    }

    if (slot->mapped != nullptr)
    {
        std::memcpy(static_cast<unsigned char*>(slot->mapped) + offset, data, size_bytes);
        /* Mapped host memory is not necessarily coherent. */
        vmaFlushAllocation(allocator_, slot->allocation, offset, size_bytes);
        return RESONATE_OK;
    }

    /* Device-local: one staging buffer, one synchronous copy. */
    VkBufferCreateInfo staging_info{};
    staging_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    staging_info.size = size_bytes;
    staging_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    staging_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo staging_allocation{};
    staging_allocation.usage = VMA_MEMORY_USAGE_AUTO;
    staging_allocation.flags =
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;

    VkBuffer staging_buffer = VK_NULL_HANDLE;
    VmaAllocation staging = VK_NULL_HANDLE;
    VmaAllocationInfo staging_info_out{};
    if (vmaCreateBuffer(allocator_, &staging_info, &staging_allocation, &staging_buffer, &staging,
                        &staging_info_out) != VK_SUCCESS)
    {
        return RESONATE_E_INTERNAL;
    }
    std::memcpy(staging_info_out.pMappedData, data, size_bytes);

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkResetCommandBuffer(aux_command_, 0);
    vkBeginCommandBuffer(aux_command_, &begin);

    VkBufferCopy copy{};
    copy.srcOffset = 0;
    copy.dstOffset = offset;
    copy.size = size_bytes;
    vkCmdCopyBuffer(aux_command_, staging_buffer, slot->buffer, 1, &copy);
    vkEndCommandBuffer(aux_command_);
    const ResonateStatus status = submitAndRun(aux_command_);

    vmaDestroyBuffer(allocator_, staging_buffer, staging);
    return status;
}

ResonateStatus Context::uploadImage(ResonateRenderImage handle, const void* data,
                                    uint64_t size_bytes, uint64_t row_pitch_bytes)
{
    if (device_ == VK_NULL_HANDLE)
    {
        return RESONATE_E_STATE;
    }
    ImageSlot* slot = images_.live(handleIndex(handle.value), handleGeneration(handle.value));
    if (slot == nullptr || data == nullptr || size_bytes == 0U)
    {
        return RESONATE_E_INVALID;
    }

    const uint64_t tight_pitch = static_cast<uint64_t>(slot->width) * slot->bytes_per_pixel;
    if (row_pitch_bytes == 0U)
    {
        row_pitch_bytes = tight_pitch;
    }
    if (row_pitch_bytes != tight_pitch ||
        size_bytes < tight_pitch * static_cast<uint64_t>(slot->height))
    {
        return RESONATE_E_INVALID;
    }

    VkBufferCreateInfo staging_info{};
    staging_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    staging_info.size = size_bytes;
    staging_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    staging_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo staging_allocation{};
    staging_allocation.usage = VMA_MEMORY_USAGE_AUTO;
    staging_allocation.flags =
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;

    VkBuffer staging_buffer = VK_NULL_HANDLE;
    VmaAllocation staging = VK_NULL_HANDLE;
    VmaAllocationInfo staging_info_out{};
    if (vmaCreateBuffer(allocator_, &staging_info, &staging_allocation, &staging_buffer, &staging,
                        &staging_info_out) != VK_SUCCESS)
    {
        return RESONATE_E_INTERNAL;
    }
    std::memcpy(staging_info_out.pMappedData, data, size_bytes);

    vkResetCommandBuffer(aux_command_, 0);
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(aux_command_, &begin);

    VkImageMemoryBarrier to_transfer{};
    to_transfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_transfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_transfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_transfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_transfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_transfer.image = slot->image;
    to_transfer.subresourceRange.aspectMask =
        slot->depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    to_transfer.subresourceRange.levelCount = 1;
    to_transfer.subresourceRange.layerCount = 1;
    to_transfer.srcAccessMask = 0;
    to_transfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(aux_command_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &to_transfer);

    VkBufferImageCopy copy{};
    copy.imageSubresource.aspectMask = to_transfer.subresourceRange.aspectMask;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent = VkExtent3D{slot->width, slot->height, 1U};
    vkCmdCopyBufferToImage(aux_command_, staging_buffer, slot->image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

    /* Ready for whatever the usage declared: sampled images wait in shader
       read-only, attachments in their attachment layout. */
    const VkImageLayout ready = (slot->depth) ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                                              : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkImageMemoryBarrier to_ready = to_transfer;
    to_ready.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_ready.newLayout = ready;
    to_ready.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_ready.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(aux_command_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &to_ready);

    vkEndCommandBuffer(aux_command_);
    const ResonateStatus status = submitAndRun(aux_command_);

    vmaDestroyBuffer(allocator_, staging_buffer, staging);
    return status;
}

ResonateStatus Context::bindPipeline(ResonateRenderPipeline handle)
{
    PipelineSlot* slot = pipelines_.live(handleIndex(handle.value), handleGeneration(handle.value));
    if (slot == nullptr)
    {
        return RESONATE_E_INVALID;
    }
    /* A graphics pipeline binds inside a render pass; a compute pipeline binds
       for the frame and dispatches outside one. */
    if (slot->compute ? !frame_open_ : !pass_open_)
    {
        return RESONATE_E_STATE;
    }
    VkCommandBuffer command = frames_[frame_ % kFramesInFlight].command;
    vkCmdBindPipeline(
        command, slot->compute ? VK_PIPELINE_BIND_POINT_COMPUTE : VK_PIPELINE_BIND_POINT_GRAPHICS,
        slot->pipeline);
    bound_pipeline_ = slot->pipeline;
    bound_compute_ = slot->compute;
    return RESONATE_OK;
}

ResonateStatus Context::bindVertexBuffer(ResonateRenderBuffer handle, uint64_t offset)
{
    if (!pass_open_)
    {
        return RESONATE_E_STATE;
    }
    BufferSlot* slot = buffers_.live(handleIndex(handle.value), handleGeneration(handle.value));
    if (slot == nullptr || offset >= slot->size)
    {
        return RESONATE_E_INVALID;
    }
    VkCommandBuffer command = frames_[frame_ % kFramesInFlight].command;
    VkDeviceSize device_offset = offset;
    vkCmdBindVertexBuffers(command, 0, 1, &slot->buffer, &device_offset);
    bound_vertex_ = slot->buffer;
    return RESONATE_OK;
}

ResonateStatus Context::bindIndexBuffer(ResonateRenderBuffer handle, uint64_t offset)
{
    if (!pass_open_)
    {
        return RESONATE_E_STATE;
    }
    BufferSlot* slot = buffers_.live(handleIndex(handle.value), handleGeneration(handle.value));
    if (slot == nullptr || offset >= slot->size)
    {
        return RESONATE_E_INVALID;
    }
    VkCommandBuffer command = frames_[frame_ % kFramesInFlight].command;
    vkCmdBindIndexBuffer(command, slot->buffer, offset, VK_INDEX_TYPE_UINT32);
    bound_index_ = slot->buffer;
    return RESONATE_OK;
}

ResonateStatus Context::setConstants(const void* data, uint32_t size_bytes)
{
    if (!frame_open_)
    {
        return RESONATE_E_STATE;
    }
    if (bound_pipeline_ == VK_NULL_HANDLE || data == nullptr || size_bytes == 0U ||
        size_bytes > kMaxConstants)
    {
        return RESONATE_E_INVALID;
    }
    VkCommandBuffer command = frames_[frame_ % kFramesInFlight].command;
    /* The layout's range covers all three stages, and a graphics pipeline's
       draw may still be preceded by a compute pass's constants. */
    vkCmdPushConstants(command, pipeline_layout_,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
                           VK_SHADER_STAGE_COMPUTE_BIT,
                       0, size_bytes, data);
    return RESONATE_OK;
}

ResonateStatus Context::updateBuffer(ResonateRenderBuffer handle, uint64_t offset, const void* data,
                                     uint64_t size_bytes)
{
    /* Recorded, not applied: the copy lands with the frame, which is what makes
       per-frame data safe without waiting on the frames in flight. */
    if (!frame_open_ || pass_open_)
    {
        return RESONATE_E_STATE;
    }
    if (data == nullptr || size_bytes == 0U || size_bytes > 65536U)
    {
        return RESONATE_E_INVALID;
    }
    BufferSlot* slot = buffers_.live(handleIndex(handle.value), handleGeneration(handle.value));
    if (slot == nullptr || offset > slot->size || size_bytes > slot->size - offset)
    {
        return RESONATE_E_INVALID;
    }
    VkCommandBuffer command = frames_[frame_ % kFramesInFlight].command;
    vkCmdUpdateBuffer(command, slot->buffer, offset, size_bytes, data);
    return RESONATE_OK;
}

ResonateStatus Context::draw(uint32_t vertex_count, uint32_t instance_count)
{
    if (!pass_open_ || bound_pipeline_ == VK_NULL_HANDLE)
    {
        return RESONATE_E_STATE;
    }
    VkCommandBuffer command = frames_[frame_ % kFramesInFlight].command;
    vkCmdDraw(command, vertex_count, instance_count, 0, 0);
    return RESONATE_OK;
}

ResonateStatus Context::drawIndexed(uint32_t index_count, uint32_t instance_count)
{
    if (!pass_open_ || bound_pipeline_ == VK_NULL_HANDLE || bound_index_ == VK_NULL_HANDLE)
    {
        return RESONATE_E_STATE;
    }
    VkCommandBuffer command = frames_[frame_ % kFramesInFlight].command;
    vkCmdDrawIndexed(command, index_count, instance_count, 0, 0, 0);
    return RESONATE_OK;
}

ResonateStatus Context::createComputePipeline(ResonateRenderShader shader,
                                              ResonateRenderPipeline* out)
{
    if (device_ == VK_NULL_HANDLE)
    {
        return RESONATE_E_STATE;
    }
    if (pipeline_layout_ == VK_NULL_HANDLE)
    {
        return RESONATE_E_INTERNAL;
    }
    ShaderSlot* shader_slot =
        shaders_.live(handleIndex(shader.value), handleGeneration(shader.value));
    if (shader_slot == nullptr)
    {
        return RESONATE_E_INVALID;
    }

    VkPipelineShaderStageCreateInfo stage{};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = shader_slot->module;
    stage.pName = "main";

    VkComputePipelineCreateInfo pipeline{};
    pipeline.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeline.stage = stage;
    pipeline.layout = pipeline_layout_;

    PipelineSlot* slot = nullptr;
    const uint32_t index = pipelines_.acquire(&slot);
    *slot = PipelineSlot{};
    if (vkCreateComputePipelines(device_, pipeline_cache_, 1, &pipeline, nullptr,
                                 &slot->pipeline) != VK_SUCCESS)
    {
        pipelines_.retire(index);
        note(device_failed_, "compute pipeline creation failed");
        return RESONATE_E_INTERNAL;
    }
    slot->alive = true;
    slot->compute = true;
    *out = ResonateRenderPipeline{handleValue(slot->generation, index)};
    return RESONATE_OK;
}

ResonateStatus Context::dispatch(uint32_t groups_x, uint32_t groups_y, uint32_t groups_z)
{
    if (!frame_open_ || bound_pipeline_ == VK_NULL_HANDLE)
    {
        return RESONATE_E_STATE;
    }
    if (bound_compute_ == false)
    {
        return RESONATE_E_INVALID;
    }
    VkCommandBuffer command = frames_[frame_ % kFramesInFlight].command;
    vkCmdDispatch(command, groups_x, groups_y, groups_z);
    return RESONATE_OK;
}

} // namespace resonate::render_vulkan
