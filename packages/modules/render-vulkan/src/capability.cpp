#include "capability.h"

namespace resonate::render_vulkan
{
namespace
{

ResonateStatus attachSurface(void* self, const void* native_surface, uint32_t width,
                             uint32_t height)
{
    auto* state = static_cast<State*>(self);
    if (state == nullptr || state->context == nullptr || native_surface == nullptr)
    {
        return RESONATE_E_INVALID;
    }
    return state->context->attachSurface(native_surface, width, height);
}

ResonateStatus createBuffer(void* self, const ResonateRenderBufferDesc* desc,
                            ResonateRenderBuffer* out)
{
    auto* state = static_cast<State*>(self);
    if (state == nullptr || state->context == nullptr || desc == nullptr || out == nullptr)
    {
        return RESONATE_E_INVALID;
    }
    return state->context->createBuffer(*desc, out);
}

void destroyBuffer(void* self, ResonateRenderBuffer buffer)
{
    auto* state = static_cast<State*>(self);
    if (state != nullptr && state->context != nullptr)
    {
        state->context->destroyBuffer(buffer);
    }
}

ResonateStatus createImage(void* self, const ResonateRenderImageDesc* desc,
                           ResonateRenderImage* out)
{
    auto* state = static_cast<State*>(self);
    if (state == nullptr || state->context == nullptr || desc == nullptr || out == nullptr)
    {
        return RESONATE_E_INVALID;
    }
    return state->context->createImage(*desc, out);
}

void destroyImage(void* self, ResonateRenderImage image)
{
    auto* state = static_cast<State*>(self);
    if (state != nullptr && state->context != nullptr)
    {
        state->context->destroyImage(image);
    }
}

ResonateStatus createSampler(void* self, const ResonateRenderSamplerDesc* desc,
                             ResonateRenderSampler* out)
{
    auto* state = static_cast<State*>(self);
    if (state == nullptr || state->context == nullptr || desc == nullptr || out == nullptr)
    {
        return RESONATE_E_INVALID;
    }
    return state->context->createSampler(*desc, out);
}

void destroySampler(void* self, ResonateRenderSampler sampler)
{
    auto* state = static_cast<State*>(self);
    if (state != nullptr && state->context != nullptr)
    {
        state->context->destroySampler(sampler);
    }
}

ResonateStatus createShader(void* self, ResonateRenderShaderStage stage, const void* spirv,
                            uint64_t size_bytes, ResonateRenderShader* out)
{
    auto* state = static_cast<State*>(self);
    if (state == nullptr || state->context == nullptr || out == nullptr)
    {
        return RESONATE_E_INVALID;
    }
    return state->context->createShader(stage, spirv, size_bytes, out);
}

void destroyShader(void* self, ResonateRenderShader shader)
{
    auto* state = static_cast<State*>(self);
    if (state != nullptr && state->context != nullptr)
    {
        state->context->destroyShader(shader);
    }
}

ResonateStatus createPipeline(void* self, const ResonateRenderGraphicsPipelineDesc* desc,
                              ResonateRenderPipeline* out)
{
    auto* state = static_cast<State*>(self);
    if (state == nullptr || state->context == nullptr || desc == nullptr || out == nullptr)
    {
        return RESONATE_E_INVALID;
    }
    return state->context->createPipeline(*desc, out);
}

void destroyPipeline(void* self, ResonateRenderPipeline pipeline)
{
    auto* state = static_cast<State*>(self);
    if (state != nullptr && state->context != nullptr)
    {
        state->context->destroyPipeline(pipeline);
    }
}

ResonateStatus createComputePipeline(void* self, ResonateRenderShader shader,
                                     ResonateRenderPipeline* out)
{
    auto* state = static_cast<State*>(self);
    if (state == nullptr || state->context == nullptr || out == nullptr)
    {
        return RESONATE_E_INVALID;
    }
    return state->context->createComputePipeline(shader, out);
}

ResonateStatus dispatch(void* self, uint32_t groups_x, uint32_t groups_y, uint32_t groups_z)
{
    auto* state = static_cast<State*>(self);
    return state != nullptr && state->context != nullptr
               ? state->context->dispatch(groups_x, groups_y, groups_z)
               : RESONATE_E_STATE;
}

ResonateStatus uploadBuffer(void* self, ResonateRenderBuffer buffer, uint64_t offset_bytes,
                            const void* data, uint64_t size_bytes)
{
    auto* state = static_cast<State*>(self);
    if (state == nullptr || state->context == nullptr)
    {
        return RESONATE_E_STATE;
    }
    return state->context->uploadBuffer(buffer, offset_bytes, data, size_bytes);
}

ResonateStatus uploadImage(void* self, ResonateRenderImage image, const void* data,
                           uint64_t size_bytes, uint64_t row_pitch_bytes)
{
    auto* state = static_cast<State*>(self);
    if (state == nullptr || state->context == nullptr)
    {
        return RESONATE_E_STATE;
    }
    return state->context->uploadImage(image, data, size_bytes, row_pitch_bytes);
}

ResonateStatus beginPass(void* self, const ResonateRenderPassDesc* desc)
{
    auto* state = static_cast<State*>(self);
    if (state == nullptr || state->context == nullptr || desc == nullptr)
    {
        return RESONATE_E_INVALID;
    }
    return state->context->beginPass(*desc);
}

ResonateStatus endPass(void* self)
{
    auto* state = static_cast<State*>(self);
    return state != nullptr && state->context != nullptr ? state->context->endPass()
                                                         : RESONATE_E_STATE;
}

ResonateStatus beginFrame(void* self)
{
    auto* state = static_cast<State*>(self);
    return state != nullptr && state->context != nullptr ? state->context->beginFrame()
                                                         : RESONATE_E_STATE;
}

ResonateStatus barrier(void* self, const ResonateRenderBarrierDesc* desc)
{
    auto* state = static_cast<State*>(self);
    return state != nullptr && state->context != nullptr ? state->context->barrier(desc)
                                                         : RESONATE_E_STATE;
}

ResonateStatus bindPipeline(void* self, ResonateRenderPipeline pipeline)
{
    auto* state = static_cast<State*>(self);
    return state != nullptr && state->context != nullptr ? state->context->bindPipeline(pipeline)
                                                         : RESONATE_E_STATE;
}

ResonateStatus bindVertexBuffer(void* self, ResonateRenderBuffer buffer, uint64_t offset_bytes)
{
    auto* state = static_cast<State*>(self);
    return state != nullptr && state->context != nullptr
               ? state->context->bindVertexBuffer(buffer, offset_bytes)
               : RESONATE_E_STATE;
}

ResonateStatus bindIndexBuffer(void* self, ResonateRenderBuffer buffer, uint64_t offset_bytes)
{
    auto* state = static_cast<State*>(self);
    return state != nullptr && state->context != nullptr
               ? state->context->bindIndexBuffer(buffer, offset_bytes)
               : RESONATE_E_STATE;
}

ResonateStatus setConstants(void* self, const void* data, uint32_t size_bytes)
{
    auto* state = static_cast<State*>(self);
    return state != nullptr && state->context != nullptr
               ? state->context->setConstants(data, size_bytes)
               : RESONATE_E_STATE;
}

ResonateStatus draw(void* self, uint32_t vertex_count, uint32_t instance_count)
{
    auto* state = static_cast<State*>(self);
    return state != nullptr && state->context != nullptr
               ? state->context->draw(vertex_count, instance_count)
               : RESONATE_E_STATE;
}

ResonateStatus drawIndexed(void* self, uint32_t index_count, uint32_t instance_count)
{
    auto* state = static_cast<State*>(self);
    return state != nullptr && state->context != nullptr
               ? state->context->drawIndexed(index_count, instance_count)
               : RESONATE_E_STATE;
}

ResonateStatus updateBuffer(void* self, ResonateRenderBuffer buffer, uint64_t offset_bytes,
                            const void* data, uint64_t size_bytes)
{
    auto* state = static_cast<State*>(self);
    return state != nullptr && state->context != nullptr
               ? state->context->updateBuffer(buffer, offset_bytes, data, size_bytes)
               : RESONATE_E_STATE;
}

ResonateStatus present(void* self)
{
    auto* state = static_cast<State*>(self);
    return state != nullptr && state->context != nullptr ? state->context->present()
                                                         : RESONATE_E_STATE;
}

ResonateStatus waitIdle(void* self)
{
    auto* state = static_cast<State*>(self);
    return state != nullptr && state->context != nullptr ? state->context->waitIdle()
                                                         : RESONATE_E_STATE;
}

ResonateStatus attachOffscreen(void* self, uint32_t width, uint32_t height)
{
    auto* state = static_cast<State*>(self);
    if (state == nullptr || state->context == nullptr)
    {
        return RESONATE_E_STATE;
    }
    return state->context->attachOffscreen(width, height);
}

ResonateStatus readbackCanvas(void* self, void* data, uint64_t size_bytes, uint32_t* out_width,
                              uint32_t* out_height)
{
    auto* state = static_cast<State*>(self);
    if (state == nullptr || state->context == nullptr)
    {
        return RESONATE_E_STATE;
    }
    return state->context->readbackCanvas(data, size_bytes, out_width, out_height);
}

uint32_t featureBits(void* self)
{
    auto* state = static_cast<State*>(self);
    return state != nullptr && state->context != nullptr ? state->context->featureBits() : 0U;
}

uint32_t bufferSlot(void* self, ResonateRenderBuffer buffer)
{
    auto* state = static_cast<State*>(self);
    return state != nullptr && state->context != nullptr ? state->context->bufferSlot(buffer) : 0U;
}

uint32_t imageSlot(void* self, ResonateRenderImage image)
{
    auto* state = static_cast<State*>(self);
    return state != nullptr && state->context != nullptr ? state->context->imageSlot(image) : 0U;
}

ResonateRenderDevice g_device = {
    RESONATE_CAPABILITY_HEADER_INIT(ResonateRenderDevice, RESONATE_THREAD_ANY),
    &attachSurface,
    &createBuffer,
    &destroyBuffer,
    &createImage,
    &destroyImage,
    &createSampler,
    &destroySampler,
    &createShader,
    &destroyShader,
    &createPipeline,
    &destroyPipeline,
    &createComputePipeline,
    &uploadBuffer,
    &uploadImage,
    &beginFrame,
    &beginPass,
    &endPass,
    &barrier,
    &bindPipeline,
    &bindVertexBuffer,
    &bindIndexBuffer,
    &setConstants,
    &draw,
    &drawIndexed,
    &dispatch,
    &updateBuffer,
    &present,
    &waitIdle,
    &attachOffscreen,
    &readbackCanvas,
    nullptr,
    &featureBits,
    &bufferSlot,
    &imageSlot,
};

} // namespace

ResonateRenderDevice& device()
{
    return g_device;
}

} // namespace resonate::render_vulkan
