#include <resonate/renderer/renderer.h>

#include <resonate/renderer/texture.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <resonate/core/math.h>
#include <resonate/ecs/query.h>
#include <resonate/hierarchy/tree.h>
#include <resonate/module/capability.h>
#include <resonate/module/capability_id.hpp>
#include <resonate/pal/io.h>
#include <resonate/render/canvas.h>
#include <resonate/render/device.h>
#include <resonate/render/executor.h>
#include <resonate/render/graph.h>
#include <resonate/ui/display_list.h>

#include "frame_math.h"
#include "meshes.h"
#include "resonate.render/components.gen.h"
#include "resonate.ui/components.gen.h"

namespace resonate::renderer
{
namespace
{

using resonate::Mat4;
using resonate::Vec3;
using resonate::Vec4;
using resonate::ecs::ChunkView;
using resonate::ecs::Column;
using resonate::render::GraphExecuteContext;
using resonate::render::kLayoutColorAttachment;
using resonate::render::kLayoutDepthAttachment;
using resonate::render::kLayoutPresent;
using resonate::render::kLayoutShaderReadOnly;
using resonate::render::kLayoutUndefined;
using resonate::render::ResourceId;
using resonate::render::Use;

/* The shader-side blocks, mirrored byte for byte in the Slang sources. The
   frame data and the point lights live in device-local storage buffers the
   extract rewrites every frame. */
struct ShaderLight
{
    Vec4 position_radius;
    Vec4 color_intensity;
};

/* A compat-tier device may not read storage buffers at all, so a fixed set of
   point lights rides the frame's uniform block; the cluster list stays the
   bindless path's way. */
constexpr std::uint32_t kUniformLights = 32;

struct ShaderFrameData
{
    Mat4 view_proj;
    Mat4 camera_world;
    Vec4 camera_and_near;             /* xyz = camera position, w = near_z */
    Vec4 direction_and_far;           /* xyz = directional direction, w = far_z */
    Vec4 directional_color_intensity; /* rgb = color, w = intensity */
    Vec4 light_count_and_pad;         /* x = light count, yz = cluster tile pixels */
    Mat4 light_view_proj[3];
    Vec4 split_distances; /* far view-space depth per cascade */
    std::uint32_t uniform_light_count = 0;
    std::uint32_t uniform_light_pad[3] = {};
    ShaderLight uniform_lights[kUniformLights] = {};
};

/* std140 offsets the shader's FrameData sees. */
static_assert(offsetof(ShaderFrameData, uniform_lights) == 416);
static_assert(sizeof(ShaderFrameData) == 1440);

constexpr std::uint32_t kMaxLights = 256;
constexpr std::uint32_t kCascades = 3;

struct ShaderClusterLights
{
    std::uint32_t offset;
    std::uint32_t count;
};

struct ShaderClusterAABB
{
    Vec4 minimum;
    Vec4 maximum;
};

constexpr std::uint32_t kClustersX = 16;
constexpr std::uint32_t kClustersY = 9;
constexpr std::uint32_t kClustersZ = 24;
constexpr std::uint32_t kClusterCount = kClustersX * kClustersY * kClustersZ;
constexpr std::uint32_t kMaxLightsPerCluster = 128;

/* The bloom targets run at a quarter of the scene target's extent per axis:
   enough for the blur's wide kernel, cheap enough to be free. */
constexpr std::uint32_t kBloomDivisor = 4U;

/* The push-constant block the scene/cluster shaders read: 124 bytes. The
   cluster and shadow shaders read only their 112-byte prefix. */
struct FrameConstants
{
    Mat4 model;
    Vec4 tint;
    std::uint32_t slots[4];     /* frame data, cluster boxes, light grid, indices */
    std::uint32_t slots2[4];    /* light list, shadow map slots 0..2 */
    std::uint32_t texture_slot; /* the scene's albedo texture */
    float roughness;
    float metallic;
};

/* The block the post shaders read: 32 bytes. */
struct PostConstants
{
    std::uint32_t slots[4]; /* input image, second input (bloom), UI overlay */
    Vec4 params;
};

/* One UI overlay vertex: a target-pixel position, a straight-alpha color and
   the sampled UV. Six per quad, so the pipeline needs no index buffer. */
struct UiVertex
{
    float x = 0.0F;
    float y = 0.0F;
    float r = 0.0F;
    float g = 0.0F;
    float b = 0.0F;
    float a = 0.0F;
    float u = 0.0F;
    float v = 0.0F;
};

/* The overlay's push block: the target extent the pixels divide by and the
   sampled-image slot for the batch being drawn. A solid batch names no slot,
   so it never depends on the sampled table being populated. */
struct UiConstants
{
    float viewport_width = 1.0F;
    float viewport_height = 1.0F;
    float pad[2] = {};
    std::uint32_t texture_slot = 0U;
};

/* A contiguous vertex range sharing one sampled texture. */
constexpr std::uint32_t kSolidUiTextureSlot = 0xFFFFFFFFU;

struct UiBatch
{
    std::uint32_t first_vertex = 0U;
    std::uint32_t vertex_count = 0U;
    std::uint32_t texture = 0U;
};

/* A frame's overlay is bounded by one vertex buffer; a larger list is
   truncated rather than resized mid-frame. */
constexpr std::uint32_t kMaxUiQuads = 4096U;
static_assert(sizeof(UiVertex) == 32U);
static_assert(sizeof(PostConstants) == 32U);

/* The frame one extract produces; the draw callback walks it. */
struct DrawInstance
{
    Mat4 model;
    Vec3 color;
    MeshId mesh;
    std::uint32_t texture; /* 0 = white, 1 = checker (v1's built-ins) */
    float roughness = 0.5F;
    float metallic = 0.0F;
};

/* The first entity carrying Canvas is the overlay's root this frame. */
struct UiRoot
{
    ecs::Entity entity{};
    bool found = false;
};

/* One extracted frame, handed to the render thread. The producer fills a
   scratch snapshot and publishes it into the double buffer; the render thread
   executes the published one. */
struct FrameSnapshot
{
    ShaderFrameData frame_data{};
    std::vector<ShaderLight> lights;
    std::vector<DrawInstance> instances;
    ui::DisplayList ui;
    std::uint32_t target_width = 0U;
    std::uint32_t target_height = 0U;
};

constexpr ResonateVec4 kClearColor{0.05F, 0.06F, 0.09F, 1.0F};

/* The shader directory sits above the executable, wherever the build tree put
   it; a shipped game keeps it beside the executable. A platform that carries
   them elsewhere — an APK's assets extracted by the host — names the directory
   through the environment. */
std::string shadersDirectory()
{
    if (const char* const from_environment = std::getenv("RESONATE_SHADER_PATH");
        from_environment != nullptr && *from_environment != '\0')
    {
        return from_environment;
    }

    std::filesystem::path directory = resonate_pal_io_executable_directory();
    for (int level = 0; level < 5; ++level)
    {
        const std::filesystem::path candidate = directory / "shaders";
        if (std::filesystem::is_directory(candidate))
        {
            return candidate.string();
        }
        if (!directory.has_parent_path())
        {
            break;
        }
        directory = directory.parent_path();
    }
    return {};
}

std::vector<char> readFile(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::vector<char>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

/* The cooked textures sit beside the executable in a shipped layout and under
   the build directory during development; the walk covers both. */
std::string contentTexturesDirectory()
{
    std::filesystem::path directory = resonate_pal_io_executable_directory();
    for (int level = 0; level < 5; ++level)
    {
        const std::filesystem::path candidate = directory / "content" / "textures";
        if (std::filesystem::is_directory(candidate))
        {
            return candidate.string();
        }
        if (!directory.has_parent_path())
        {
            break;
        }
        directory = directory.parent_path();
    }
    return {};
}

/* A tightly packed RGBA8 texture in the sampled-image table. */
bool createSampledTexture(ResonateRenderDevice& device, std::uint32_t width, std::uint32_t height,
                          ResonateRenderFormat format, const std::vector<std::uint8_t>& pixels,
                          ResonateRenderImage& out_texture)
{
    ResonateRenderImageDesc desc{};
    desc.struct_size = sizeof(desc);
    desc.format = format;
    desc.width = width;
    desc.height = height;
    desc.usage = RESONATE_RENDER_IMAGE_SAMPLED;
    /* Albedo texels stay crisp: a checker or pixel art must not smear. */
    desc.nearest = 1U;
    if (device.create_image(device.self, &desc, &out_texture) != RESONATE_OK)
    {
        return false;
    }
    return device.upload_image(device.self, out_texture, pixels.data(), pixels.size(),
                               static_cast<std::uint64_t>(width) * 4U) == RESONATE_OK;
}

/* The three frustum-corner world positions of one cascade slice: the light's
   ortho box fits their bounds. */
void cascadeBounds(const Mat4& view_proj, const Mat4& camera_world, float near_depth,
                   float far_depth, Vec3& out_min, Vec3& out_max)
{
    const float width_scale = view_proj.m[0][0];
    const float height_scale = view_proj.m[1][1];
    out_min = Vec3{1e30F, 1e30F, 1e30F};
    out_max = Vec3{-1e30F, -1e30F, -1e30F};
    for (std::uint32_t corner = 0; corner < 16U; ++corner)
    {
        const float depth = (corner & 8U) != 0U ? far_depth : near_depth;
        const float x_ndc = (corner & 1U) != 0U ? 1.0F : -1.0F;
        const float y_ndc = (corner & 2U) != 0U ? 1.0F : -1.0F;
        /* Undo the projection (flipped y, [0, 1] depth), then to world. */
        const Vec3 view_point{x_ndc * depth / width_scale, y_ndc * depth / height_scale, depth};
        const Vec3 world = transformPoint(camera_world, view_point);
        out_min = Vec3{std::fmin(out_min.x, world.x), std::fmin(out_min.y, world.y),
                       std::fmin(out_min.z, world.z)};
        out_max = Vec3{std::fmax(out_max.x, world.x), std::fmax(out_max.y, world.y),
                       std::fmax(out_max.z, world.z)};
    }
}

} // namespace

struct RenderSystem::Impl
{
    ResonateCapabilityRegistry* registry = nullptr;

    /* Resolved once the render module's device appears in the registry. */
    ResonateRenderDevice* device = nullptr;
    ResonateRenderCanvas* canvas = nullptr;
    /* Target and device state. The render thread owns everything below the
       handoff block; `device` and `canvas` are resolved before it starts and
       only read afterwards. */
    bool surface_attached = false;
    bool surface_unavailable = false;
    bool windowed = false;
    /* The native target the surface was attached from; a platform that replaces
       its window needs the surface rebuilt. */
    const void* attached_native = nullptr;
    bool initialized = false;
    bool released = false;

    /* The view's post-chain selection, defaults until the run sets it. */
    FeatureStack features = FeatureStack::defaults();

    ecs::Query instances;
    ecs::Query cameras;
    ecs::Query point_lights;
    ecs::Query directional_lights;
    ecs::Query ui_canvases;
    bool ui_enabled = false;
    bool queries_ready = false;

    /* Producer state: the extract fills a scratch snapshot, then publishes it.
       The camera and light fallbacks persist across frames. */
    FrameSnapshot scratch;
    bool have_camera = false;
    Mat4 camera_world{};
    float fov_radians = 60.0F * 3.14159265F / 180.0F;
    float near_z = 0.1F;
    float far_z = 1000.0F;
    Vec3 directional_direction{0.35F, -1.0F, 0.25F};
    Vec3 directional_color{1.0F, 0.96F, 0.88F};
    float directional_intensity = 1.1F;

    /* Handoff: two snapshots. The producer fills the one the render thread is
       not reading and publishes it; when both are in the render thread's hands
       it waits, which bounds the render lag at one frame. */
    static constexpr std::uint32_t kSnapshotCount = 2U;
    static constexpr std::uint32_t kNoSnapshot = kSnapshotCount;
    std::array<FrameSnapshot, kSnapshotCount> snapshots;
    std::uint32_t pending_snapshot = 0U;
    std::uint32_t active_snapshot = kNoSnapshot;
    bool snapshot_pending = false;
    bool consumer_busy = false;
    bool stop = false;
    std::mutex handoff_mutex;
    std::condition_variable handoff_wake;
    std::condition_variable idle_wake;
    std::thread render_thread;
    bool render_thread_started = false;
    bool warm = false;

    /* The render thread's current snapshot; the pass callbacks read it. */
    FrameSnapshot* active = nullptr;

    resonate::render::GraphPlan plan;
    ResourceId canvas_resource = 0;
    ResourceId depth_resource = 0;
    ResourceId hdr_resource = 0;
    ResourceId ui_resource = 0;
    ResourceId ldr_resource = 0;
    ResourceId bright_resource = 0;
    ResourceId blur_a_resource = 0;
    ResourceId blur_b_resource = 0;
    ResourceId shadow_resource[3] = {0, 0, 0};
    ResourceId boxes_resource = 0;
    ResourceId grid_resource = 0;
    ResourceId indices_resource = 0;
    std::unique_ptr<resonate::render::GraphExecutor> executor;

    /* The scene and post targets follow the canvas extent; a resize rebuilds
       the plan and its executor. */
    std::uint32_t target_width = 0U;
    std::uint32_t target_height = 0U;
    std::uint32_t bloom_width = 1U;
    std::uint32_t bloom_height = 1U;

    ResonateRenderPipeline scene_pipeline{};
    ResonateRenderPipeline shadow_pipeline{};
    ResonateRenderPipeline build_pipeline{};
    ResonateRenderPipeline cull_pipeline{};
    ResonateRenderPipeline bright_pipeline{};
    ResonateRenderPipeline blur_pipeline{};
    ResonateRenderPipeline composite_pipeline{};
    ResonateRenderPipeline fxaa_pipeline{};
    ResonateRenderPipeline ui_pipeline{};
    bool pipelines_ready = false;
    bool pipelines_failed = false;
    bool ready_noted = false;
    /* Set by whichever thread hits the first failure: the producer can refuse
       queries while the render thread reports a pipeline. */
    std::atomic<bool> complained{false};

    GpuMesh cube{};
    GpuMesh ground{};
    MeshId bound_mesh = kMeshNone;
    /* The fullscreen triangle of the post chain; a vertex buffer keeps the
       vertex stage free of the base-vertex builtin. */
    ResonateRenderBuffer post_vertex_buffer{};

    /* The overlay's quad vertices, rewritten every frame the list is non-empty.
       The render thread owns the buffer and the scratch. */
    ResonateRenderBuffer ui_vertices{};
    std::vector<UiVertex> ui_vertex_scratch;
    std::vector<UiBatch> ui_batches;
    std::uint32_t ui_drawn_vertices = 0U;
    UiConstants ui_constants{};

    /* The two albedo textures v1 ships: a white 1x1 (material id 0) and an
       8x8 checker (id 1). Created once; their table slots ride the push
       constants. */
    ResonateRenderImage white_texture{};
    ResonateRenderImage checker_texture{};
    std::uint32_t white_texture_slot = 0U;
    std::uint32_t checker_texture_slot = 0U;
    bool albedo_ready = false;

    /* The producer's world, for the optional Material lookup. */
    ecs::World* extract_world = nullptr;

    /* The frame's per-view data, rewritten every frame. The cluster outputs
       are graph transients: the executor materializes them and its barriers
       hand them from compute to the scene pass. */
    GpuMesh frame_data{};
    GpuMesh light_list{};
    FrameConstants constants{};
    PostConstants post_constants{};

    /* The slots the per-frame buffers landed at, recorded at creation. */
    std::uint32_t frame_data_slot = 0U;
    std::uint32_t light_list_slot = 0U;

    void complain(const char* what)
    {
        if (!complained.exchange(true))
        {
            std::fprintf(stderr, "renderer: %s\n", what);
        }
    }

    bool initialize();
    bool createAlbedoTextures();
    bool ensurePipelines();
    ResonateRenderBuffer uploadStorage(std::uint64_t size_bytes, bool host_visible);
    ResonateRenderBuffer uploadUniform(std::uint64_t size_bytes);
    std::uint32_t slotOf(ResonateRenderBuffer handle) const;
    std::uint32_t slotOf(ResonateRenderImage handle) const;
    std::uint32_t uiTextureSlot(std::uint32_t texture) const;
    void bindQueries(ecs::World& world);
    void resolveCapabilities();
    void ensureThread();
    bool extract(ecs::World& world);
    void publish();
    void renderLoop();
    bool prepareTargets(uint32_t width, uint32_t height);
    void renderFrame(FrameSnapshot& snapshot);
    void prepareFrame(GraphExecuteContext& commands);
    void runClusters(GraphExecuteContext& commands, ResonateRenderPipeline pipeline);
    void runShadows(GraphExecuteContext& commands, std::uint32_t cascade);
    void drawScene(GraphExecuteContext& commands);
    void drawUi(GraphExecuteContext& commands);
    void runPost(GraphExecuteContext& commands, ResourceId target, ResonateRenderPipeline pipeline,
                 std::uint32_t input_slot, std::uint32_t bloom_slot, std::uint32_t ui_slot,
                 const Vec4& params);
    static void prepareEntry(void* context, GraphExecuteContext& commands);
    static void clusterBuildEntry(void* context, GraphExecuteContext& commands);
    static void clusterCullEntry(void* context, GraphExecuteContext& commands);
    static void shadow0Entry(void* context, GraphExecuteContext& commands);
    static void shadow1Entry(void* context, GraphExecuteContext& commands);
    static void shadow2Entry(void* context, GraphExecuteContext& commands);
    static void drawSceneEntry(void* context, GraphExecuteContext& commands);
    static void uiEntry(void* context, GraphExecuteContext& commands);
    static void brightEntry(void* context, GraphExecuteContext& commands);
    static void blurHEntry(void* context, GraphExecuteContext& commands);
    static void blurVEntry(void* context, GraphExecuteContext& commands);
    static void compositeEntry(void* context, GraphExecuteContext& commands);
    static void fxaaEntry(void* context, GraphExecuteContext& commands);
};

bool RenderSystem::Impl::initialize()
{
    /* The fixed spine: three CSM shadow passes, two cluster compute passes and
       the lit scene into HDR. The post chain after it comes from the feature
       stack: bloom adds its extraction and blur passes, tonemap composites, and
       the AA slot either runs FXAA into the canvas or leaves the composite's
       output there. */
    const bool bloom = features.has("bloom");
    const bool fxaa = features.antialiases();

    resonate::render::RenderGraph graph;
    canvas_resource = graph.importImage("backbuffer", kLayoutUndefined, kLayoutPresent);
    depth_resource = graph.declareImage("depth", RESONATE_RENDER_FORMAT_D32_SFLOAT, target_width,
                                        target_height, RESONATE_RENDER_IMAGE_DEPTH_ATTACHMENT);
    hdr_resource = graph.declareImage(
        "hdr", RESONATE_RENDER_FORMAT_R16G16B16A16_SFLOAT, target_width, target_height,
        RESONATE_RENDER_IMAGE_COLOR_ATTACHMENT | RESONATE_RENDER_IMAGE_SAMPLED);
    /* The UI overlay is straight-alpha color the composite blends over the
       tonemapped frame; it is always declared, even with no widgets, so the
       pass can clear a reused target. */
    ui_resource = graph.declareImage(
        "ui", RESONATE_RENDER_FORMAT_R8G8B8A8_UNORM, target_width, target_height,
        RESONATE_RENDER_IMAGE_COLOR_ATTACHMENT | RESONATE_RENDER_IMAGE_SAMPLED);

    /* Resources a feature does not ask for stay invalid: no pass names them and
       the executor does not create them. */
    ldr_resource = resonate::render::kInvalidResource;
    bright_resource = resonate::render::kInvalidResource;
    blur_a_resource = resonate::render::kInvalidResource;
    blur_b_resource = resonate::render::kInvalidResource;
    if (fxaa)
    {
        ldr_resource = graph.declareImage(
            "ldr", RESONATE_RENDER_FORMAT_R8G8B8A8_UNORM, target_width, target_height,
            RESONATE_RENDER_IMAGE_COLOR_ATTACHMENT | RESONATE_RENDER_IMAGE_SAMPLED);
    }
    if (bloom)
    {
        bright_resource = graph.declareImage(
            "bright", RESONATE_RENDER_FORMAT_R16G16B16A16_SFLOAT, bloom_width, bloom_height,
            RESONATE_RENDER_IMAGE_COLOR_ATTACHMENT | RESONATE_RENDER_IMAGE_SAMPLED);
        blur_a_resource = graph.declareImage(
            "blur_a", RESONATE_RENDER_FORMAT_R16G16B16A16_SFLOAT, bloom_width, bloom_height,
            RESONATE_RENDER_IMAGE_COLOR_ATTACHMENT | RESONATE_RENDER_IMAGE_SAMPLED);
        blur_b_resource = graph.declareImage(
            "blur_b", RESONATE_RENDER_FORMAT_R16G16B16A16_SFLOAT, bloom_width, bloom_height,
            RESONATE_RENDER_IMAGE_COLOR_ATTACHMENT | RESONATE_RENDER_IMAGE_SAMPLED);
    }
    for (std::uint32_t cascade = 0; cascade < kCascades; ++cascade)
    {
        shadow_resource[cascade] = graph.declareImage(
            "shadow", RESONATE_RENDER_FORMAT_D32_SFLOAT, 2048U, 2048U,
            RESONATE_RENDER_IMAGE_DEPTH_ATTACHMENT | RESONATE_RENDER_IMAGE_SAMPLED);
    }
    boxes_resource = graph.declareBuffer("cluster_boxes", kClusterCount * sizeof(ShaderClusterAABB),
                                         RESONATE_RENDER_BUFFER_STORAGE);
    grid_resource = graph.declareBuffer("light_grid", kClusterCount * sizeof(ShaderClusterLights),
                                        RESONATE_RENDER_BUFFER_STORAGE);
    indices_resource = graph.declareBuffer(
        "light_indices", kClusterCount * kMaxLightsPerCluster * sizeof(std::uint32_t),
        RESONATE_RENDER_BUFFER_STORAGE);

    Impl* const self = this;
    for (std::uint32_t cascade = 0; cascade < kCascades; ++cascade)
    {
        Use shadow_write{};
        shadow_write.resource = shadow_resource[cascade];
        shadow_write.stages = resonate::render::kStageDepthOutput;
        shadow_write.access = resonate::render::kAccessDepthWrite;
        shadow_write.layout = kLayoutDepthAttachment;
        resonate::render::PassDesc shadow{};
        shadow.name = "shadow";
        shadow.writes = &shadow_write;
        shadow.write_count = 1U;
        shadow.context = self;
        shadow.execute = cascade == 0U
                             ? &Impl::shadow0Entry
                             : (cascade == 1U ? &Impl::shadow1Entry : &Impl::shadow2Entry);
        graph.addPass(shadow);
    }

    Use boxes_write{};
    boxes_write.resource = boxes_resource;
    boxes_write.stages = resonate::render::kStageCompute;
    boxes_write.access = resonate::render::kAccessShaderWrite;
    resonate::render::PassDesc cluster_build{};
    cluster_build.name = "cluster-build";
    cluster_build.queue = resonate::render::Queue::Compute;
    cluster_build.writes = &boxes_write;
    cluster_build.write_count = 1U;
    cluster_build.context = this;
    cluster_build.execute = &Impl::clusterBuildEntry;
    graph.addPass(cluster_build);

    Use cull_reads[1]{};
    cull_reads[0].resource = boxes_resource;
    cull_reads[0].stages = resonate::render::kStageCompute;
    cull_reads[0].access = resonate::render::kAccessShaderRead;
    Use cull_writes[2]{};
    cull_writes[0].resource = grid_resource;
    cull_writes[0].stages = resonate::render::kStageCompute;
    cull_writes[0].access = resonate::render::kAccessShaderWrite;
    cull_writes[1].resource = indices_resource;
    cull_writes[1].stages = resonate::render::kStageCompute;
    cull_writes[1].access = resonate::render::kAccessShaderWrite;
    resonate::render::PassDesc cluster_cull{};
    cluster_cull.name = "cluster-cull";
    cluster_cull.queue = resonate::render::Queue::Compute;
    cluster_cull.reads = cull_reads;
    cluster_cull.read_count = 1U;
    cluster_cull.writes = cull_writes;
    cluster_cull.write_count = 2U;
    cluster_cull.context = this;
    cluster_cull.execute = &Impl::clusterCullEntry;
    graph.addPass(cluster_cull);

    Use scene_reads[5]{};
    scene_reads[0].resource = grid_resource;
    scene_reads[0].stages = resonate::render::kStageFragment;
    scene_reads[0].access = resonate::render::kAccessShaderRead;
    scene_reads[0].layout = kLayoutShaderReadOnly;
    scene_reads[1].resource = indices_resource;
    scene_reads[1].stages = resonate::render::kStageFragment;
    scene_reads[1].access = resonate::render::kAccessShaderRead;
    scene_reads[1].layout = kLayoutShaderReadOnly;
    for (std::uint32_t cascade = 0; cascade < kCascades; ++cascade)
    {
        scene_reads[2 + cascade].resource = shadow_resource[cascade];
        scene_reads[2 + cascade].stages = resonate::render::kStageFragment;
        scene_reads[2 + cascade].access = resonate::render::kAccessShaderRead;
        scene_reads[2 + cascade].layout = kLayoutShaderReadOnly;
    }
    Use scene_write{};
    scene_write.resource = hdr_resource;
    scene_write.stages = resonate::render::kStageColorOutput;
    scene_write.access = resonate::render::kAccessColorWrite;
    scene_write.layout = kLayoutColorAttachment;
    /* The depth target is a graph resource too, so its layout and lifetime are
       tracked like the color target's. */
    Use scene_writes[2] = {scene_write, Use{}};
    scene_writes[1].resource = depth_resource;
    scene_writes[1].stages = resonate::render::kStageDepthOutput;
    scene_writes[1].access = resonate::render::kAccessDepthWrite;
    scene_writes[1].layout = kLayoutDepthAttachment;
    resonate::render::PassDesc scene{};
    scene.name = "scene";
    scene.reads = scene_reads;
    scene.read_count = 5U;
    scene.writes = scene_writes;
    scene.write_count = 2U;
    scene.context = this;
    scene.execute = &Impl::drawSceneEntry;
    graph.addPass(scene);

    if (bloom)
    {
        Use hdr_read{};
        hdr_read.resource = hdr_resource;
        hdr_read.stages = resonate::render::kStageFragment;
        hdr_read.access = resonate::render::kAccessShaderRead;
        hdr_read.layout = kLayoutShaderReadOnly;
        Use bright_write{};
        bright_write.resource = bright_resource;
        bright_write.stages = resonate::render::kStageColorOutput;
        bright_write.access = resonate::render::kAccessColorWrite;
        bright_write.layout = kLayoutColorAttachment;
        resonate::render::PassDesc bright{};
        bright.name = "bright";
        bright.reads = &hdr_read;
        bright.read_count = 1U;
        bright.writes = &bright_write;
        bright.write_count = 1U;
        bright.context = this;
        bright.execute = &Impl::brightEntry;
        graph.addPass(bright);

        Use bright_read{};
        bright_read.resource = bright_resource;
        bright_read.stages = resonate::render::kStageFragment;
        bright_read.access = resonate::render::kAccessShaderRead;
        bright_read.layout = kLayoutShaderReadOnly;
        Use blur_a_write{};
        blur_a_write.resource = blur_a_resource;
        blur_a_write.stages = resonate::render::kStageColorOutput;
        blur_a_write.access = resonate::render::kAccessColorWrite;
        blur_a_write.layout = kLayoutColorAttachment;
        resonate::render::PassDesc blur_h{};
        blur_h.name = "blur-h";
        blur_h.reads = &bright_read;
        blur_h.read_count = 1U;
        blur_h.writes = &blur_a_write;
        blur_h.write_count = 1U;
        blur_h.context = this;
        blur_h.execute = &Impl::blurHEntry;
        graph.addPass(blur_h);

        Use blur_a_read{};
        blur_a_read.resource = blur_a_resource;
        blur_a_read.stages = resonate::render::kStageFragment;
        blur_a_read.access = resonate::render::kAccessShaderRead;
        blur_a_read.layout = kLayoutShaderReadOnly;
        Use blur_b_write{};
        blur_b_write.resource = blur_b_resource;
        blur_b_write.stages = resonate::render::kStageColorOutput;
        blur_b_write.access = resonate::render::kAccessColorWrite;
        blur_b_write.layout = kLayoutColorAttachment;
        resonate::render::PassDesc blur_v{};
        blur_v.name = "blur-v";
        blur_v.reads = &blur_a_read;
        blur_v.read_count = 1U;
        blur_v.writes = &blur_b_write;
        blur_v.write_count = 1U;
        blur_v.context = this;
        blur_v.execute = &Impl::blurVEntry;
        graph.addPass(blur_v);
    }

    /* The overlay pass comes after the scene's post chain and before the
       composite: it clears its own target and every quad blends into it in
       paint order. */
    Use ui_write{};
    ui_write.resource = ui_resource;
    ui_write.stages = resonate::render::kStageColorOutput;
    ui_write.access = resonate::render::kAccessColorWrite;
    ui_write.layout = kLayoutColorAttachment;
    resonate::render::PassDesc ui{};
    ui.name = "ui";
    ui.writes = &ui_write;
    ui.write_count = 1U;
    ui.context = this;
    ui.execute = &Impl::uiEntry;
    graph.addPass(ui);

    /* Without an AA pass the composite owns the canvas; with one it writes the
       LDR target FXAA reads. */
    const ResourceId composite_target = fxaa ? ldr_resource : canvas_resource;
    Use composite_reads[3]{};
    composite_reads[0].resource = hdr_resource;
    composite_reads[0].stages = resonate::render::kStageFragment;
    composite_reads[0].access = resonate::render::kAccessShaderRead;
    composite_reads[0].layout = kLayoutShaderReadOnly;
    std::uint32_t composite_read_count = 1U;
    if (bloom)
    {
        composite_reads[composite_read_count].resource = blur_b_resource;
        composite_reads[composite_read_count].stages = resonate::render::kStageFragment;
        composite_reads[composite_read_count].access = resonate::render::kAccessShaderRead;
        composite_reads[composite_read_count].layout = kLayoutShaderReadOnly;
        ++composite_read_count;
    }
    composite_reads[composite_read_count].resource = ui_resource;
    composite_reads[composite_read_count].stages = resonate::render::kStageFragment;
    composite_reads[composite_read_count].access = resonate::render::kAccessShaderRead;
    composite_reads[composite_read_count].layout = kLayoutShaderReadOnly;
    ++composite_read_count;
    Use composite_write{};
    composite_write.resource = composite_target;
    composite_write.stages = resonate::render::kStageColorOutput;
    composite_write.access = resonate::render::kAccessColorWrite;
    composite_write.layout = kLayoutColorAttachment;
    resonate::render::PassDesc composite{};
    composite.name = "composite";
    composite.reads = composite_reads;
    composite.read_count = composite_read_count;
    composite.writes = &composite_write;
    composite.write_count = 1U;
    composite.context = this;
    composite.execute = &Impl::compositeEntry;
    graph.addPass(composite);

    if (fxaa)
    {
        Use ldr_read{};
        ldr_read.resource = ldr_resource;
        ldr_read.stages = resonate::render::kStageFragment;
        ldr_read.access = resonate::render::kAccessShaderRead;
        ldr_read.layout = kLayoutShaderReadOnly;
        Use canvas_write{};
        canvas_write.resource = canvas_resource;
        canvas_write.stages = resonate::render::kStageColorOutput;
        canvas_write.access = resonate::render::kAccessColorWrite;
        canvas_write.layout = kLayoutColorAttachment;
        resonate::render::PassDesc fxaa{};
        fxaa.name = "fxaa";
        fxaa.reads = &ldr_read;
        fxaa.read_count = 1U;
        fxaa.writes = &canvas_write;
        fxaa.write_count = 1U;
        fxaa.context = this;
        fxaa.execute = &Impl::fxaaEntry;
        graph.addPass(fxaa);
    }
    graph.exportResource(canvas_resource);

    std::string error;
    if (!graph.compile(plan, error))
    {
        complain(error.c_str());
        return false;
    }
    executor = std::make_unique<resonate::render::GraphExecutor>(*device, plan, canvas_resource);
    return true;
}

ResonateRenderBuffer RenderSystem::Impl::uploadStorage(std::uint64_t size_bytes, bool host_visible)
{
    ResonateRenderBufferDesc desc{};
    desc.struct_size = sizeof(desc);
    desc.size_bytes = size_bytes;
    desc.usage = RESONATE_RENDER_BUFFER_STORAGE;
    desc.host_visible = host_visible ? 1U : 0U;
    ResonateRenderBuffer buffer{};
    if (device->create_buffer(device->self, &desc, &buffer) != RESONATE_OK)
    {
        complain("a storage buffer was refused");
        return ResonateRenderBuffer{};
    }
    return buffer;
}

ResonateRenderBuffer RenderSystem::Impl::uploadUniform(std::uint64_t size_bytes)
{
    ResonateRenderBufferDesc desc{};
    desc.struct_size = sizeof(desc);
    desc.size_bytes = size_bytes;
    desc.usage = RESONATE_RENDER_BUFFER_UNIFORM;
    desc.host_visible = 0U;
    ResonateRenderBuffer buffer{};
    if (device->create_buffer(device->self, &desc, &buffer) != RESONATE_OK)
    {
        complain("a uniform buffer was refused");
        return ResonateRenderBuffer{};
    }
    return buffer;
}

std::uint32_t RenderSystem::Impl::slotOf(ResonateRenderBuffer handle) const
{
    if (device != nullptr && device->buffer_slot != nullptr)
    {
        return device->buffer_slot(device->self, handle);
    }
    return static_cast<std::uint32_t>(handle.value & 0xFFFFFFFFU);
}

std::uint32_t RenderSystem::Impl::slotOf(ResonateRenderImage handle) const
{
    if (device != nullptr && device->image_slot != nullptr)
    {
        return device->image_slot(device->self, handle);
    }
    return static_cast<std::uint32_t>(handle.value & 0xFFFFFFFFU);
}

std::uint32_t RenderSystem::Impl::uiTextureSlot(std::uint32_t texture) const
{
    if (texture == ui::kTextureWhite)
    {
        return kSolidUiTextureSlot;
    }
    if (!albedo_ready)
    {
        return kSolidUiTextureSlot;
    }
    return texture == ui::kTextureChecker ? checker_texture_slot : white_texture_slot;
}

bool RenderSystem::Impl::ensurePipelines()
{
    if (pipelines_ready)
    {
        return true;
    }
    if (pipelines_failed)
    {
        return false;
    }

    const std::string directory = shadersDirectory();
    if (directory.empty())
    {
        complain("no shaders directory above the executable");
        pipelines_failed = true;
        return false;
    }

    /* A device without descriptor indexing gets the shadow shader that samples
       the three cascades through push-constant slots and selects, instead of
       indexing the image table per pixel. */
    const bool compat_shadows =
        device->header.struct_size >=
            offsetof(ResonateRenderDevice, feature_bits) + sizeof(device->feature_bits) &&
        device->feature_bits != nullptr &&
        (device->feature_bits(device->self) & RESONATE_RENDER_FEATURE_DESCRIPTOR_INDEXING) == 0U;
    const std::vector<char> vertex_spirv =
        readFile(std::filesystem::path(directory) / "scene.sceneVertex.spv");
    const std::vector<char> fragment_spirv = readFile(
        std::filesystem::path(directory) /
        (compat_shadows ? "scene.sceneFragmentCompat.spv" : "scene.sceneFragment.spv"));
    const std::vector<char> shadow_vertex_spirv =
        readFile(std::filesystem::path(directory) / "shadow.shadowVertex.spv");
    const std::vector<char> shadow_fragment_spirv =
        readFile(std::filesystem::path(directory) / "shadow.shadowFragment.spv");
    const std::vector<char> build_spirv =
        readFile(std::filesystem::path(directory) / "cluster.clusterBuild.spv");
    const std::vector<char> cull_spirv =
        readFile(std::filesystem::path(directory) / "cluster.clusterCull.spv");
    const std::vector<char> post_vertex_spirv =
        readFile(std::filesystem::path(directory) / "post.postVertex.spv");
    const std::vector<char> bright_spirv =
        readFile(std::filesystem::path(directory) / "post.brightFragment.spv");
    const std::vector<char> blur_spirv =
        readFile(std::filesystem::path(directory) / "post.blurFragment.spv");
    const std::vector<char> composite_spirv =
        readFile(std::filesystem::path(directory) / "post.compositeFragment.spv");
    const std::vector<char> fxaa_spirv =
        readFile(std::filesystem::path(directory) / "post.fxaaFragment.spv");
    const std::vector<char> ui_vertex_spirv =
        readFile(std::filesystem::path(directory) / "ui.uiVertex.spv");
    const std::vector<char> ui_fragment_spirv =
        readFile(std::filesystem::path(directory) / "ui.uiFragment.spv");
    if (vertex_spirv.empty() || fragment_spirv.empty() || shadow_vertex_spirv.empty() ||
        shadow_fragment_spirv.empty() || build_spirv.empty() || cull_spirv.empty() ||
        post_vertex_spirv.empty() || bright_spirv.empty() || blur_spirv.empty() ||
        composite_spirv.empty() || fxaa_spirv.empty() || ui_vertex_spirv.empty() ||
        ui_fragment_spirv.empty())
    {
        complain("shader files missing; run `xmake slang`");
        pipelines_failed = true;
        return false;
    }

    ResonateRenderShader vertex{};
    ResonateRenderShader fragment{};
    ResonateRenderShader shadow_vertex{};
    ResonateRenderShader shadow_fragment{};
    ResonateRenderShader build{};
    ResonateRenderShader cull{};
    ResonateRenderShader post_vertex{};
    ResonateRenderShader bright{};
    ResonateRenderShader blur{};
    ResonateRenderShader composite{};
    ResonateRenderShader fxaa{};
    ResonateRenderShader ui_vertex{};
    ResonateRenderShader ui_fragment{};
    if (device->create_shader(device->self, RESONATE_RENDER_SHADER_VERTEX, vertex_spirv.data(),
                              vertex_spirv.size(), &vertex) != RESONATE_OK ||
        device->create_shader(device->self, RESONATE_RENDER_SHADER_FRAGMENT, fragment_spirv.data(),
                              fragment_spirv.size(), &fragment) != RESONATE_OK ||
        device->create_shader(device->self, RESONATE_RENDER_SHADER_VERTEX,
                              shadow_vertex_spirv.data(), shadow_vertex_spirv.size(),
                              &shadow_vertex) != RESONATE_OK ||
        device->create_shader(device->self, RESONATE_RENDER_SHADER_FRAGMENT,
                              shadow_fragment_spirv.data(), shadow_fragment_spirv.size(),
                              &shadow_fragment) != RESONATE_OK ||
        device->create_shader(device->self, RESONATE_RENDER_SHADER_COMPUTE, build_spirv.data(),
                              build_spirv.size(), &build) != RESONATE_OK ||
        device->create_shader(device->self, RESONATE_RENDER_SHADER_COMPUTE, cull_spirv.data(),
                              cull_spirv.size(), &cull) != RESONATE_OK ||
        device->create_shader(device->self, RESONATE_RENDER_SHADER_VERTEX, post_vertex_spirv.data(),
                              post_vertex_spirv.size(), &post_vertex) != RESONATE_OK ||
        device->create_shader(device->self, RESONATE_RENDER_SHADER_FRAGMENT, bright_spirv.data(),
                              bright_spirv.size(), &bright) != RESONATE_OK ||
        device->create_shader(device->self, RESONATE_RENDER_SHADER_FRAGMENT, blur_spirv.data(),
                              blur_spirv.size(), &blur) != RESONATE_OK ||
        device->create_shader(device->self, RESONATE_RENDER_SHADER_FRAGMENT, composite_spirv.data(),
                              composite_spirv.size(), &composite) != RESONATE_OK ||
        device->create_shader(device->self, RESONATE_RENDER_SHADER_FRAGMENT, fxaa_spirv.data(),
                              fxaa_spirv.size(), &fxaa) != RESONATE_OK ||
        device->create_shader(device->self, RESONATE_RENDER_SHADER_VERTEX, ui_vertex_spirv.data(),
                              ui_vertex_spirv.size(), &ui_vertex) != RESONATE_OK ||
        device->create_shader(device->self, RESONATE_RENDER_SHADER_FRAGMENT,
                              ui_fragment_spirv.data(), ui_fragment_spirv.size(),
                              &ui_fragment) != RESONATE_OK)
    {
        complain("shader creation failed");
        pipelines_failed = true;
        return false;
    }

    ResonateRenderVertexAttribute attributes[4] = {};
    attributes[0].location = 0U;
    attributes[0].format = RESONATE_RENDER_FORMAT_R32G32B32_SFLOAT;
    attributes[0].offset_bytes = 0U;
    attributes[1].location = 1U;
    attributes[1].format = RESONATE_RENDER_FORMAT_R32G32B32_SFLOAT;
    attributes[1].offset_bytes = 12U;
    attributes[2].location = 2U;
    attributes[2].format = RESONATE_RENDER_FORMAT_R32G32B32_SFLOAT;
    attributes[2].offset_bytes = 24U;
    attributes[3].location = 3U;
    attributes[3].format = RESONATE_RENDER_FORMAT_R32G32_SFLOAT;
    attributes[3].offset_bytes = 36U;

    ResonateRenderVertexStream stream{};
    stream.stride_bytes = sizeof(Vertex);
    stream.attribute_count = 4U;
    stream.attributes = attributes;

    ResonateRenderGraphicsPipelineDesc scene_desc{};
    scene_desc.struct_size = sizeof(scene_desc);
    scene_desc.vertex_shader = vertex;
    scene_desc.fragment_shader = fragment;
    scene_desc.stream_count = 1U;
    scene_desc.streams = &stream;
    scene_desc.color_format = RESONATE_RENDER_FORMAT_R16G16B16A16_SFLOAT;
    scene_desc.depth_format = RESONATE_RENDER_FORMAT_D32_SFLOAT;
    scene_desc.depth_test = 1U;
    scene_desc.depth_write = 1U;
    if (device->create_pipeline(device->self, &scene_desc, &scene_pipeline) != RESONATE_OK)
    {
        complain("scene pipeline creation failed");
        pipelines_failed = true;
        return false;
    }

    ResonateRenderVertexAttribute shadow_attributes[1] = {};
    shadow_attributes[0].location = 0U;
    shadow_attributes[0].format = RESONATE_RENDER_FORMAT_R32G32B32_SFLOAT;
    shadow_attributes[0].offset_bytes = 0U;
    ResonateRenderVertexStream shadow_stream{};
    shadow_stream.stride_bytes = sizeof(Vertex);
    shadow_stream.attribute_count = 1U;
    shadow_stream.attributes = shadow_attributes;
    ResonateRenderGraphicsPipelineDesc shadow_desc{};
    shadow_desc.struct_size = sizeof(shadow_desc);
    shadow_desc.vertex_shader = shadow_vertex;
    shadow_desc.fragment_shader = shadow_fragment;
    shadow_desc.stream_count = 1U;
    shadow_desc.streams = &shadow_stream;
    shadow_desc.color_format = RESONATE_RENDER_FORMAT_NONE;
    shadow_desc.depth_format = RESONATE_RENDER_FORMAT_D32_SFLOAT;
    shadow_desc.depth_test = 1U;
    shadow_desc.depth_write = 1U;
    if (device->create_pipeline(device->self, &shadow_desc, &shadow_pipeline) != RESONATE_OK)
    {
        complain("shadow pipeline creation failed");
        pipelines_failed = true;
        return false;
    }

    if (device->create_compute_pipeline(device->self, build, &build_pipeline) != RESONATE_OK ||
        device->create_compute_pipeline(device->self, cull, &cull_pipeline) != RESONATE_OK)
    {
        complain("cluster pipeline creation failed");
        pipelines_failed = true;
        return false;
    }

    /* Without an AA pass the composite owns the canvas, so its pipeline names
       the swapchain format instead of the LDR target's. */
    const ResonateRenderFormat composite_format = features.antialiases()
                                                      ? RESONATE_RENDER_FORMAT_R8G8B8A8_UNORM
                                                      : RESONATE_RENDER_FORMAT_UNKNOWN;
    const ResonateRenderFormat post_targets[4] = {RESONATE_RENDER_FORMAT_R16G16B16A16_SFLOAT,
                                                  RESONATE_RENDER_FORMAT_R16G16B16A16_SFLOAT,
                                                  composite_format, RESONATE_RENDER_FORMAT_UNKNOWN};
    ResonateRenderShader* post_fragments[4] = {&bright, &blur, &composite, &fxaa};
    ResonateRenderPipeline* post_pipelines[4] = {&bright_pipeline, &blur_pipeline,
                                                 &composite_pipeline, &fxaa_pipeline};

    /* The fullscreen triangle carries its positions and UVs in a vertex buffer,
       so the vertex stage needs no base-vertex builtin (shaderDrawParameters is
       optional on some Vulkan 1.1 mobile drivers). */
    struct PostVertex
    {
        float position[2];
        float uv[2];
    };
    const PostVertex vertices[3] = {
        {{-1.0F, -1.0F}, {0.0F, 0.0F}},
        {{3.0F, -1.0F}, {2.0F, 0.0F}},
        {{-1.0F, 3.0F}, {0.0F, 2.0F}},
    };
    ResonateRenderBufferDesc post_buffer{};
    post_buffer.struct_size = sizeof(post_buffer);
    post_buffer.size_bytes = sizeof(vertices);
    post_buffer.usage = RESONATE_RENDER_BUFFER_VERTEX;
    post_buffer.host_visible = 1U;
    if (device->create_buffer(device->self, &post_buffer, &post_vertex_buffer) != RESONATE_OK ||
        device->upload_buffer(device->self, post_vertex_buffer, 0U, vertices, sizeof(vertices)) !=
            RESONATE_OK)
    {
        complain("the post vertex buffer was not created");
        pipelines_failed = true;
        return false;
    }

    ResonateRenderVertexAttribute post_attributes[2] = {};
    post_attributes[0].location = 0U;
    post_attributes[0].format = RESONATE_RENDER_FORMAT_R32G32_SFLOAT;
    post_attributes[0].offset_bytes = 0U;
    post_attributes[1].location = 1U;
    post_attributes[1].format = RESONATE_RENDER_FORMAT_R32G32_SFLOAT;
    post_attributes[1].offset_bytes = 8U;
    ResonateRenderVertexStream post_stream{};
    post_stream.stride_bytes = sizeof(PostVertex);
    post_stream.attribute_count = 2U;
    post_stream.attributes = post_attributes;

    for (std::uint32_t i = 0; i < 4U; ++i)
    {
        ResonateRenderGraphicsPipelineDesc post_desc{};
        post_desc.struct_size = sizeof(post_desc);
        post_desc.vertex_shader = post_vertex;
        post_desc.fragment_shader = *post_fragments[i];
        post_desc.stream_count = 1U;
        post_desc.streams = &post_stream;
        post_desc.color_format = post_targets[i];
        post_desc.depth_format = RESONATE_RENDER_FORMAT_UNKNOWN;
        if (device->create_pipeline(device->self, &post_desc, post_pipelines[i]) != RESONATE_OK)
        {
            complain("post pipeline creation failed");
            pipelines_failed = true;
            return false;
        }
    }

    /* The overlay: target-pixel positions, straight-alpha colors and sampled
       UVs, blended into the UI image. */
    ResonateRenderVertexAttribute ui_attributes[3] = {};
    ui_attributes[0].location = 0U;
    ui_attributes[0].format = RESONATE_RENDER_FORMAT_R32G32_SFLOAT;
    ui_attributes[0].offset_bytes = 0U;
    ui_attributes[1].location = 1U;
    ui_attributes[1].format = RESONATE_RENDER_FORMAT_R32G32B32A32_SFLOAT;
    ui_attributes[1].offset_bytes = 8U;
    ui_attributes[2].location = 2U;
    ui_attributes[2].format = RESONATE_RENDER_FORMAT_R32G32_SFLOAT;
    ui_attributes[2].offset_bytes = 24U;
    ResonateRenderVertexStream ui_stream{};
    ui_stream.stride_bytes = sizeof(UiVertex);
    ui_stream.attribute_count = 3U;
    ui_stream.attributes = ui_attributes;

    ResonateRenderGraphicsPipelineDesc ui_desc{};
    ui_desc.struct_size = sizeof(ui_desc);
    ui_desc.vertex_shader = ui_vertex;
    ui_desc.fragment_shader = ui_fragment;
    ui_desc.stream_count = 1U;
    ui_desc.streams = &ui_stream;
    ui_desc.color_format = RESONATE_RENDER_FORMAT_R8G8B8A8_UNORM;
    ui_desc.depth_format = RESONATE_RENDER_FORMAT_UNKNOWN;
    ui_desc.blend = 1U;
    if (device->create_pipeline(device->self, &ui_desc, &ui_pipeline) != RESONATE_OK)
    {
        complain("ui pipeline creation failed");
        pipelines_failed = true;
        return false;
    }

    ResonateRenderBufferDesc ui_buffer{};
    ui_buffer.struct_size = sizeof(ui_buffer);
    ui_buffer.size_bytes = sizeof(UiVertex) * 6U * kMaxUiQuads;
    ui_buffer.usage = RESONATE_RENDER_BUFFER_VERTEX;
    ui_buffer.host_visible = 0U;
    if (device->create_buffer(device->self, &ui_buffer, &ui_vertices) != RESONATE_OK)
    {
        complain("the UI vertex buffer was not created");
        pipelines_failed = true;
        return false;
    }
    ui_vertex_scratch.reserve(static_cast<std::size_t>(kMaxUiQuads) * 6U);

    cube = buildCube(*device);
    ground = buildGround(*device, 12.0F);
    /* Device-local: the extract writes them through update_buffer, recorded
       into the frame that reads them. */
    frame_data = GpuMesh{uploadUniform(sizeof(ShaderFrameData)), ResonateRenderBuffer{}, 0U};
    light_list =
        GpuMesh{uploadStorage(kMaxLights * sizeof(ShaderLight), false), ResonateRenderBuffer{}, 0U};
    if (cube.vertices.value == 0U || ground.vertices.value == 0U ||
        frame_data.vertices.value == 0U || light_list.vertices.value == 0U ||
        ui_vertices.value == 0U)
    {
        complain("buffer upload failed");
        pipelines_failed = true;
        return false;
    }

    /* The table slots the push constants carry. */
    frame_data_slot = slotOf(frame_data.vertices);
    light_list_slot = slotOf(light_list.vertices);

    pipelines_ready = true;
    if (!ready_noted)
    {
        ready_noted = true;
        std::fprintf(stderr, "renderer: pipelines ready\n");
    }
    return true;
}

/* The albedo tables v1 ships: white (material 0) and a checker (material 1).
   The checker comes from the cooked KTX2 when the content tree is there; the
   procedural pattern keeps a bare build drawing.

   Created after the first frame on purpose: this Mali driver samples an image
   uploaded before the first present as a constant, ignoring later uploads and
   descriptor rewrites, while the same image created after the first frame
   samples correctly. */
bool RenderSystem::Impl::createAlbedoTextures()
{
    const std::vector<std::uint8_t> white_pixels(4U, 255U);
    TextureImage checker;
    std::string texture_error;
    const std::string content_directory = contentTexturesDirectory();
    const bool checker_loaded =
        !content_directory.empty() &&
        loadTexture(content_directory + "/checker.ktx2", checker, texture_error);
    if (!content_directory.empty() && !checker_loaded)
    {
        std::fprintf(stderr, "renderer: checker.ktx2: %s\n", texture_error.c_str());
    }
    std::uint32_t checker_width = 8U;
    std::uint32_t checker_height = 8U;
    std::vector<std::uint8_t> checker_pixels(8U * 8U * 4U);
    if (checker_loaded)
    {
        checker_width = checker.width;
        checker_height = checker.height;
        checker_pixels = std::move(checker.rgba);
    }
    else
    {
        for (std::uint32_t y = 0; y < 8U; ++y)
        {
            for (std::uint32_t x = 0; x < 8U; ++x)
            {
                const bool light = ((x / 2U) + (y / 2U)) % 2U == 0U;
                const std::uint8_t value = light ? 235U : 120U;
                const std::size_t index = (static_cast<std::size_t>(y) * 8U + x) * 4U;
                checker_pixels[index] = value;
                checker_pixels[index + 1U] = value;
                checker_pixels[index + 2U] = value;
                checker_pixels[index + 3U] = 255U;
            }
        }
    }
    /* Albedo is sRGB-encoded unless the container says otherwise; the device
       linearizes it on sampling. */
    const ResonateRenderFormat checker_format = checker_loaded && !checker.srgb
                                                    ? RESONATE_RENDER_FORMAT_R8G8B8A8_UNORM
                                                    : RESONATE_RENDER_FORMAT_R8G8B8A8_SRGB;
    if (!createSampledTexture(*device, checker_width, checker_height, checker_format,
                              checker_pixels, checker_texture) ||
        !createSampledTexture(*device, 1U, 1U, RESONATE_RENDER_FORMAT_R8G8B8A8_UNORM, white_pixels,
                              white_texture))
    {
        complain("texture creation failed");
        return false;
    }
    white_texture_slot = slotOf(white_texture);
    checker_texture_slot = slotOf(checker_texture);
    return true;
}

void RenderSystem::Impl::bindQueries(ecs::World& world)
{
    if (queries_ready)
    {
        return;
    }
    const ecs::ComponentIndex instance_all[] = {
        ecs::ComponentTraits<render::MeshInstance>::index,
        ecs::ComponentTraits<hierarchy::WorldTransform>::index};
    instances = world.createQuery(ecs::QueryDesc{instance_all, {}, {}});
    const ecs::ComponentIndex camera_all[] = {
        ecs::ComponentTraits<render::Camera>::index,
        ecs::ComponentTraits<hierarchy::WorldTransform>::index};
    cameras = world.createQuery(ecs::QueryDesc{camera_all, {}, {}});
    const ecs::ComponentIndex light_all[] = {
        ecs::ComponentTraits<render::PointLight>::index,
        ecs::ComponentTraits<hierarchy::WorldTransform>::index};
    point_lights = world.createQuery(ecs::QueryDesc{light_all, {}, {}});
    const ecs::ComponentIndex directional_all[] = {
        ecs::ComponentTraits<render::DirectionalLight>::index};
    directional_lights = world.createQuery(ecs::QueryDesc{directional_all, {}, {}});
    /* A run without the UI components (a unit test or a headless tool that
       never registers them) renders the scene and no overlay. */
    ui_enabled = ecs::ComponentTraits<ui::Canvas>::index != ecs::kInvalidComponent;
    if (ui_enabled)
    {
        const ecs::ComponentIndex ui_all[] = {ecs::ComponentTraits<ui::Canvas>::index};
        ui_canvases = world.createQuery(ecs::QueryDesc{ui_all, {}, {}});
        ui_enabled = ui_canvases.valid();
    }
    queries_ready =
        instances.valid() && cameras.valid() && point_lights.valid() && directional_lights.valid();
    if (!queries_ready)
    {
        complain("the scene queries were refused");
    }
}

void RenderSystem::Impl::resolveCapabilities()
{
    if (registry == nullptr)
    {
        return;
    }
    uint32_t version = 0;
    void* instance = resonate_capability_find(
        registry, resonate::detail::CapabilityTraits<ResonateRenderDevice>::id.value(), 1U,
        &version);
    if (instance != nullptr)
    {
        device = static_cast<ResonateRenderDevice*>(instance);
    }
    instance = resonate_capability_find(
        registry, resonate::detail::CapabilityTraits<ResonateRenderCanvas>::id.value(), 1U,
        &version);
    if (instance != nullptr)
    {
        canvas = static_cast<ResonateRenderCanvas*>(instance);
    }
}

bool RenderSystem::Impl::extract(ecs::World& world)
{
    bindQueries(world);
    extract_world = &world;
    scratch.instances.clear();
    scratch.lights.clear();
    scratch.ui.clear();
    have_camera = false;
    scratch.target_width = canvas != nullptr ? canvas->width(canvas->self) : 0U;
    scratch.target_height = canvas != nullptr ? canvas->height(canvas->self) : 0U;
    if (!queries_ready || scratch.target_width == 0U || scratch.target_height == 0U)
    {
        return false;
    }

    directional_lights.forEachChunk(
        [](void* context, ChunkView view)
        {
            auto* self = static_cast<RenderSystem::Impl*>(context);
            const Column<render::DirectionalLight> light =
                resonate::ecs::column<render::DirectionalLight>(view);
            if (view.count > 0U)
            {
                self->directional_direction = light.data[0].direction;
                self->directional_color = light.data[0].color;
                self->directional_intensity = light.data[0].intensity;
            }
        },
        this);

    cameras.forEachChunk(
        [](void* context, ChunkView view)
        {
            auto* self = static_cast<RenderSystem::Impl*>(context);
            const Column<render::Camera> camera = resonate::ecs::column<render::Camera>(view);
            const Column<hierarchy::WorldTransform> transform =
                resonate::ecs::column<hierarchy::WorldTransform>(view);
            for (std::uint32_t row = 0; row < view.count; ++row)
            {
                if (self->have_camera)
                {
                    continue;
                }
                self->have_camera = true;
                self->camera_world = transform.data[row].matrix;
                self->fov_radians = camera.data[row].fov_y_degrees * 3.14159265F / 180.0F;
                self->near_z = camera.data[row].near_z;
                self->far_z = camera.data[row].far_z;
            }
        },
        this);

    point_lights.forEachChunk(
        [](void* context, ChunkView view)
        {
            auto* self = static_cast<RenderSystem::Impl*>(context);
            const Column<render::PointLight> light =
                resonate::ecs::column<render::PointLight>(view);
            const Column<hierarchy::WorldTransform> transform =
                resonate::ecs::column<hierarchy::WorldTransform>(view);
            for (std::uint32_t row = 0; row < view.count; ++row)
            {
                if (self->scratch.lights.size() >= kMaxLights)
                {
                    break;
                }
                ShaderLight shader_light{};
                shader_light.position_radius =
                    Vec4{transform.data[row].matrix.m[0][3], transform.data[row].matrix.m[1][3],
                         transform.data[row].matrix.m[2][3], light.data[row].radius};
                shader_light.color_intensity =
                    Vec4{light.data[row].color.x, light.data[row].color.y, light.data[row].color.z,
                         light.data[row].intensity};
                self->scratch.lights.push_back(shader_light);
            }
        },
        this);

    /* The compat path walks a fixed light set, so the set is the lights nearest
       the view centre: taking the first N would leave the visible field dark
       whenever the light order starts far away. */
    std::vector<std::uint32_t> nearest(scratch.lights.size());
    for (std::uint32_t index = 0; index < nearest.size(); ++index)
    {
        nearest[index] = index;
    }
    const Vec3 forward{camera_world.m[0][2], camera_world.m[1][2], camera_world.m[2][2]};
    const Vec3 centre{camera_world.m[0][3] + forward.x * 10.0F,
                      camera_world.m[1][3] + forward.y * 10.0F,
                      camera_world.m[2][3] + forward.z * 10.0F};
    const auto distanceSquared = [&centre](const ShaderLight& light)
    {
        const Vec3 position{light.position_radius.x, light.position_radius.y,
                            light.position_radius.z};
        return lengthSquared(position - centre);
    };
    const auto uniform_lights = static_cast<std::uint32_t>(
        std::min<std::size_t>(scratch.lights.size(), kUniformLights));
    std::partial_sort(nearest.begin(), nearest.begin() + uniform_lights, nearest.end(),
                      [&](std::uint32_t a, std::uint32_t b)
                      {
                          return distanceSquared(scratch.lights[a]) <
                                 distanceSquared(scratch.lights[b]);
                      });
    scratch.frame_data.uniform_light_count = uniform_lights;
    for (std::uint32_t index = 0; index < uniform_lights; ++index)
    {
        scratch.frame_data.uniform_lights[index] = scratch.lights[nearest[index]];
    }

    instances.forEachChunk(
        [](void* context, ChunkView view)
        {
            auto* self = static_cast<RenderSystem::Impl*>(context);
            const Column<render::MeshInstance> mesh =
                resonate::ecs::column<render::MeshInstance>(view);
            const Column<hierarchy::WorldTransform> transform =
                resonate::ecs::column<hierarchy::WorldTransform>(view);
            for (std::uint32_t row = 0; row < view.count; ++row)
            {
                DrawInstance instance;
                instance.model = transform.data[row].matrix;
                instance.mesh = mesh.data[row].mesh == kMeshGround ? kMeshGround : kMeshCube;
                instance.texture = 0U;
                instance.color = mesh.data[row].color;
                /* A material is optional: without one the mesh keeps its
                   vertex-driven color and the white texture. */
                if (const render::Material* material =
                        self->extract_world->get<render::Material>(view.entities[row]);
                    material != nullptr)
                {
                    instance.texture = material->texture;
                    instance.color = Vec3{instance.color.x * material->tint.x,
                                          instance.color.y * material->tint.y,
                                          instance.color.z * material->tint.z};
                    instance.roughness = material->roughness;
                    instance.metallic = material->metallic;
                }
                self->scratch.instances.push_back(instance);
            }
        },
        this);

    const float aspect =
        static_cast<float>(scratch.target_width) / static_cast<float>(scratch.target_height);
    const Mat4 view = have_camera ? rigidInverse(camera_world) : Mat4{};
    const Mat4 projection = perspectiveClip(fov_radians, aspect, near_z, far_z);
    scratch.frame_data.view_proj = projection * view;
    scratch.frame_data.camera_world = camera_world;
    scratch.frame_data.camera_and_near =
        Vec4{camera_world.m[0][3], camera_world.m[1][3], camera_world.m[2][3], near_z};
    scratch.frame_data.direction_and_far =
        Vec4{directional_direction.x, directional_direction.y, directional_direction.z, far_z};
    scratch.frame_data.directional_color_intensity =
        Vec4{directional_color.x, directional_color.y, directional_color.z, directional_intensity};
    /* The cluster grid's pixel tile size travels with the frame data, so the
       fragment shader's lookup follows the target's extent. */
    scratch.frame_data.light_count_and_pad =
        Vec4{static_cast<float>(scratch.lights.size()),
             static_cast<float>(scratch.target_width) / static_cast<float>(kClustersX),
             static_cast<float>(scratch.target_height) / static_cast<float>(kClustersY), 0.0F};

    /* CSM: three logarithmic splits, ascending, the last at 35% of the view
       distance — shadow range trades against texel density. Each cascade's
       ortho box fits the main camera's frustum slice as seen from the light. */
    constexpr float kSplitRatio = 0.35F;
    const Vec3 light_forward = normalize(directional_direction);
    const Vec3 light_side = normalize(cross(Vec3{0.0F, 1.0F, 0.0F}, light_forward));
    const Vec3 light_up = cross(light_forward, light_side);
    float previous_split = near_z;
    float split_values[3] = {0.0F, 0.0F, 0.0F};
    for (std::uint32_t cascade = 0; cascade < kCascades; ++cascade)
    {
        const float split = far_z * std::pow(kSplitRatio, static_cast<float>(kCascades - cascade));
        split_values[cascade] = split;
        Vec3 minimum;
        Vec3 maximum;
        cascadeBounds(projection * view, camera_world, previous_split, split, minimum, maximum);
        const Vec3 center = (minimum + maximum) * 0.5F;
        const Vec3 extents = (maximum - minimum) * 0.5F;
        const float box_width = std::fmax(std::fmax(extents.x, extents.y), extents.z) * 2.0F;
        const Vec3 eye = center - light_forward * (far_z * 0.5F);
        const Mat4 light_view = lookAtLh(eye, light_forward, light_up);
        scratch.frame_data.light_view_proj[cascade] =
            orthographicClip(box_width, box_width, far_z * 0.5F - box_width,
                             far_z * 0.5F + box_width) *
            light_view;
        previous_split = split;
    }
    scratch.frame_data.split_distances =
        Vec4{split_values[0], split_values[1], split_values[2], 0.0F};

    /* The retained tree's paint order is the UI system's business; the frame
       carries the resolved quads and the render thread only uploads them. */
    if (ui_enabled && ui_canvases.valid())
    {
        UiRoot root;
        ui_canvases.forEachChunk(
            [](void* context, ChunkView view)
            {
                auto* out = static_cast<UiRoot*>(context);
                if (!out->found && view.count > 0U)
                {
                    out->entity = view.entities[0];
                    out->found = true;
                }
            },
            &root);
        if (root.found)
        {
            ui::buildDisplayList(world, root.entity, scratch.ui);
        }
    }
    return true;
}

void RenderSystem::Impl::publish()
{
    std::unique_lock<std::mutex> lock(handoff_mutex);
    for (;;)
    {
        for (std::uint32_t slot = 0; slot < kSnapshotCount; ++slot)
        {
            if (slot == active_snapshot)
            {
                continue;
            }
            if (snapshot_pending && slot == pending_snapshot)
            {
                continue;
            }
            std::swap(snapshots[slot], scratch);
            pending_snapshot = slot;
            snapshot_pending = true;
            lock.unlock();
            handoff_wake.notify_one();
            return;
        }
        handoff_wake.wait(lock);
    }
}

void RenderSystem::Impl::prepareFrame(GraphExecuteContext& commands)
{
    if (active == nullptr)
    {
        return;
    }
    /* Recorded into the open frame: the previous frames may still be reading
       the same buffers while the extract runs. */
    ResonateRenderDevice& frame_device = *commands.device;
    frame_device.update_buffer(frame_device.self, frame_data.vertices, 0, &active->frame_data,
                               sizeof(ShaderFrameData));
    if (!active->lights.empty())
    {
        frame_device.update_buffer(frame_device.self, light_list.vertices, 0, active->lights.data(),
                                   active->lights.size() * sizeof(ShaderLight));
    }

    /* The updates land through the transfer stage; the passes that read them
       need the copy visible. Both buffers are renderer-owned, so the graph
       declares no barrier for them. */
    ResonateRenderBarrierDesc barrier{};
    barrier.struct_size = sizeof(barrier);
    barrier.before_stages = RESONATE_RENDER_STAGE_TRANSFER;
    barrier.before_access = RESONATE_RENDER_ACCESS_TRANSFER_WRITE;
    barrier.after_stages = RESONATE_RENDER_STAGE_VERTEX | RESONATE_RENDER_STAGE_FRAGMENT |
                           RESONATE_RENDER_STAGE_COMPUTE;
    barrier.after_access = RESONATE_RENDER_ACCESS_SHADER_READ;
    barrier.buffer = frame_data.vertices;
    frame_device.barrier(frame_device.self, &barrier);
    if (!active->lights.empty())
    {
        barrier.buffer = light_list.vertices;
        frame_device.barrier(frame_device.self, &barrier);
    }

    ui_drawn_vertices = 0U;
    ui_batches.clear();
    ui_constants.viewport_width = static_cast<float>(active->target_width);
    ui_constants.viewport_height = static_cast<float>(active->target_height);
    if (!active->ui.empty())
    {
        const auto quads = active->ui.quads();
        const std::size_t count = std::min<std::size_t>(quads.size(), kMaxUiQuads);
        if (quads.size() > count)
        {
            complain("the UI display list was truncated to the vertex buffer");
        }

        ui_vertex_scratch.clear();
        ui_batches.clear();
        for (std::size_t index = 0; index < count; ++index)
        {
            const ui::Quad& quad = quads[index];
            const float right = quad.x + quad.width;
            const float bottom = quad.y + quad.height;
            const UiVertex top_left{quad.x,  quad.y,   quad.color.x, quad.color.y,
                                    quad.color.z, quad.color.w, quad.uv_min.x, quad.uv_min.y};
            const UiVertex top_right{right, quad.y, quad.color.x, quad.color.y, quad.color.z,
                                     quad.color.w, quad.uv_max.x, quad.uv_min.y};
            const UiVertex bottom_right{right, bottom, quad.color.x, quad.color.y, quad.color.z,
                                        quad.color.w, quad.uv_max.x, quad.uv_max.y};
            const UiVertex bottom_left{quad.x, bottom, quad.color.x, quad.color.y, quad.color.z,
                                       quad.color.w, quad.uv_min.x, quad.uv_max.y};

            const std::uint32_t first = static_cast<std::uint32_t>(ui_vertex_scratch.size());
            if (ui_batches.empty() || ui_batches.back().texture != quad.texture)
            {
                ui_batches.push_back(UiBatch{first, 0U, quad.texture});
            }
            ui_batches.back().vertex_count += 6U;

            ui_vertex_scratch.push_back(top_left);
            ui_vertex_scratch.push_back(top_right);
            ui_vertex_scratch.push_back(bottom_right);
            ui_vertex_scratch.push_back(top_left);
            ui_vertex_scratch.push_back(bottom_right);
            ui_vertex_scratch.push_back(bottom_left);
        }

        const auto* bytes = reinterpret_cast<const std::uint8_t*>(ui_vertex_scratch.data());
        const std::size_t total_bytes = ui_vertex_scratch.size() * sizeof(UiVertex);
        constexpr std::size_t kUploadBytes = 60U * 1024U;
        for (std::size_t offset = 0; offset < total_bytes; offset += kUploadBytes)
        {
            const std::size_t size = std::min(kUploadBytes, total_bytes - offset);
            frame_device.update_buffer(frame_device.self, ui_vertices, offset, bytes + offset,
                                       size);
        }

        barrier.buffer = ui_vertices;
        barrier.after_stages = RESONATE_RENDER_STAGE_VERTEX;
        barrier.after_access = RESONATE_RENDER_ACCESS_SHADER_READ;
        frame_device.barrier(frame_device.self, &barrier);
        ui_drawn_vertices = static_cast<std::uint32_t>(count * 6U);
    }
}

void RenderSystem::Impl::runClusters(GraphExecuteContext& commands, ResonateRenderPipeline pipeline)
{
    ResonateRenderDevice& device = *commands.device;
    if (device.bind_pipeline(device.self, pipeline) != RESONATE_OK)
    {
        return;
    }
    constants.model = Mat4{};
    constants.tint = Vec4{1.0F, 1.0F, 1.0F, 1.0F};
    constants.slots[0] = frame_data_slot;
    constants.slots[1] = commands.bufferSlot(boxes_resource);
    constants.slots[2] = commands.bufferSlot(grid_resource);
    constants.slots[3] = commands.bufferSlot(indices_resource);
    constants.slots2[0] = light_list_slot;
    device.set_constants(device.self, &constants, sizeof(constants));
    device.dispatch(device.self, kClustersX, kClustersY, kClustersZ);
}

void RenderSystem::Impl::runShadows(GraphExecuteContext& commands, std::uint32_t cascade)
{
    ResonateRenderDevice& device = *commands.device;

    ResonateRenderPassDesc pass{};
    pass.struct_size = sizeof(pass);
    pass.color = ResonateRenderImage{}; /* no color: depth-only */
    pass.colorless = 1U;
    pass.depth = commands.image(shadow_resource[cascade]);
    if (device.begin_pass(device.self, &pass) != RESONATE_OK)
    {
        return;
    }
    if (device.bind_pipeline(device.self, shadow_pipeline) != RESONATE_OK)
    {
        device.end_pass(device.self);
        return;
    }

    constants.slots[0] = frame_data_slot;
    constants.slots2[0] = light_list_slot;
    constants.slots2[1] = cascade;
    device.set_constants(device.self, &constants, sizeof(constants));

    for (const DrawInstance& instance : active->instances)
    {
        const GpuMesh& mesh = instance.mesh == kMeshGround ? ground : cube;
        if (mesh.vertices.value == 0U || mesh.indices.value == 0U)
        {
            continue;
        }
        constants.model = instance.model;
        device.set_constants(device.self, &constants, sizeof(constants));
        if (bound_mesh != instance.mesh)
        {
            device.bind_vertex_buffer(device.self, mesh.vertices, 0);
            device.bind_index_buffer(device.self, mesh.indices, 0);
            bound_mesh = instance.mesh;
        }
        device.draw_indexed(device.self, mesh.index_count, 1U);
    }
    bound_mesh = kMeshNone;

    device.end_pass(device.self);
}

void RenderSystem::Impl::drawScene(GraphExecuteContext& commands)
{
    ResonateRenderDevice& device = *commands.device;

    ResonateRenderPassDesc pass{};
    pass.struct_size = sizeof(pass);
    pass.color = commands.image(hdr_resource);
    pass.clear_color = kClearColor;
    pass.depth = commands.image(depth_resource);
    if (device.begin_pass(device.self, &pass) != RESONATE_OK)
    {
        return;
    }
    if (device.bind_pipeline(device.self, scene_pipeline) != RESONATE_OK)
    {
        device.end_pass(device.self);
        return;
    }

    constants.slots[0] = frame_data_slot;
    constants.slots[1] = commands.bufferSlot(boxes_resource);
    constants.slots[2] = commands.bufferSlot(grid_resource);
    constants.slots[3] = commands.bufferSlot(indices_resource);
    constants.slots2[0] = light_list_slot;
    for (std::uint32_t cascade = 0; cascade < kCascades; ++cascade)
    {
        constants.slots2[1 + cascade] = commands.imageSlot(shadow_resource[cascade]);
    }
    for (const DrawInstance& instance : active->instances)
    {
        const GpuMesh& mesh = instance.mesh == kMeshGround ? ground : cube;
        if (mesh.vertices.value == 0U || mesh.indices.value == 0U)
        {
            continue;
        }

        constants.model = instance.model;
        constants.tint = Vec4{instance.color.x, instance.color.y, instance.color.z, 1.0F};
        constants.texture_slot = instance.texture == 1U ? checker_texture_slot : white_texture_slot;
        constants.roughness = instance.roughness;
        constants.metallic = instance.metallic;
        device.set_constants(device.self, &constants, sizeof(constants));

        if (bound_mesh != instance.mesh)
        {
            device.bind_vertex_buffer(device.self, mesh.vertices, 0);
            device.bind_index_buffer(device.self, mesh.indices, 0);
            bound_mesh = instance.mesh;
        }
        device.draw_indexed(device.self, mesh.index_count, 1U);
    }
    bound_mesh = kMeshNone;

    device.end_pass(device.self);
}

void RenderSystem::Impl::drawUi(GraphExecuteContext& commands)
{
    ResonateRenderDevice& device = *commands.device;

    /* Clear even when the list is empty: the target is a reused transient and
       the composite samples it every frame. */
    ResonateRenderPassDesc pass{};
    pass.struct_size = sizeof(pass);
    pass.color = commands.image(ui_resource);
    pass.clear_color = ResonateVec4{0.0F, 0.0F, 0.0F, 0.0F};
    if (device.begin_pass(device.self, &pass) != RESONATE_OK)
    {
        return;
    }
    if (ui_drawn_vertices != 0U &&
        device.bind_pipeline(device.self, ui_pipeline) == RESONATE_OK)
    {
        for (const UiBatch& batch : ui_batches)
        {
            ui_constants.texture_slot = uiTextureSlot(batch.texture);
            device.set_constants(device.self, &ui_constants, sizeof(ui_constants));
            device.bind_vertex_buffer(device.self, ui_vertices,
                                      static_cast<std::uint64_t>(batch.first_vertex) *
                                          sizeof(UiVertex));
            device.draw(device.self, batch.vertex_count, 1U);
        }
    }
    device.end_pass(device.self);
}

void RenderSystem::Impl::runPost(GraphExecuteContext& commands, ResourceId target,
                                 ResonateRenderPipeline pipeline, std::uint32_t input_slot,
                                 std::uint32_t bloom_slot, std::uint32_t ui_slot,
                                 const Vec4& params)
{
    ResonateRenderDevice& device = *commands.device;

    ResonateRenderPassDesc pass{};
    pass.struct_size = sizeof(pass);
    pass.color = target == canvas_resource ? ResonateRenderImage{} : commands.image(target);
    pass.clear_color = kClearColor;
    if (device.begin_pass(device.self, &pass) != RESONATE_OK)
    {
        return;
    }
    if (device.bind_pipeline(device.self, pipeline) != RESONATE_OK)
    {
        device.end_pass(device.self);
        return;
    }
    post_constants.slots[0] = input_slot;
    post_constants.slots[1] = bloom_slot;
    post_constants.slots[2] = ui_slot;
    post_constants.slots[3] = 0U;
    post_constants.params = params;
    device.set_constants(device.self, &post_constants, sizeof(post_constants));
    device.bind_vertex_buffer(device.self, post_vertex_buffer, 0U);
    device.draw(device.self, 3U, 1U);
    device.end_pass(device.self);
}

void RenderSystem::Impl::clusterBuildEntry(void* context, GraphExecuteContext& commands)
{
    auto* self = static_cast<RenderSystem::Impl*>(context);
    self->runClusters(commands, self->build_pipeline);
}

void RenderSystem::Impl::clusterCullEntry(void* context, GraphExecuteContext& commands)
{
    auto* self = static_cast<RenderSystem::Impl*>(context);
    self->runClusters(commands, self->cull_pipeline);
}

void RenderSystem::Impl::shadow0Entry(void* context, GraphExecuteContext& commands)
{
    static_cast<RenderSystem::Impl*>(context)->runShadows(commands, 0U);
}

void RenderSystem::Impl::shadow1Entry(void* context, GraphExecuteContext& commands)
{
    static_cast<RenderSystem::Impl*>(context)->runShadows(commands, 1U);
}

void RenderSystem::Impl::shadow2Entry(void* context, GraphExecuteContext& commands)
{
    static_cast<RenderSystem::Impl*>(context)->runShadows(commands, 2U);
}

void RenderSystem::Impl::drawSceneEntry(void* context, GraphExecuteContext& commands)
{
    auto* self = static_cast<RenderSystem::Impl*>(context);
    self->drawScene(commands);
}

void RenderSystem::Impl::uiEntry(void* context, GraphExecuteContext& commands)
{
    static_cast<RenderSystem::Impl*>(context)->drawUi(commands);
}

void RenderSystem::Impl::prepareEntry(void* context, GraphExecuteContext& commands)
{
    static_cast<RenderSystem::Impl*>(context)->prepareFrame(commands);
}

void RenderSystem::Impl::brightEntry(void* context, GraphExecuteContext& commands)
{
    auto* self = static_cast<RenderSystem::Impl*>(context);
    self->runPost(commands, self->bright_resource, self->bright_pipeline,
                  commands.imageSlot(self->hdr_resource), 0U, 0U,
                  Vec4{self->features.bloom_threshold, 0.0F, 0.0F, 0.0F});
}

void RenderSystem::Impl::blurHEntry(void* context, GraphExecuteContext& commands)
{
    auto* self = static_cast<RenderSystem::Impl*>(context);
    self->runPost(commands, self->blur_a_resource, self->blur_pipeline,
                  commands.imageSlot(self->bright_resource), 0U, 0U,
                  Vec4{1.0F / static_cast<float>(self->bloom_width), 0.0F, 0.0F, 0.0F});
}

void RenderSystem::Impl::blurVEntry(void* context, GraphExecuteContext& commands)
{
    auto* self = static_cast<RenderSystem::Impl*>(context);
    self->runPost(commands, self->blur_b_resource, self->blur_pipeline,
                  commands.imageSlot(self->blur_a_resource), 0U, 0U,
                  Vec4{0.0F, 1.0F / static_cast<float>(self->bloom_height), 0.0F, 0.0F});
}

void RenderSystem::Impl::compositeEntry(void* context, GraphExecuteContext& commands)
{
    auto* self = static_cast<RenderSystem::Impl*>(context);
    const bool bloom = self->features.has("bloom");
    const ResourceId target =
        self->features.antialiases() ? self->ldr_resource : self->canvas_resource;
    self->runPost(commands, target, self->composite_pipeline,
                  commands.imageSlot(self->hdr_resource),
                  bloom ? commands.imageSlot(self->blur_b_resource) : 0U,
                  commands.imageSlot(self->ui_resource),
                  Vec4{0.0F, 0.0F, bloom ? 1.0F : 0.0F, 0.0F});
}

void RenderSystem::Impl::fxaaEntry(void* context, GraphExecuteContext& commands)
{
    auto* self = static_cast<RenderSystem::Impl*>(context);
    self->runPost(commands, self->canvas_resource, self->fxaa_pipeline,
                  commands.imageSlot(self->ldr_resource), 0U, 0U,
                  Vec4{1.0F / static_cast<float>(self->target_width),
                       1.0F / static_cast<float>(self->target_height), 0.0F, 0.0F});
}

RenderSystem::RenderSystem() : impl_(std::make_unique<Impl>())
{
}

RenderSystem::~RenderSystem() = default;

void RenderSystem::setRegistry(ResonateCapabilityRegistry* registry)
{
    impl_->registry = registry;
}

void RenderSystem::setFeatures(const FeatureStack& stack)
{
    impl_->features = stack;
}

bool RenderSystem::registerComponents(ecs::World& world)
{
    bool ok = world.registerComponent<render::Camera>() != ecs::kInvalidComponent;
    ok = world.registerComponent<render::MeshInstance>() != ecs::kInvalidComponent && ok;
    ok = world.registerComponent<render::Material>() != ecs::kInvalidComponent && ok;
    ok = world.registerComponent<render::DirectionalLight>() != ecs::kInvalidComponent && ok;
    ok = world.registerComponent<render::PointLight>() != ecs::kInvalidComponent && ok;
    return ok;
}

void RenderSystem::invoke(void* context, ecs::World& world, ecs::CommandBuffer&, float)
{
    static_cast<RenderSystem*>(context)->run(world);
}

void RenderSystem::release()
{
    Impl& impl = *impl_;
    impl.released = true;
    {
        std::lock_guard<std::mutex> lock(impl.handoff_mutex);
        impl.stop = true;
    }
    impl.handoff_wake.notify_all();
    if (impl.render_thread.joinable())
    {
        impl.render_thread.join();
    }
    /* The executor's destruction calls into the device; the render thread is
       gone and the capability is still alive. */
    impl.executor.reset();
}

void RenderSystem::waitIdle()
{
    Impl& impl = *impl_;
    std::unique_lock<std::mutex> lock(impl.handoff_mutex);
    impl.idle_wake.wait(lock, [&impl] { return !impl.snapshot_pending && !impl.consumer_busy; });
}

void RenderSystem::warmup()
{
    Impl& impl = *impl_;
    if (impl.released)
    {
        return;
    }
    impl.ensureThread();
    std::unique_lock<std::mutex> lock(impl.handoff_mutex);
    impl.idle_wake.wait(lock, [&impl] { return impl.warm || impl.stop; });
}

void RenderSystem::run(ecs::World& world)
{
    Impl& impl = *impl_;
    if (impl.released)
    {
        return;
    }
    impl.ensureThread();
    if (!impl.extract(world))
    {
        return;
    }
    impl.publish();
}

void RenderSystem::Impl::ensureThread()
{
    if (render_thread_started)
    {
        return;
    }
    /* The pointers are resolved before the thread starts and only read
       afterwards; the producer needs the canvas for the frame's aspect. */
    resolveCapabilities();
    render_thread = std::thread([this] { renderLoop(); });
    render_thread_started = true;
}

void RenderSystem::Impl::renderLoop()
{
    /* Warmup: attach, compile the graph and create every pipeline before the
       first snapshot, so the frame loop starts with a drawable frame. */
    if (device != nullptr && canvas != nullptr)
    {
        const uint32_t width = canvas->width(canvas->self);
        const uint32_t height = canvas->height(canvas->self);
        if (width != 0U && height != 0U)
        {
            prepareTargets(width, height);
        }
    }
    {
        std::lock_guard<std::mutex> lock(handoff_mutex);
        warm = true;
    }
    idle_wake.notify_all();

    for (;;)
    {
        FrameSnapshot* snapshot = nullptr;
        {
            std::unique_lock<std::mutex> lock(handoff_mutex);
            handoff_wake.wait(lock, [this] { return stop || snapshot_pending; });
            if (stop)
            {
                break;
            }
            snapshot = &snapshots[pending_snapshot];
            snapshot_pending = false;
            active_snapshot = pending_snapshot;
            consumer_busy = true;
            active = snapshot;
        }

        if (device != nullptr && canvas != nullptr)
        {
            renderFrame(*snapshot);
        }

        {
            std::lock_guard<std::mutex> lock(handoff_mutex);
            active = nullptr;
            consumer_busy = false;
            active_snapshot = kNoSnapshot;
            idle_wake.notify_all();
            handoff_wake.notify_all();
        }
    }
}

bool RenderSystem::Impl::prepareTargets(uint32_t width, uint32_t height)
{
    if (width == 0U || height == 0U)
    {
        return false;
    }
    if (!surface_attached && !surface_unavailable)
    {
        windowed = canvas->is_window_backed(canvas->self) != 0U;
    }
    if (!surface_unavailable)
    {
        if (windowed)
        {
            /* A platform can take its window away and hand back a new one
               (Android does on rotation); frames wait for the new target. */
            const void* native = canvas->native_handle(canvas->self);
            if (native == nullptr)
            {
                return false;
            }
            if (!surface_attached || native != attached_native)
            {
                const ResonateStatus attached =
                    device->attach_surface(device->self, native, width, height);
                if (attached != RESONATE_OK)
                {
                    surface_unavailable = true;
                    complain("the surface was not attached");
                    return false;
                }
                attached_native = native;
                surface_attached = true;
            }
        }
        else if (!surface_attached)
        {
            if (device->attach_offscreen(device->self, width, height) != RESONATE_OK)
            {
                surface_unavailable = true;
                complain("the offscreen target was not attached");
                return false;
            }
            surface_attached = true;
        }
    }
    if (!surface_attached)
    {
        return false;
    }

    /* The scene and post targets follow the canvas extent; a resize waits out
       the frames in flight, rebuilds the plan and its executor, and keeps the
       format-keyed pipelines. */
    if (!initialized || width != target_width || height != target_height)
    {
        device->wait_idle(device->self);
        executor.reset();
        /* An offscreen target is device-owned, so the renderer re-attaches it
           at the new extent; the first attach already used the current size.
           The swapchain resizes itself in begin_frame. */
        if (initialized && !windowed &&
            device->attach_offscreen(device->self, width, height) != RESONATE_OK)
        {
            surface_unavailable = true;
            complain("the offscreen target was not resized");
            return false;
        }
        target_width = width;
        target_height = height;
        bloom_width = std::max(1U, width / kBloomDivisor);
        bloom_height = std::max(1U, height / kBloomDivisor);
        if (!initialize())
        {
            initialized = false;
            return false;
        }
        initialized = true;
    }
    return ensurePipelines();
}

void RenderSystem::Impl::renderFrame(FrameSnapshot& snapshot)
{
    if (!prepareTargets(snapshot.target_width, snapshot.target_height))
    {
        return;
    }
    std::string error;
    if (!executor->execute(error, &Impl::prepareEntry, this) && !error.empty())
    {
        complain(error.c_str());
    }
    if (!albedo_ready)
    {
        albedo_ready = true;
        device->wait_idle(device->self);
        if (!createAlbedoTextures())
        {
            complain("albedo textures unavailable");
        }
    }
}

void composeDemoScene(ecs::World& world)
{
    const ecs::Entity camera = world.create();
    world.add(camera, hierarchy::LocalTransform{Vec3{0.0F, 2.5F, -7.0F}, resonate::Quat{},
                                                Vec3{1.0F, 1.0F, 1.0F}});
    world.add(camera, hierarchy::WorldTransform{Mat4{}});
    world.add(camera, render::Camera{});

    const ecs::Entity ground = world.create();
    world.add(ground, hierarchy::LocalTransform{Vec3{}, resonate::Quat{}, Vec3{1.0F, 1.0F, 1.0F}});
    world.add(ground, hierarchy::WorldTransform{Mat4{}});
    world.add(ground, render::MeshInstance{kMeshGround, Vec3{1.0F, 1.0F, 1.0F}});

    static const Vec3 kColors[3][3] = {
        {Vec3{0.90F, 0.45F, 0.35F}, Vec3{0.95F, 0.65F, 0.30F}, Vec3{0.55F, 0.80F, 0.40F}},
        {Vec3{0.35F, 0.75F, 0.70F}, Vec3{0.40F, 0.55F, 0.90F}, Vec3{0.60F, 0.45F, 0.90F}},
        {Vec3{0.85F, 0.40F, 0.70F}, Vec3{0.90F, 0.85F, 0.40F}, Vec3{0.45F, 0.85F, 0.85F}},
    };
    for (int x = 0; x < 3; ++x)
    {
        for (int z = 0; z < 3; ++z)
        {
            const ecs::Entity box = world.create();
            const Vec3 position{static_cast<float>(x - 1) * 2.5F, 0.5F,
                                static_cast<float>(z) * 2.5F};
            world.add(
                box, hierarchy::LocalTransform{position, resonate::Quat{}, Vec3{1.0F, 1.0F, 1.0F}});
            world.add(box, hierarchy::WorldTransform{Mat4{}});
            world.add(box, render::MeshInstance{kMeshCube, kColors[x][z]});
            world.add(box, render::Material{1U, Vec3{1.0F, 1.0F, 1.0F}});
        }
    }

    /* The cluster acceptance wants a hundred lights and change: a ceiling grid
       of warm and cool points over the field. */
    for (int x = 0; x < 12; ++x)
    {
        for (int z = 0; z < 12; ++z)
        {
            const ecs::Entity light = world.create();
            const Vec3 position{static_cast<float>(x - 5) * 2.2F, 2.2F,
                                static_cast<float>(z - 5) * 2.2F};
            const float mix = (x + z) % 2 == 0 ? 1.0F : 0.0F;
            const Vec3 color{0.30F + 0.65F * mix, 0.45F, 0.85F - 0.45F * mix};
            world.add(light, hierarchy::LocalTransform{position, resonate::Quat{},
                                                       Vec3{1.0F, 1.0F, 1.0F}});
            world.add(light, hierarchy::WorldTransform{Mat4{}});
            world.add(light, render::PointLight{color, 1.4F, 5.0F});
        }
    }

    const ecs::Entity sun = world.create();
    world.add(sun,
              render::DirectionalLight{Vec3{0.35F, -1.0F, 0.25F}, Vec3{1.0F, 0.96F, 0.88F}, 1.1F});
}

} // namespace resonate::renderer
