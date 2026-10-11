#ifndef RESONATE_RENDER_DEVICE_H
#define RESONATE_RENDER_DEVICE_H

/*
 * The RHI device: what a render backend module publishes and what the renderer
 * above it writes against.
 *
 * One provider serves a run. Backends are separate modules, each publishing
 * this capability; a backend that is not the configuration's choice answers
 * RESONATE_E_UNSUPPORTED at on_attach and is skipped, and so does one whose id
 * is already taken, so coexisting backend modules self-select instead of
 * failing the run. Switching backends is a configuration change applied at
 * startup; a mid-run device swap is not an RHI operation.
 *
 * Handles are opaque 64-bit ids, {0} being null. The index half names the
 * resource's slot in the device's resource table: shaders reach resources
 * through indexed arrays and a draw names them by handle, which is why draw
 * calls carry no binding calls.
 *
 * Calls are serialised by the stage schedule (THREAD_ANY). Uploads are
 * synchronous.
 *
 * Every desc struct leads with struct_size and is read only as far as it
 * covers, so fields append without breaking an older writer.
 */

#include <resonate/module/capability.h>

#ifdef __cplusplus
#    include <resonate/module/capability_id.hpp>
#endif
#include <resonate/module/types.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct ResonateRenderBuffer
{
    uint64_t value;
} ResonateRenderBuffer;

typedef struct ResonateRenderImage
{
    uint64_t value;
} ResonateRenderImage;

typedef struct ResonateRenderSampler
{
    uint64_t value;
} ResonateRenderSampler;

typedef struct ResonateRenderShader
{
    uint64_t value;
} ResonateRenderShader;

typedef struct ResonateRenderPipeline
{
    uint64_t value;
} ResonateRenderPipeline;

/* Color and depth formats. The list only ever appends. */
typedef enum ResonateRenderFormat : uint8_t
{
    RESONATE_RENDER_FORMAT_UNKNOWN = 0,
    RESONATE_RENDER_FORMAT_R8G8B8A8_UNORM,
    RESONATE_RENDER_FORMAT_R8G8B8A8_SRGB,
    RESONATE_RENDER_FORMAT_B8G8R8A8_SRGB, /* the usual swapchain format */
    RESONATE_RENDER_FORMAT_R16G16B16A16_SFLOAT,
    RESONATE_RENDER_FORMAT_R32G32_SFLOAT,
    RESONATE_RENDER_FORMAT_R32G32B32_SFLOAT,
    RESONATE_RENDER_FORMAT_D32_SFLOAT,
} ResonateRenderFormat;

typedef enum ResonateRenderShaderStage : uint8_t
{
    RESONATE_RENDER_SHADER_VERTEX = 0,
    RESONATE_RENDER_SHADER_FRAGMENT,
    RESONATE_RENDER_SHADER_COMPUTE,
} ResonateRenderShaderStage;

typedef enum ResonateRenderFilter : uint8_t
{
    RESONATE_RENDER_FILTER_NEAREST = 0,
    RESONATE_RENDER_FILTER_LINEAR,
} ResonateRenderFilter;

typedef enum ResonateRenderAddressMode : uint8_t
{
    RESONATE_RENDER_ADDRESS_REPEAT = 0,
    RESONATE_RENDER_ADDRESS_CLAMP,
} ResonateRenderAddressMode;

/* Buffer usage bits; vertex/index/uniform/storage also gate the resource
   table a resource lands in. */
#define RESONATE_RENDER_BUFFER_VERTEX (1u << 0)
#define RESONATE_RENDER_BUFFER_INDEX (1u << 1)
#define RESONATE_RENDER_BUFFER_UNIFORM (1u << 2)
#define RESONATE_RENDER_BUFFER_STORAGE (1u << 3)
#define RESONATE_RENDER_BUFFER_TRANSFER_SRC (1u << 4)
#define RESONATE_RENDER_BUFFER_TRANSFER_DST (1u << 5)

/* Image usage bits. */
#define RESONATE_RENDER_IMAGE_COLOR_ATTACHMENT (1u << 0)
#define RESONATE_RENDER_IMAGE_DEPTH_ATTACHMENT (1u << 1)
#define RESONATE_RENDER_IMAGE_SAMPLED (1u << 2)
#define RESONATE_RENDER_IMAGE_TRANSFER_DST (1u << 3)

typedef struct ResonateRenderBufferDesc
{
    uint32_t struct_size;
    uint64_t size_bytes;
    uint32_t usage; /* RESONATE_RENDER_BUFFER_* bits */
    /* 1: persistently mapped, written through upload_buffer's memcpy path;
       0: device-local, uploaded through the device's own staging. */
    uint32_t host_visible;
} ResonateRenderBufferDesc;

typedef struct ResonateRenderImageDesc
{
    uint32_t struct_size;
    ResonateRenderFormat format;
    uint32_t width;
    uint32_t height;
    uint32_t usage; /* RESONATE_RENDER_IMAGE_* bits */
} ResonateRenderImageDesc;

typedef struct ResonateRenderSamplerDesc
{
    uint32_t struct_size;
    ResonateRenderFilter min_filter;
    ResonateRenderFilter mag_filter;
    ResonateRenderAddressMode address_mode; /* both axes in this version */
} ResonateRenderSamplerDesc;

/* A vertex attribute's format is one of the float/normalized color formats. */
typedef struct ResonateRenderVertexAttribute
{
    uint32_t location;
    ResonateRenderFormat format;
    uint32_t offset_bytes;
} ResonateRenderVertexAttribute;

typedef struct ResonateRenderVertexStream
{
    uint32_t stride_bytes;
    uint32_t attribute_count;
    const ResonateRenderVertexAttribute* attributes;
} ResonateRenderVertexStream;

/* One color target in this version; UNKNOWN names the canvas's swapchain
   format, which is also what a pass without an explicit image renders to. */
typedef struct ResonateRenderGraphicsPipelineDesc
{
    uint32_t struct_size;
    ResonateRenderShader vertex_shader;
    ResonateRenderShader fragment_shader;
    uint32_t stream_count; /* 0: the vertex stage pulls from its vertex id */
    const ResonateRenderVertexStream* streams;
    ResonateRenderFormat color_format;
    ResonateRenderFormat depth_format; /* UNKNOWN: no depth attachment */
    uint32_t cull_back;
    uint32_t depth_test;
    uint32_t depth_write;
} ResonateRenderGraphicsPipelineDesc;

/* The pass's targets. color {0} means the canvas's swapchain (the windowed or
   offscreen canvas the device attached); an explicit image renders into that
   image. depth {0} means none. The pass always clears. */
typedef struct ResonateRenderPassDesc
{
    uint32_t struct_size;
    ResonateRenderImage color;
    ResonateVec4 clear_color;
    ResonateRenderImage depth;
} ResonateRenderPassDesc;

typedef struct ResonateRenderDevice
{
    ResonateCapabilityHeader header;

    /* Attaches the native surface the device presents to and creates the
       device-side state. RESONATE_E_INVALID for a null surface,
       RESONATE_E_UNSUPPORTED when the platform has no surface backend,
       RESONATE_E_INTERNAL when creation fails. Until it succeeds, every other
       call answers RESONATE_E_STATE. */
    ResonateStatus (*attach_surface)(void* self, const void* native_surface, uint32_t width,
                                     uint32_t height);

    /* Resources. Creation fills the handle or reports; destruction schedules
       the release behind the frames that may still use the resource, so
       destroying in use is safe. A null handle answers RESONATE_E_INVALID. */
    ResonateStatus (*create_buffer)(void* self, const ResonateRenderBufferDesc* desc,
                                    ResonateRenderBuffer* out_buffer);
    void (*destroy_buffer)(void* self, ResonateRenderBuffer buffer);
    ResonateStatus (*create_image)(void* self, const ResonateRenderImageDesc* desc,
                                   ResonateRenderImage* out_image);
    void (*destroy_image)(void* self, ResonateRenderImage image);
    ResonateStatus (*create_sampler)(void* self, const ResonateRenderSamplerDesc* desc,
                                     ResonateRenderSampler* out_sampler);
    void (*destroy_sampler)(void* self, ResonateRenderSampler sampler);
    /* SPIR-V, as the slang toolchain emits it (`xmake slang`). */
    ResonateStatus (*create_shader)(void* self, ResonateRenderShaderStage stage, const void* spirv,
                                    uint64_t size_bytes, ResonateRenderShader* out_shader);
    void (*destroy_shader)(void* self, ResonateRenderShader shader);
    ResonateStatus (*create_pipeline)(void* self, const ResonateRenderGraphicsPipelineDesc* desc,
                                      ResonateRenderPipeline* out_pipeline);
    void (*destroy_pipeline)(void* self, ResonateRenderPipeline pipeline);

    /* Uploads, synchronous. An image's row_pitch must be the tightly packed
       one until a consumer needs otherwise. */
    ResonateStatus (*upload_buffer)(void* self, ResonateRenderBuffer buffer, uint64_t offset_bytes,
                                    const void* data, uint64_t size_bytes);
    ResonateStatus (*upload_image)(void* self, ResonateRenderImage image, const void* data,
                                   uint64_t size_bytes, uint64_t row_pitch_bytes);

    /* --- the frame --- */

    ResonateStatus (*begin_pass)(void* self, const ResonateRenderPassDesc* desc);
    ResonateStatus (*end_pass)(void* self);

    /* Graphics state for the draws of the open pass. A pipeline bound inside
       one pass stays bound to it; a new pass resets the state. */
    ResonateStatus (*bind_pipeline)(void* self, ResonateRenderPipeline pipeline);
    ResonateStatus (*bind_vertex_buffer)(void* self, ResonateRenderBuffer buffer,
                                         uint64_t offset_bytes);
    ResonateStatus (*bind_index_buffer)(void* self, ResonateRenderBuffer buffer,
                                        uint64_t offset_bytes);

    /* Constants the shaders read as the pipeline's own block; 128 bytes is the
       guarantee every backend keeps. */
    ResonateStatus (*set_constants)(void* self, const void* data, uint32_t size_bytes);

    ResonateStatus (*draw)(void* self, uint32_t vertex_count, uint32_t instance_count);
    ResonateStatus (*draw_indexed)(void* self, uint32_t index_count, uint32_t instance_count);

    /* Presents the frame's pass on the canvas. RESONATE_E_STATE when no
       surface is attached or the frame rendered nothing. */
    ResonateStatus (*present)(void* self);

    /* Provider state; the host never interprets it. */
    void* self;
} ResonateRenderDevice;

#ifdef __cplusplus
} // extern "C"

namespace resonate::detail
{
template <> struct CapabilityTraits<ResonateRenderDevice>
{
    static constexpr const char* name = "Resonate.Render.Device";
    /* Inline so the whole program shares one definition; not constexpr, because
       the hash is a library call. */
    static inline const Id id{name};
    static constexpr std::uint32_t version = 1;
};
} // namespace resonate::detail
#endif

#endif /* RESONATE_RENDER_DEVICE_H */
