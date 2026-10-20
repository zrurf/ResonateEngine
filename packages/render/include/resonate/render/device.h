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
 * calls carry no binding calls. The table is set 0: bindings 0-4 are 64-slot
 * storage-buffer arrays and binding 5 is a 64-slot combined-image-sampler
 * array. A buffer created with the STORAGE usage, or an image with the SAMPLED
 * usage, is written into its slot (a buffer into every storage binding) at
 * creation, so any of those shader declarations can name it by slot:
 * `[[vk::binding(0, 0)]] StructuredBuffer<T> table[64];` on a Vulkan backend.
 * Because the write is the creation, resources must exist before the frame
 * that reads them.
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
    /* A pipeline marker, not a real format: a pipeline whose color format is
       NONE has no color outputs (depth-only). */
    RESONATE_RENDER_FORMAT_NONE = 0xFF,
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

/* Image layouts a barrier moves a resource between; the graph derives them
   from the passes' declarations. */
typedef enum ResonateRenderImageLayout : uint8_t
{
    RESONATE_RENDER_LAYOUT_UNDEFINED = 0,
    RESONATE_RENDER_LAYOUT_COLOR_ATTACHMENT,
    RESONATE_RENDER_LAYOUT_DEPTH_ATTACHMENT,
    RESONATE_RENDER_LAYOUT_SHADER_READ_ONLY,
    RESONATE_RENDER_LAYOUT_TRANSFER_SRC,
    RESONATE_RENDER_LAYOUT_TRANSFER_DST,
    RESONATE_RENDER_LAYOUT_PRESENT,
} ResonateRenderImageLayout;

/* Pipeline stages and memory access, the barrier's vocabulary. A stage mask
   of zero reads as "no prior work" (source) or "no later work" (destination). */
#define RESONATE_RENDER_STAGE_VERTEX (1u << 0)
#define RESONATE_RENDER_STAGE_FRAGMENT (1u << 1)
#define RESONATE_RENDER_STAGE_COLOR_OUTPUT (1u << 2)
#define RESONATE_RENDER_STAGE_DEPTH_OUTPUT (1u << 3)
#define RESONATE_RENDER_STAGE_COMPUTE (1u << 4)
#define RESONATE_RENDER_STAGE_TRANSFER (1u << 5)

#define RESONATE_RENDER_ACCESS_SHADER_READ (1u << 0)
#define RESONATE_RENDER_ACCESS_SHADER_WRITE (1u << 1)
#define RESONATE_RENDER_ACCESS_COLOR_WRITE (1u << 2)
#define RESONATE_RENDER_ACCESS_DEPTH_READ (1u << 3)
#define RESONATE_RENDER_ACCESS_DEPTH_WRITE (1u << 4)
#define RESONATE_RENDER_ACCESS_TRANSFER_READ (1u << 5)
#define RESONATE_RENDER_ACCESS_TRANSFER_WRITE (1u << 6)

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
   image. depth {0} means none. The pass always clears. `colorless` (a tail
   field, read only when struct_size covers it) drops the color attachment for
   a depth-only pass; `color` is then ignored. */
typedef struct ResonateRenderPassDesc
{
    uint32_t struct_size;
    ResonateRenderImage color;
    ResonateVec4 clear_color;
    ResonateRenderImage depth;
    uint32_t colorless;
} ResonateRenderPassDesc;

/* A memory dependency: what a resource's state was, what it becomes. The
   graph derives these from the passes' declarations; the device applies them
   inside the open frame. */
typedef struct ResonateRenderBarrierDesc
{
    uint32_t struct_size;
    ResonateRenderBuffer buffer; /* one of the two, the other null */
    ResonateRenderImage image;
    uint32_t before_stages;
    uint32_t after_stages;
    uint32_t before_access;
    uint32_t after_access;
    ResonateRenderImageLayout before_layout; /* images only */
    ResonateRenderImageLayout after_layout;
} ResonateRenderBarrierDesc;

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
    /* A compute pipeline runs through dispatch() inside the open frame; its
       shader is one compute entry point. */
    ResonateStatus (*create_compute_pipeline)(void* self, ResonateRenderShader shader,
                                              ResonateRenderPipeline* out_pipeline);

    /* Uploads, synchronous. An image's row_pitch must be the tightly packed
       one until a consumer needs otherwise. */
    ResonateStatus (*upload_buffer)(void* self, ResonateRenderBuffer buffer, uint64_t offset_bytes,
                                    const void* data, uint64_t size_bytes);
    ResonateStatus (*upload_image)(void* self, ResonateRenderImage image, const void* data,
                                   uint64_t size_bytes, uint64_t row_pitch_bytes);

    /* --- the frame --- */

    /* Acquires the canvas's image and opens the frame: one submission takes
       every pass and barrier between here and present. RESONATE_E_STATE
       without a surface. */
    ResonateStatus (*begin_frame)(void* self);

    /* One render pass inside the open frame. color {0} means the canvas's
       acquired image; an explicit image renders into that image. depth {0}
       means none. The pass always clears. */
    ResonateStatus (*begin_pass)(void* self, const ResonateRenderPassDesc* desc);
    ResonateStatus (*end_pass)(void* self);

    /* A memory dependency inside the open frame. */
    ResonateStatus (*barrier)(void* self, const ResonateRenderBarrierDesc* desc);

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

    /* A compute dispatch inside the open frame, against the bound compute
       pipeline. A compute pipeline binds without an open pass. */
    ResonateStatus (*dispatch)(void* self, uint32_t groups_x, uint32_t groups_y, uint32_t groups_z);

    /* Copies `size_bytes` into the buffer through the open frame's command
       stream: the write lands with the frame, so per-frame data needs no host
       mapping and no waiting on the frames in flight. `size_bytes` is capped at
       64 KiB. */
    ResonateStatus (*update_buffer)(void* self, ResonateRenderBuffer buffer, uint64_t offset_bytes,
                                    const void* data, uint64_t size_bytes);

    /* Closes and submits the frame, then presents the canvas's image.
       RESONATE_E_STATE when no frame is open. */
    ResonateStatus (*present)(void* self);

    /* Blocks until every submitted frame has completed. Resource surgery that
       rewrites table slots or destroys resources happens between frames, under
       this wait. RESONATE_E_STATE without a device or during an open frame. */
    ResonateStatus (*wait_idle)(void* self);

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
