#include <resonate/renderer/renderer.h>

#include <resonate/renderer/texture.h>

#include <algorithm>
#include <array>
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
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <resonate/core/math.h>
#include <resonate/ecs/query.h>
#include <resonate/hierarchy/name.h>
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

#if defined(RESONATE_WITH_TGFX)
#    include <resonate/ui_tgfx.h>
#endif

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
    Vec4 light_count_and_pad;         /* x = light count, yz = render tile, w = proj y */
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
    /* The quad's extent, corner radius and border width. A solid quad rebuilds
       its rounded-rect coverage from these and reads its local position off the
       interpolated uv, which it samples no texture for. */
    float width = 0.0F;
    float height = 0.0F;
    float radius = 0.0F;
    float border = 0.0F;
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
    std::uint32_t font_slot = 0U;
    std::uint32_t overlay_slot = 0U;
    std::uint32_t pad1 = 0U;
};

/* A contiguous vertex range sharing one sampled texture. */
constexpr std::uint32_t kSolidUiTextureSlot = 0xFFFFFFFFU;
/* Not a ui::Quad texture id: the renderer appends one fullscreen quad with
   this id after the display list to composite the tgfx text layer. */
constexpr std::uint32_t kUiTextOverlayTexture = 3U;

struct UiBatch
{
    std::uint32_t first_vertex = 0U;
    std::uint32_t vertex_count = 0U;
    std::uint32_t texture = 0U;
};

/* A frame's overlay is bounded by one vertex buffer; a larger list is
   truncated rather than resized mid-frame. */
constexpr std::uint32_t kMaxUiQuads = 4096U;
static_assert(sizeof(UiVertex) == 48U);
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

/* The font atlas one UI canvas carries, shared by snapshots so publishing it
   is a pointer swap rather than a page copy. */
struct UiAtlas
{
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::uint32_t revision = 0U;
    std::vector<std::uint8_t> pixels;
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
    std::shared_ptr<const UiAtlas> text_atlas;
    std::uint32_t target_width = 0U;
    std::uint32_t target_height = 0U;
};

constexpr ResonateVec4 kClearColor{0.05F, 0.06F, 0.09F, 1.0F};

/* FNV-1a over a byte range: cheap enough to run every frame and exact enough to
   tell "the UI did not change" from "it did". */
std::uint64_t layerHash(std::uint64_t hash, const void* data, std::size_t size)
{
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    for (std::size_t index = 0U; index < size; ++index)
    {
        hash ^= bytes[index];
        hash *= 1099511628211ULL;
    }
    return hash;
}

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
                          ResonateRenderImage& out_texture, bool nearest = true)
{
    ResonateRenderImageDesc desc{};
    desc.struct_size = sizeof(desc);
    desc.format = format;
    desc.width = width;
    desc.height = height;
    desc.usage = RESONATE_RENDER_IMAGE_SAMPLED;
    desc.nearest = nearest ? 1U : 0U;
    if (device.create_image(device.self, &desc, &out_texture) != RESONATE_OK)
    {
        return false;
    }
    return device.upload_image(device.self, out_texture, pixels.data(), pixels.size(),
                               static_cast<std::uint64_t>(width) * 4U) == RESONATE_OK;
}

/* The frustum corners of one cascade slice in view space, mapped to world:
   the light's ortho box fits their bounds. `projection` is the camera's
   projection alone; its x/y scales unproject NDC at a given view depth. */
void cascadeBounds(const Mat4& projection, const Mat4& camera_world, float near_depth,
                   float far_depth, Vec3& out_min, Vec3& out_max)
{
    const float width_scale = projection.m[0][0];
    const float height_scale = projection.m[1][1];
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

/* RESONATE_FRAME_PROFILE: where a frame's CPU time goes, split by the thread
   that spends it. The producer writes from the game thread and the consumer
   from the render thread, so the counters are atomic. */
struct FrameProfile
{
    bool enabled = std::getenv("RESONATE_FRAME_PROFILE") != nullptr;
    std::atomic<double> extract_ms{0.0};
    std::atomic<double> publish_wait_ms{0.0};
    std::atomic<double> frame_ms{0.0};
    std::atomic<double> ui_ms{0.0};
    std::atomic<double> graph_ms{0.0};
};

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
    bool text_enabled = false;
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
    ResourceId ldr_aa_resource = 0;
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
    /* 3D and post targets are always allocated at the target extent; the scale
       only picks the sub-viewport the scene renders into and the UV window the
       tonemap and bloom read, so changing it never rebuilds the graph. The UI
       keeps its own full-resolution target, so text stays sharp. */
    bool overrides_read = false;
    /* The compat tier (no descriptor indexing) also shades like a mobile part:
       fewer uniform lights and a single full-range cascade. Set once the device
       is resolved. */
    bool compat_tier = false;
    /* Settings overrides: an explicit HDR format and shadow extent; "auto"
       and zero keep the tier's choice. */
    std::string hdr_format_name = "auto";
    std::string present_mode_name = "auto";
    std::uint32_t shadow_size = 0U;
    /* The scene pass's sample count; zero renders 1x (no MSAA). */
    std::uint32_t msaa = 0U;
    /* Diagnostic: when positive, force a graph rebuild every Nth frame, which
       retires every graph image and exercises the framebuffer cache. */
    std::uint32_t rebuild_stress = 0U;
    std::uint64_t rendered_frames = 0U;
    float render_scale = 1.0F;
    std::uint32_t render_width = 0U;
    std::uint32_t render_height = 0U;
    /* Adaptive scaling: the scale moves between these bounds to hold
       target_frame_ms, one step per sample window. */
    static constexpr float kMinRenderScale = 0.35F;
    float target_frame_ms = 0.0F;
    bool render_scale_dirty = false;
    /* The first frames carry pipeline warmup and the first present; letting them
       into the average would drive the scale straight to its floor. */
    unsigned warmup_frames = 30U;
    double sample_ms = 0.0;
    unsigned sample_frames = 0U;
    std::chrono::steady_clock::time_point sample_since = std::chrono::steady_clock::now();

    ResonateRenderPipeline scene_pipeline{};
    ResonateRenderPipeline shadow_pipeline{};
    ResonateRenderPipeline build_pipeline{};
    ResonateRenderPipeline cull_pipeline{};
    ResonateRenderPipeline bright_pipeline{};
    ResonateRenderPipeline blur_pipeline{};
    ResonateRenderPipeline composite_pipeline{};
    ResonateRenderPipeline fxaa_pipeline{};
    ResonateRenderPipeline blend_pipeline{};
    ResonateRenderPipeline ui_pipeline{};
    bool pipelines_ready = false;
    bool pipelines_failed = false;
    bool ready_noted = false;
    /* Set by whichever thread hits the first failure: the producer can refuse
       queries while the render thread reports a pipeline. */
    /* One report per distinct message; the render thread is the only caller. */
    std::unordered_set<std::string> complained;
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
    ResonateRenderImage font_atlas{};
    std::uint32_t font_atlas_width = 0U;
    std::uint32_t font_atlas_height = 0U;
    std::uint32_t white_texture_slot = 0U;
    std::uint32_t checker_texture_slot = 0U;
    std::uint32_t font_atlas_slot = 0U;
    std::uint32_t font_atlas_revision = 0U;
    bool albedo_ready = false;
    bool albedo_attempted = false;
    bool font_atlas_ready = false;
    /* True when this frame's text came from tgfx; the glyph quads are then
       skipped and the renderer appends the text overlay quad. */
    bool ui_tgfx_ready = false;

#if defined(RESONATE_WITH_TGFX)
    /* tgfx renders the whole UI layer through Resonate.UiTgfx; its pixels are
       uploaded to the sampled image the overlay quad reads. */
    std::vector<std::string> ui_font_paths;
    ResonateTgfxUi tgfx_ui = nullptr;
    ResonateRenderImage ui_text_image{};
    std::uint32_t ui_text_slot = 0U;
    std::uint32_t ui_text_width = 0U;
    std::uint32_t ui_text_height = 0U;
    /* The content hash of the layer as last rasterized, and whether it is. */
    std::uint64_t ui_layer_signature = 0U;
    bool ui_layer_drawn = false;
    FrameProfile profile;
    unsigned profile_frames = 0U;
    std::chrono::steady_clock::time_point profile_since = std::chrono::steady_clock::now();
    std::vector<std::uint8_t> ui_pixels;
    std::vector<ResonateTgfxRect> ui_draw_rects;
    std::vector<ResonateTgfxGlyph> ui_draw_glyphs;
    std::vector<ResonateTgfxText> ui_draw_texts;
    std::vector<ResonateTgfxImageDraw> ui_draw_images;
    /* True when the tgfx layer draws into the renderer's own image (zero
       copy); false falls back to the wrapper surface plus a readback. */
    bool ui_target_attached = false;

    /* Picture textures: the main thread registers id → file; the render thread
       registers the file with the tgfx layer, which decodes on its first draw.
       Quads naming a registered id draw the image; others stay solid rects. */
    struct UiImageRequest
    {
        std::uint32_t id = 0U;
        std::string path;
    };
    std::vector<UiImageRequest> ui_image_requests;
    std::unordered_map<std::uint32_t, std::string> ui_images;
#endif

    /* The producer's copy of the canvas's TextAtlasPage, replaced only when
       its revision moves. */
    std::shared_ptr<const UiAtlas> text_atlas_cache;
    std::uint32_t text_atlas_revision = 0U;

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
        if (complained.insert(what).second)
        {
            std::fprintf(stderr, "renderer: %s\n", what);
        }
    }

    void applyPresentMode();
    void readRenderScaleOverrides();
    ResonateRenderFormat hdrFormat() const;
    std::uint32_t shadowExtent() const;
    bool initialize();
    bool createAlbedoTextures(const FrameSnapshot& snapshot);
    bool ensurePipelines();
    ResonateRenderBuffer uploadStorage(std::uint64_t size_bytes, bool host_visible);
    ResonateRenderBuffer uploadUniform(std::uint64_t size_bytes);
    std::uint32_t slotOf(ResonateRenderBuffer handle) const;
    std::uint32_t slotOf(ResonateRenderImage handle) const;
    std::uint32_t uiTextureSlot(std::uint32_t texture) const;
    bool updateFontAtlas(const FrameSnapshot& snapshot);
#if defined(RESONATE_WITH_TGFX)
    bool renderTgfxUi(FrameSnapshot& snapshot);
    void releaseTgfx();
    void processUiImages();
#endif
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
    static void blendEntry(void* context, GraphExecuteContext& commands);
};

/* The scale and the frame budget are read once: `initialize` runs again on every
   rebuild, and re-reading them would undo the controller's own adjustments and
   rebuild forever. */
void RenderSystem::Impl::readRenderScaleOverrides()
{
    if (overrides_read)
    {
        return;
    }
    overrides_read = true;
    if (const char* scale = std::getenv("RESONATE_RENDER_SCALE"))
    {
        render_scale = std::clamp(static_cast<float>(std::atof(scale)), kMinRenderScale, 1.0F);
    }
    if (const char* budget = std::getenv("RESONATE_TARGET_FRAME_MS"))
    {
        target_frame_ms = std::fmax(0.0F, static_cast<float>(std::atof(budget)));
    }
    /* Same environment the tests use; Android reaches it through the host's
       property mapping, where no settings file travels with the package. */
    if (const char* msaa_override = std::getenv("RESONATE_MSAA"))
    {
        msaa = static_cast<std::uint32_t>(std::max(0, std::atoi(msaa_override)));
    }
    if (const char* stress = std::getenv("RESONATE_REBUILD_STRESS"))
    {
        rebuild_stress = static_cast<std::uint32_t>(std::max(0, std::atoi(stress)));
    }
}

ResonateRenderFormat RenderSystem::Impl::hdrFormat() const
{
    if (hdr_format_name == "rgba16f")
    {
        return RESONATE_RENDER_FORMAT_R16G16B16A16_SFLOAT;
    }
    if (hdr_format_name == "b10g11r11")
    {
        return RESONATE_RENDER_FORMAT_B10G11R11_UFLOAT;
    }
    return compat_tier ? RESONATE_RENDER_FORMAT_B10G11R11_UFLOAT
                       : RESONATE_RENDER_FORMAT_R16G16B16A16_SFLOAT;
}

std::uint32_t RenderSystem::Impl::shadowExtent() const
{
    return shadow_size != 0U ? shadow_size : (compat_tier ? 1024U : 2048U);
}

bool RenderSystem::Impl::initialize()
{
    /* The graph is built from the scale, and the warmup builds it before the
       first frame: the overrides are read here, once. */
    readRenderScaleOverrides();
    compat_tier =
        device->header.struct_size >=
            offsetof(ResonateRenderDevice, feature_bits) + sizeof(device->feature_bits) &&
        device->feature_bits != nullptr &&
        (device->feature_bits(device->self) & RESONATE_RENDER_FEATURE_DESCRIPTOR_INDEXING) == 0U;

    /* The fixed spine: three CSM shadow passes, two cluster compute passes and
       the lit scene into HDR. The post chain after it comes from the feature
       stack: bloom adds its extraction and blur passes, the tonemap writes the
       LDR scene, and FXAA — when enabled — antialiases that scene before any
       UI exists. The blend pass composites the UI over it into the canvas. */
    const bool bloom = features.has("bloom");
    const bool fxaa = features.antialiases();

    resonate::render::RenderGraph graph;
    canvas_resource = graph.importImage("backbuffer", kLayoutUndefined, kLayoutPresent);
    /* The tier picks the scene formats: packed HDR and 16-bit depth halve the
       bandwidth of the full-size targets on a mobile GPU; the settings may
       name the HDR format explicitly. */
    const ResonateRenderFormat hdr_format = hdrFormat();
    const ResonateRenderFormat depth_format =
        compat_tier ? RESONATE_RENDER_FORMAT_D16_UNORM : RESONATE_RENDER_FORMAT_D32_SFLOAT;
    depth_resource = graph.declareImage("depth", depth_format, target_width, target_height,
                                        RESONATE_RENDER_IMAGE_DEPTH_ATTACHMENT);
    hdr_resource = graph.declareImage(
        "hdr", hdr_format, target_width, target_height,
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
    ldr_aa_resource = resonate::render::kInvalidResource;
    bright_resource = resonate::render::kInvalidResource;
    blur_a_resource = resonate::render::kInvalidResource;
    blur_b_resource = resonate::render::kInvalidResource;
    /* The LDR scene exists in both modes: the tonemap always writes it, and the
       blend pass reads it back when it composites the UI. */
    ldr_resource = graph.declareImage(
        "ldr", RESONATE_RENDER_FORMAT_R8G8B8A8_UNORM, target_width, target_height,
        RESONATE_RENDER_IMAGE_COLOR_ATTACHMENT | RESONATE_RENDER_IMAGE_SAMPLED);
    if (fxaa)
    {
        /* A second LDR target because FXAA cannot read and write one image. */
        ldr_aa_resource = graph.declareImage(
            "ldr_aa", RESONATE_RENDER_FORMAT_R8G8B8A8_UNORM, target_width, target_height,
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
    const std::uint32_t shadow_count = compat_tier ? 1U : kCascades;
    for (std::uint32_t cascade = 0; cascade < shadow_count; ++cascade)
    {
        /* Mobile tier: half the map size — a 4-tap PCF at 2048 over a soft
           cascade is indistinguishable here, and the clear/store bandwidth
           halves. */
        shadow_resource[cascade] = graph.declareImage(
            "shadow", RESONATE_RENDER_FORMAT_D32_SFLOAT,
            shadowExtent(), shadowExtent(),
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
    const std::uint32_t cascade_count = compat_tier ? 1U : kCascades;
    for (std::uint32_t cascade = 0; cascade < cascade_count; ++cascade)
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
    /* The compat tier shades from uniform lights and never reads the cluster
       grid: building it there is wasted GPU time. */
    if (!compat_tier)
    {
        resonate::render::PassDesc cluster_build{};
        cluster_build.name = "cluster-build";
    cluster_build.queue = resonate::render::Queue::Compute;
    cluster_build.writes = &boxes_write;
    cluster_build.write_count = 1U;
    cluster_build.context = this;
        cluster_build.execute = &Impl::clusterBuildEntry;
        graph.addPass(cluster_build);
    }

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
    if (!compat_tier)
    {
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
    }


    Use scene_reads[5]{};
    scene_reads[0].resource = grid_resource;
    scene_reads[0].stages = resonate::render::kStageFragment;
    scene_reads[0].access = resonate::render::kAccessShaderRead;
    scene_reads[0].layout = kLayoutShaderReadOnly;
    scene_reads[1].resource = indices_resource;
    scene_reads[1].stages = resonate::render::kStageFragment;
    scene_reads[1].access = resonate::render::kAccessShaderRead;
    scene_reads[1].layout = kLayoutShaderReadOnly;
    for (std::uint32_t cascade = 0; cascade < cascade_count; ++cascade)
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
    scene.read_count = 2U + cascade_count;
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

    /* The tonemap pass writes the LDR scene; bloom's output feeds it when the
       feature exists. No UI here: antialiasing must not touch the UI layer. */
    Use composite_reads[2]{};
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
    Use composite_write{};
    composite_write.resource = ldr_resource;
    composite_write.stages = resonate::render::kStageColorOutput;
    composite_write.access = resonate::render::kAccessColorWrite;
    composite_write.layout = kLayoutColorAttachment;
    resonate::render::PassDesc composite{};
    composite.name = "tonemap";
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
        Use ldr_aa_write{};
        ldr_aa_write.resource = ldr_aa_resource;
        ldr_aa_write.stages = resonate::render::kStageColorOutput;
        ldr_aa_write.access = resonate::render::kAccessColorWrite;
        ldr_aa_write.layout = kLayoutColorAttachment;
        resonate::render::PassDesc fxaa_pass{};
        fxaa_pass.name = "fxaa";
        fxaa_pass.reads = &ldr_read;
        fxaa_pass.read_count = 1U;
        fxaa_pass.writes = &ldr_aa_write;
        fxaa_pass.write_count = 1U;
        fxaa_pass.context = this;
        fxaa_pass.execute = &Impl::fxaaEntry;
        graph.addPass(fxaa_pass);
    }

    /* The overlay pass comes after the scene's post chain: it clears its own
       target and every quad blends into it in paint order. */
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

    Use blend_reads[2]{};
    blend_reads[0].resource = fxaa ? ldr_aa_resource : ldr_resource;
    blend_reads[0].stages = resonate::render::kStageFragment;
    blend_reads[0].access = resonate::render::kAccessShaderRead;
    blend_reads[0].layout = kLayoutShaderReadOnly;
    blend_reads[1].resource = ui_resource;
    blend_reads[1].stages = resonate::render::kStageFragment;
    blend_reads[1].access = resonate::render::kAccessShaderRead;
    blend_reads[1].layout = kLayoutShaderReadOnly;
    Use blend_write{};
    blend_write.resource = canvas_resource;
    blend_write.stages = resonate::render::kStageColorOutput;
    blend_write.access = resonate::render::kAccessColorWrite;
    blend_write.layout = kLayoutColorAttachment;
    resonate::render::PassDesc blend{};
    blend.name = "blend";
    blend.reads = blend_reads;
    blend.read_count = 2U;
    blend.writes = &blend_write;
    blend.write_count = 1U;
    blend.context = this;
    blend.execute = &Impl::blendEntry;
    graph.addPass(blend);
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
#if defined(RESONATE_WITH_TGFX)
    if (texture == kUiTextOverlayTexture)
    {
        return ui_tgfx_ready ? ui_text_slot : kSolidUiTextureSlot;
    }
#endif
    if (texture == ui::kTextureWhite)
    {
        return kSolidUiTextureSlot;
    }
    if (texture == ui::kTextureFont)
    {
        return font_atlas_ready ? font_atlas_slot : kSolidUiTextureSlot;
    }
    if (!albedo_ready)
    {
        return kSolidUiTextureSlot;
    }
    return texture == ui::kTextureChecker ? checker_texture_slot : white_texture_slot;
}

bool RenderSystem::Impl::updateFontAtlas(const FrameSnapshot& snapshot)
{
    if (device == nullptr || snapshot.text_atlas == nullptr)
    {
        return true;
    }
    const UiAtlas& atlas = *snapshot.text_atlas;
    if (font_atlas_ready && atlas.revision == font_atlas_revision)
    {
        return true;
    }
    if (atlas.width == 0U || atlas.height == 0U ||
        atlas.pixels.size() != static_cast<std::size_t>(atlas.width) * atlas.height * 4U)
    {
        complain("the font atlas page is malformed");
        return false;
    }
    if (font_atlas.value != 0U && font_atlas_width == atlas.width &&
        font_atlas_height == atlas.height)
    {
        /* The page grew in place: overwrite the image and keep the slot. */
        if (device->upload_image(device->self, font_atlas, atlas.pixels.data(), atlas.pixels.size(),
                                 static_cast<std::uint64_t>(atlas.width) * 4U) != RESONATE_OK)
        {
            complain("the font atlas texture was not updated");
            return false;
        }
        font_atlas_revision = atlas.revision;
        return true;
    }
    device->wait_idle(device->self);
    if (font_atlas.value != 0U)
    {
        device->destroy_image(device->self, font_atlas);
        font_atlas = ResonateRenderImage{};
    }
    font_atlas_ready = false;
    if (!createSampledTexture(*device, atlas.width, atlas.height,
                              RESONATE_RENDER_FORMAT_R8G8B8A8_UNORM, atlas.pixels, font_atlas,
                              true))
    {
        complain("the font atlas texture was not created");
        return false;
    }
    font_atlas_slot = slotOf(font_atlas);
    if (device->header.struct_size >=
            offsetof(ResonateRenderDevice, bind_image_slot) +
                sizeof(device->bind_image_slot) &&
        device->bind_image_slot != nullptr)
    {
        device->bind_image_slot(device->self, font_atlas_slot, font_atlas, 1U);
    }
    font_atlas_width = atlas.width;
    font_atlas_height = atlas.height;
    font_atlas_revision = atlas.revision;
    font_atlas_ready = true;
    return true;
}

#if defined(RESONATE_WITH_TGFX)
bool RenderSystem::Impl::renderTgfxUi(FrameSnapshot& snapshot)
{
    ui_tgfx_ready = false;
    const std::uint32_t width = snapshot.target_width;
    const std::uint32_t height = snapshot.target_height;
    if (device == nullptr || width == 0U || height == 0U || !albedo_ready)
    {
        /* albedo_ready flips after the first frame executed: a target created
           before the first present samples as a constant on the Mali-G51
           (see createAlbedoTextures), so frame one draws the atlas fallback. */
        return false;
    }
    if (tgfx_ui == nullptr)
    {
        /* The vertex path is the portable rasterizer, and a host bisecting a UI
           bug needs to see it without rebuilding. */
        if (std::getenv("RESONATE_UI_NO_TGFX") != nullptr)
        {
            return false;
        }
        if (device->native_handles == nullptr || ui_font_paths.empty())
        {
            return false;
        }
        ResonateRenderNativeHandles handles = {};
        if (device->native_handles(device->self, &handles) != RESONATE_OK || handles.device == 0U)
        {
            complain("tgfx: the device does not share its native handles");
            return false;
        }
        std::vector<const char*> font_paths;
        font_paths.reserve(ui_font_paths.size());
        for (const std::string& path : ui_font_paths)
        {
            font_paths.push_back(path.c_str());
        }
        if (resonate_tgfx_ui_create(reinterpret_cast<void*>(handles.instance),
                                    reinterpret_cast<void*>(handles.physical_device),
                                    reinterpret_cast<void*>(handles.device),
                                    reinterpret_cast<void*>(handles.queue), handles.queue_family,
                                    font_paths.data(),
                                    static_cast<std::uint32_t>(font_paths.size()),
                                    &tgfx_ui) != 0)
        {
            complain("tgfx: the UI layer was not created");
            tgfx_ui = nullptr;
            return false;
        }
    }
    /* Pending Picture registrations decode here: the tgfx layer must exist
       first, and the contains-check makes an empty queue cheap. */
    processUiImages();

    ui_draw_rects.clear();
    ui_draw_images.clear();
    for (const ui::Quad& quad : snapshot.ui.quads())
    {
        if (quad.width <= 0.0F || quad.height <= 0.0F || quad.texture == ui::kTextureFont)
        {
            continue;
        }
        if (quad.texture == ui::kTextureChecker)
        {
            const float half_width = quad.width * 0.5F;
            const float half_height = quad.height * 0.5F;
            for (int row = 0; row < 2; ++row)
            {
                for (int column = 0; column < 2; ++column)
                {
                    const float shade = (row + column) % 2 == 0 ? 0.82F : 0.45F;
                    ui_draw_rects.push_back(ResonateTgfxRect{
                        quad.x + static_cast<float>(column) * half_width,
                        quad.y + static_cast<float>(row) * half_height, half_width, half_height,
                        0.0F, 0.0F, quad.color.x * shade, quad.color.y * shade,
                        quad.color.z * shade, quad.color.w, quad.clip.x, quad.clip.y,
                        quad.clip.width, quad.clip.height});
                }
            }
            continue;
        }
        if (ui_images.contains(quad.texture))
        {
            ui_draw_images.push_back(ResonateTgfxImageDraw{
                quad.texture, quad.x, quad.y, quad.width, quad.height, quad.uv_min.x,
                quad.uv_min.y, quad.uv_max.x, quad.uv_max.y, quad.color.x, quad.color.y,
                quad.color.z, quad.color.w, quad.clip.x, quad.clip.y, quad.clip.width,
                quad.clip.height});
            continue;
        }
        ui_draw_rects.push_back(ResonateTgfxRect{quad.x, quad.y, quad.width, quad.height,
                                                 quad.radius, quad.border, quad.color.x,
                                                 quad.color.y, quad.color.z, quad.color.w,
                                                 quad.clip.x, quad.clip.y, quad.clip.width,
                                                 quad.clip.height});
    }

    ui_draw_glyphs.clear();
    ui_draw_texts.clear();
    for (const ui::TextRun& run : snapshot.ui.texts())
    {
        if (run.glyphs.empty())
        {
            continue;
        }
        const std::uint32_t base = static_cast<std::uint32_t>(ui_draw_glyphs.size());
        for (const ui::TextGlyph& glyph : run.glyphs)
        {
            ui_draw_glyphs.push_back(ResonateTgfxGlyph{glyph.pen_x, glyph.pen_y, glyph.glyph_id,
                                                       glyph.font_index});
        }
        ResonateTgfxText text{};
        text.size = run.font_size;
        text.r = run.color.x;
        text.g = run.color.y;
        text.b = run.color.z;
        text.a = run.color.w;
        text.glyphs = ui_draw_glyphs.data() + base;
        text.glyph_count = static_cast<std::uint32_t>(run.glyphs.size());
        text.clip_x = run.clip.x;
        text.clip_y = run.clip.y;
        text.clip_width = run.clip.width;
        text.clip_height = run.clip.height;
        ui_draw_texts.push_back(text);
    }

    /* The UI target the layer draws into: the renderer's own color-attachment
       image, so the blend pass samples it without a readback. A failed attach
       falls back to the wrapper-owned surface and the readback upload. */
    if (ui_text_image.value == 0U || width != ui_text_width || height != ui_text_height)
    {
        if (ui_text_image.value != 0U)
        {
            device->destroy_image(device->self, ui_text_image);
            ui_text_image = ResonateRenderImage{};
        }
        ResonateRenderImageDesc desc = {};
        desc.struct_size = sizeof(desc);
        desc.format = RESONATE_RENDER_FORMAT_R8G8B8A8_UNORM;
        desc.width = width;
        desc.height = height;
        desc.usage = RESONATE_RENDER_IMAGE_COLOR_ATTACHMENT | RESONATE_RENDER_IMAGE_SAMPLED;
        if (device->create_image(device->self, &desc, &ui_text_image) != RESONATE_OK)
        {
            return false;
        }
        ui_text_slot = slotOf(ui_text_image);
        if (device->bind_image_slot != nullptr)
        {
            device->bind_image_slot(device->self, ui_text_slot, ui_text_image, 0U);
        }
        ui_text_width = width;
        ui_text_height = height;
        ui_pixels.resize(static_cast<std::size_t>(width) * height * 4U);
        ui_target_attached = false;
        ui_layer_drawn = false;
    }
    /* The compat tier (the Mali-G51 class) keeps the attach path: the readback
       bridge wedged the render thread there (the driver's readPixels and our
       own upload waits interleave badly), and the attach path renders once
       the layout handshake is symmetric. */
    if (!ui_target_attached && device->native_image != nullptr)
    {
        ResonateRenderNativeImage native = {};
        if (device->native_image(device->self, ui_text_image, &native) == RESONATE_OK &&
            native.image != 0U)
        {
            const ResonateTgfxNativeImage target{native.image, native.format, native.layout,
                                                 native.width, native.height};
            ui_target_attached = resonate_tgfx_ui_attach(tgfx_ui, &target) == 0;
            if (!ui_target_attached)
            {
                complain("tgfx: the UI target was not attached; drawing into a readback surface");
            }
        }
    }

    /* The layer is only re-rasterized when its content or extent changed; a
       static HUD costs one hash per frame instead of a full redraw. */
    std::uint64_t signature = 1469598103934665603ULL;
    signature = layerHash(signature, &width, sizeof(width));
    signature = layerHash(signature, &height, sizeof(height));
    signature = layerHash(signature, ui_draw_rects.data(),
                          ui_draw_rects.size() * sizeof(ResonateTgfxRect));
    signature = layerHash(signature, ui_draw_images.data(),
                          ui_draw_images.size() * sizeof(ResonateTgfxImageDraw));
    for (const ResonateTgfxText& text : ui_draw_texts)
    {
        signature = layerHash(signature, &text.size, sizeof(text.size));
        signature = layerHash(signature, &text.r, sizeof(text.r));
        signature = layerHash(signature, &text.g, sizeof(text.g));
        signature = layerHash(signature, &text.b, sizeof(text.b));
        signature = layerHash(signature, &text.a, sizeof(text.a));
        if (text.glyphs != nullptr)
        {
            signature = layerHash(signature, text.glyphs,
                                  text.glyph_count * sizeof(ResonateTgfxGlyph));
        }
    }
    if (std::getenv("RESONATE_FRAME_TRACE") != nullptr && signature != ui_layer_signature)
    {
        std::size_t glyphs = 0U;
        for (const ResonateTgfxText& text : ui_draw_texts)
        {
            glyphs += text.glyph_count;
        }
        std::fprintf(stderr, "renderer: ui layer changed: %u rects, %u texts (%zu glyphs), %u images\n",
                     static_cast<unsigned>(ui_draw_rects.size()),
                     static_cast<unsigned>(ui_draw_texts.size()), glyphs,
                     static_cast<unsigned>(ui_draw_images.size()));
    }
    static const bool always_redraw = std::getenv("RESONATE_UI_REDRAW_ALWAYS") != nullptr;
    if (!always_redraw && ui_layer_drawn && signature == ui_layer_signature)
    {
        ui_tgfx_ready = true;
        return true;
    }

    /* The draw always runs: it clears the target even on an empty layer, so
       the blend pass never resamples stale content. */
    if (resonate_tgfx_ui_draw(tgfx_ui, width, height, ui_draw_rects.data(),
                              static_cast<std::uint32_t>(ui_draw_rects.size()),
                              ui_draw_texts.data(),
                              static_cast<std::uint32_t>(ui_draw_texts.size()),
                              ui_draw_images.data(),
                              static_cast<std::uint32_t>(ui_draw_images.size())) != 0)
    {
        complain("tgfx: the UI draw failed");
        return false;
    }
    if (!ui_target_attached)
    {
        /* The wrapper drew into its own surface; upload the pixels through the
           RHI, whose transitions own the layout deterministically. */
        if (resonate_tgfx_ui_read_pixels(tgfx_ui, ui_pixels.data(), ui_pixels.size()) != 0 ||
            device->upload_image(device->self, ui_text_image, ui_pixels.data(), ui_pixels.size(),
                                 static_cast<std::uint64_t>(width) * 4U) != RESONATE_OK)
        {
            return false;
        }
    }

    if (const char* dump = std::getenv("RESONATE_DUMP_UI_TARGET"))
    {
        std::fprintf(stderr,
                     "renderer: ui layer %u rects, %u texts, %u images, %u registered\n",
                     static_cast<unsigned>(ui_draw_rects.size()),
                     static_cast<unsigned>(ui_draw_texts.size()),
                     static_cast<unsigned>(ui_draw_images.size()),
                     static_cast<unsigned>(ui_images.size()));
        /* Verification: read the target back through tgfx (its own transition)
           and write the first frames out as BMPs. */
        static int dumped = 0;
        if (dumped < 4)
        {
            char path[512];
            std::snprintf(path, sizeof(path), "%s.%d.bmp", dump, dumped);
            ++dumped;
            if (resonate_tgfx_ui_read_pixels(tgfx_ui, ui_pixels.data(), ui_pixels.size()) == 0)
            {
                std::FILE* file = std::fopen(path, "wb");
                if (file != nullptr)
                {
                    const std::uint32_t stride = (width * 3U + 3U) & ~3U;
                    const std::uint32_t image_size = stride * height;
                    unsigned char header[54] = {'B', 'M', 0, 0, 0, 0, 0, 0, 0, 0,
                                                54,  0,   0, 0, 40, 0,  0, 0};
                    const auto put32 = [&header](std::size_t at, std::uint32_t value)
                    {
                        header[at] = static_cast<unsigned char>(value & 0xFFU);
                        header[at + 1U] = static_cast<unsigned char>((value >> 8U) & 0xFFU);
                        header[at + 2U] = static_cast<unsigned char>((value >> 16U) & 0xFFU);
                        header[at + 3U] = static_cast<unsigned char>((value >> 24U) & 0xFFU);
                    };
                    put32(2U, 54U + image_size);
                    put32(18U, width);
                    put32(22U, height);
                    header[26] = 1U; /* biPlanes */
                    header[28] = 24U; /* biBitCount */
                    put32(34U, image_size);
                    std::fwrite(header, 1U, sizeof(header), file);
                    std::vector<unsigned char> row(stride, 0U);
                    for (std::uint32_t y = height; y-- > 0U;)
                    {
                        for (std::uint32_t x = 0; x < width; ++x)
                        {
                            const std::size_t source =
                                (static_cast<std::size_t>(y) * width + x) * 4U;
                            row[x * 3U] = ui_pixels[source + 2U];
                            row[x * 3U + 1U] = ui_pixels[source + 1U];
                            row[x * 3U + 2U] = ui_pixels[source];
                        }
                        std::fwrite(row.data(), 1U, stride, file);
                    }
                    std::fclose(file);
                }
            }
        }
    }
    ui_layer_signature = signature;
    ui_layer_drawn = true;
    ui_tgfx_ready = true;
    return true;
}

void RenderSystem::Impl::processUiImages()
{
    for (const UiImageRequest& request : ui_image_requests)
    {
        if (resonate_tgfx_image_register(tgfx_ui, request.id, request.path.c_str()) != 0)
        {
            complain("tgfx: a UI image was not registered");
            continue;
        }
        ui_images.emplace(request.id, request.path);
    }
    ui_image_requests.clear();
}

void RenderSystem::Impl::releaseTgfx()
{
    resonate_tgfx_ui_destroy(tgfx_ui);
    tgfx_ui = nullptr;
    ui_images.clear();
    ui_image_requests.clear();
    if (ui_text_image.value != 0U && device != nullptr)
    {
        device->destroy_image(device->self, ui_text_image);
    }
    ui_text_image = ResonateRenderImage{};
    ui_text_width = 0U;
    ui_text_height = 0U;
    ui_text_slot = 0U;
    ui_tgfx_ready = false;
}
#endif

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
    /* The bindless fragment lives in its own file: slangc carries every
       function of a source into each module, and the NonUniform decoration it
       holds would make a no-descriptor-indexing driver refuse the pipeline. */
    const std::vector<char> fragment_spirv = readFile(
        std::filesystem::path(directory) /
        (compat_shadows ? "scene.sceneFragmentCompat.spv" : "scene_lit.sceneFragment.spv"));
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
        readFile(std::filesystem::path(directory) / "post.tonemapFragment.spv");
    const std::vector<char> fxaa_spirv =
        readFile(std::filesystem::path(directory) / "post.fxaaFragment.spv");
    const std::vector<char> blend_spirv =
        readFile(std::filesystem::path(directory) / "post.blendFragment.spv");
    const std::vector<char> ui_vertex_spirv =
        readFile(std::filesystem::path(directory) / "ui.uiVertex.spv");
    const std::vector<char> ui_fragment_spirv =
        readFile(std::filesystem::path(directory) / "ui.uiFragment.spv");
    if (vertex_spirv.empty() || fragment_spirv.empty() || shadow_vertex_spirv.empty() ||
        shadow_fragment_spirv.empty() || build_spirv.empty() || cull_spirv.empty() ||
        post_vertex_spirv.empty() || bright_spirv.empty() || blur_spirv.empty() ||
        composite_spirv.empty() || fxaa_spirv.empty() || blend_spirv.empty() ||
        ui_vertex_spirv.empty() || ui_fragment_spirv.empty())
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
    ResonateRenderShader blend{};
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
        device->create_shader(device->self, RESONATE_RENDER_SHADER_FRAGMENT, blend_spirv.data(),
                              blend_spirv.size(), &blend) != RESONATE_OK ||
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
    /* The formats must match the graph's declarations: the compat tier renders
       the scene into a packed-HDR target with a 16-bit depth. */
    const ResonateRenderFormat hdr_format = hdrFormat();
    const ResonateRenderFormat depth_format =
        compat_tier ? RESONATE_RENDER_FORMAT_D16_UNORM : RESONATE_RENDER_FORMAT_D32_SFLOAT;
    scene_desc.color_format = hdr_format;
    scene_desc.depth_format = depth_format;
    /* The scene pipeline rasterizes at the pass's sample count; a samples>1
       pipeline is only valid inside a samples>1 pass. */
    scene_desc.samples = msaa;
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

    /* The tonemap and FXAA passes write LDR targets; only the final blend
       owns the canvas, so its pipeline names the swapchain format. */
    const ResonateRenderFormat post_targets[5] = {hdr_format,
                                                  hdr_format,
                                                  RESONATE_RENDER_FORMAT_R8G8B8A8_UNORM,
                                                  RESONATE_RENDER_FORMAT_R8G8B8A8_UNORM,
                                                  RESONATE_RENDER_FORMAT_UNKNOWN};
    ResonateRenderShader* post_fragments[5] = {&bright, &blur, &composite, &fxaa, &blend};
    ResonateRenderPipeline* post_pipelines[5] = {&bright_pipeline, &blur_pipeline,
                                                 &composite_pipeline, &fxaa_pipeline,
                                                 &blend_pipeline};

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

    for (std::uint32_t i = 0; i < 5U; ++i)
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

    /* The overlay: target-pixel positions, straight-alpha colors, sampled UVs
       and the rounded-rect shape, blended into the UI image. */
    ResonateRenderVertexAttribute ui_attributes[4] = {};
    ui_attributes[0].location = 0U;
    ui_attributes[0].format = RESONATE_RENDER_FORMAT_R32G32_SFLOAT;
    ui_attributes[0].offset_bytes = 0U;
    ui_attributes[1].location = 1U;
    ui_attributes[1].format = RESONATE_RENDER_FORMAT_R32G32B32A32_SFLOAT;
    ui_attributes[1].offset_bytes = 8U;
    ui_attributes[2].location = 2U;
    ui_attributes[2].format = RESONATE_RENDER_FORMAT_R32G32_SFLOAT;
    ui_attributes[2].offset_bytes = 24U;
    ui_attributes[3].location = 3U;
    ui_attributes[3].format = RESONATE_RENDER_FORMAT_R32G32B32A32_SFLOAT;
    ui_attributes[3].offset_bytes = 32U;
    ResonateRenderVertexStream ui_stream{};
    ui_stream.stride_bytes = sizeof(UiVertex);
    ui_stream.attribute_count = 4U;
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
        /* The scene pass's sample count belongs here: the setting may ask for a
           count this device has to answer for, and the run is otherwise silent
           about it. */
        std::fprintf(stderr, "renderer: pipelines ready, scene msaa %u\n",
                     static_cast<unsigned>(msaa));
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
bool RenderSystem::Impl::createAlbedoTextures(const FrameSnapshot& snapshot)
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
    /* The atlas is rebuilt here for the same reason as the albedo images: the
       copy updateFontAtlas made before the first present is the one this
       driver ignores. */
    if (snapshot.text_atlas != nullptr && snapshot.text_atlas->revision != 0U)
    {
        const UiAtlas& atlas = *snapshot.text_atlas;
        if (atlas.width == 0U || atlas.height == 0U ||
            atlas.pixels.size() != static_cast<std::size_t>(atlas.width) * atlas.height * 4U)
        {
            complain("the font atlas page is malformed");
            return false;
        }
        /* The pre-first-present copy updateFontAtlas made is replaced, not
           leaked; the frame is idle here. */
        if (font_atlas.value != 0U)
        {
            device->destroy_image(device->self, font_atlas);
            font_atlas = ResonateRenderImage{};
        }
        font_atlas_ready = false;
        if (!createSampledTexture(*device, atlas.width, atlas.height,
                                  RESONATE_RENDER_FORMAT_R8G8B8A8_UNORM, atlas.pixels, font_atlas,
                                  true))
        {
            complain("the font atlas texture was not created");
            return false;
        }
        font_atlas_slot = slotOf(font_atlas);
        if (device->header.struct_size >=
                offsetof(ResonateRenderDevice, bind_image_slot) +
                    sizeof(device->bind_image_slot) &&
            device->bind_image_slot != nullptr)
        {
            device->bind_image_slot(device->self, font_atlas_slot, font_atlas, 1U);
        }
        font_atlas_revision = atlas.revision;
        font_atlas_ready = true;
    }
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
    text_enabled = ecs::ComponentTraits<ui::TextAtlasPage>::index != ecs::kInvalidComponent;
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
    applyPresentMode();
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
    /* Mobile tier: a low uniform-light budget — the fallback shading loop runs
       it per pixel with no clustering, so this is the dominant pixel cost. */
    const std::uint32_t light_budget = compat_tier ? 8U : kUniformLights;
    const auto uniform_lights = static_cast<std::uint32_t>(
        std::min<std::size_t>(scratch.lights.size(), light_budget));
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
    /* x = light count, yz = the cluster tile in render pixels, w = the
       projection's y scale. The tile follows the scaled sub-viewport, which
       the render thread knows at draw time, so it fills yz there; the cluster
       pass needs the projection scale alone (view_proj carries the camera's
       rotation and cannot be unprojected with it). */
    scratch.frame_data.light_count_and_pad =
        Vec4{static_cast<float>(scratch.lights.size()), 0.0F, 0.0F, projection.m[1][1]};

    /* CSM: three logarithmic splits, ascending, the last at 35% of the view
       distance — shadow range trades against texel density. Each cascade's
       ortho box fits the main camera's frustum slice as seen from the light. */
    constexpr float kSplitRatio = 0.35F;
    const Vec3 light_forward = normalize(directional_direction);
    const Vec3 light_side = normalize(cross(Vec3{0.0F, 1.0F, 0.0F}, light_forward));
    const Vec3 light_up = cross(light_forward, light_side);
    const std::uint32_t cascades = compat_tier ? 1U : kCascades;
    float previous_split = near_z;
    float split_values[3] = {far_z, far_z, far_z};
    for (std::uint32_t cascade = 0; cascade < kCascades && cascade < cascades; ++cascade)
    {
        /* The compat tier renders one cascade over the whole shadow range: its
           shader cannot pick a cascade per pixel, and a mobile GPU cannot afford
           the fetches the three-map fallback would cost. */
        const float split = cascades == 1U ? far_z
                                           : far_z * std::pow(kSplitRatio,
                                                              static_cast<float>(kCascades - cascade));
        split_values[cascade] = split;
        Vec3 minimum;
        Vec3 maximum;
        /* The unprojection divides by the projection's own x/y scales; with
           view_proj they would carry the camera's rotation, and a yaw near 90
           degrees would divide by zero. */
        cascadeBounds(projection, camera_world, previous_split, split, minimum, maximum);
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
            scratch.text_atlas.reset();
            if (text_enabled)
            {
                if (const auto* page = world.get<ui::TextAtlasPage>(root.entity))
                {
                    if (page->revision != text_atlas_revision &&
                        world.alive(page->pixels))
                    {
                        auto atlas = std::make_shared<UiAtlas>();
                        atlas->width = page->width;
                        atlas->height = page->height;
                        atlas->revision = page->revision;
                        const auto* pixels =
                            static_cast<const std::uint8_t*>(world.blobData(page->pixels));
                        const std::size_t size = world.blobSize(page->pixels);
                        if (pixels != nullptr)
                        {
                            atlas->pixels.assign(pixels, pixels + size);
                        }
                        text_atlas_cache = std::move(atlas);
                        text_atlas_revision = page->revision;
                    }
                    scratch.text_atlas = text_atlas_cache;
                }
            }
        }
    }
    return true;
}

void RenderSystem::Impl::publish()
{
    const auto wait_begin = std::chrono::steady_clock::now();
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
            if (profile.enabled)
            {
                profile.publish_wait_ms.fetch_add(
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                              wait_begin)
                        .count());
            }
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
    ui_constants.font_slot = uiTextureSlot(ui::kTextureFont);
    ui_constants.overlay_slot = uiTextureSlot(kUiTextOverlayTexture);
    const auto quads = active->ui.quads();
    /* With tgfx ready the blend pass samples the tgfx target itself; the ui
       pass clears only, and no vertices ride the buffer. */
    if (!quads.empty() && !ui_tgfx_ready)
    {
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
            const auto corner = [&quad](float x, float y, float u, float v) -> UiVertex
            {
                return UiVertex{x,            y,            quad.color.x, quad.color.y,
                                quad.color.z, quad.color.w, u,            v,
                                quad.width,   quad.height,  quad.radius,  quad.border};
            };
            const UiVertex top_left = corner(quad.x, quad.y, quad.uv_min.x, quad.uv_min.y);
            const UiVertex top_right = corner(right, quad.y, quad.uv_max.x, quad.uv_min.y);
            const UiVertex bottom_right = corner(right, bottom, quad.uv_max.x, quad.uv_max.y);
            const UiVertex bottom_left = corner(quad.x, bottom, quad.uv_min.x, quad.uv_max.y);

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
        ui_drawn_vertices = static_cast<std::uint32_t>(ui_vertex_scratch.size());
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
    /* Above 1 the device renders on its transient MSAA attachments and
       resolves into the HDR target; the declared depth is ignored there. */
    pass.samples = msaa;
    /* Dynamic resolution: the scene renders into the scaled sub-rectangle; the
       tonemap reads exactly that window back up. */
    pass.viewport_width = render_width;
    pass.viewport_height = render_height;
    if (device.begin_pass(device.self, &pass) != RESONATE_OK)
    {
        return;
    }
    /* Until the albedo images exist there is no texture slot the scene may
       sample: the table's free entries are not the white dummy once the graph
       has taken them, and an unready slot would sample a render target. The
       pass still clears, so the first frame shows the sky. */
    if (!albedo_ready)
    {
        device.end_pass(device.self);
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
        /* Slots past the tier's cascade count mirror cascade 0: the compat
           shader reads slot 1 only, and unbound table entries stay dummy. */
        const std::uint32_t source = std::min(cascade, compat_tier ? 1U : kCascades - 1U);
        constants.slots2[1 + cascade] = commands.imageSlot(shadow_resource[source]);
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

#if defined(RESONATE_WITH_TGFX)
    /* With tgfx ready this pass only clears its target for the graph's
       resource bookkeeping; the blend pass samples the tgfx target directly
       (see blendEntry, which also owns its barrier). */
    if (ui_tgfx_ready)
    {
        ResonateRenderPassDesc clear_only{};
        clear_only.struct_size = sizeof(clear_only);
        clear_only.color = commands.image(ui_resource);
        clear_only.clear_color = ResonateVec4{0.0F, 0.0F, 0.0F, 0.0F};
        if (device.begin_pass(device.self, &clear_only) == RESONATE_OK)
        {
            device.end_pass(device.self);
        }
        return;
    }
#endif

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
    /* Every pass but the final blend renders into the scaled sub-rectangle; the
       blend is what magnifies it to the output. */
    if (target != canvas_resource)
    {
        pass.viewport_width = render_width;
        pass.viewport_height = render_height;
    }
    /* RESONATE_PASS_SENTINELS: each post pass clears to a colour of its own, so a
       frame that arrives showing only a clear names the pass whose draw was lost
       instead of looking identical for all of them. This is what identified the
       dropped-pass flash. */
    static const bool sentinels = std::getenv("RESONATE_PASS_SENTINELS") != nullptr;
    if (sentinels)
    {
        if (pipeline.value == composite_pipeline.value)
        {
            pass.clear_color = ResonateVec4{0.0F, 1.0F, 0.0F, 1.0F};
        }
        else if (pipeline.value == fxaa_pipeline.value)
        {
            pass.clear_color = ResonateVec4{0.0F, 0.0F, 1.0F, 1.0F};
        }
        else if (pipeline.value == blend_pipeline.value)
        {
            pass.clear_color = ResonateVec4{1.0F, 0.0F, 1.0F, 1.0F};
        }
    }
    static const bool trace_failures = std::getenv("RESONATE_FRAME_TRACE") != nullptr;
    const ResonateStatus begun = device.begin_pass(device.self, &pass);
    if (begun != RESONATE_OK)
    {
        if (trace_failures)
        {
            std::fprintf(stderr, "renderer: post pass begin failed (%d), pipeline %llu\n",
                         static_cast<int>(begun), static_cast<unsigned long long>(pipeline.value));
        }
        return;
    }
    const ResonateStatus bound = device.bind_pipeline(device.self, pipeline);
    if (bound != RESONATE_OK)
    {
        if (trace_failures)
        {
            std::fprintf(stderr, "renderer: post pass bind failed (%d), pipeline %llu\n",
                         static_cast<int>(bound), static_cast<unsigned long long>(pipeline.value));
        }
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
    const ResonateStatus drawn = device.draw(device.self, 3U, 1U);
    if (drawn != RESONATE_OK && trace_failures)
    {
        std::fprintf(stderr, "renderer: post pass draw failed (%d), pipeline %llu\n",
                     static_cast<int>(drawn), static_cast<unsigned long long>(pipeline.value));
    }
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
    const float uv_x = static_cast<float>(self->render_width) / static_cast<float>(self->target_width);
    const float uv_y = static_cast<float>(self->render_height) / static_cast<float>(self->target_height);
    self->runPost(commands, self->bright_resource, self->bright_pipeline,
                  commands.imageSlot(self->hdr_resource), 0U, 0U,
                  Vec4{self->features.bloom_threshold, uv_x, 0.0F, uv_y});
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

namespace
{

/* The transfer function the presented surface applies: an SRGB format encodes
   on write. A writer without the surface-format tail predates the query and
   always presented through sRGB; UNKNOWN (no target yet) reads the same way,
   so the encode switches on with the swapchain that needs it. */
bool surfaceEncodes(ResonateRenderDevice& device)
{
    if (device.header.struct_size <
            offsetof(ResonateRenderDevice, surface_format) + sizeof(device.surface_format) ||
        device.surface_format == nullptr)
    {
        return true;
    }
    switch (device.surface_format(device.self))
    {
        case RESONATE_RENDER_FORMAT_R8G8B8A8_SRGB:
        case RESONATE_RENDER_FORMAT_B8G8R8A8_SRGB:
        case RESONATE_RENDER_FORMAT_UNKNOWN:
            return true;
        default:
            return false;
    }
}

} // namespace

void RenderSystem::Impl::compositeEntry(void* context, GraphExecuteContext& commands)
{
    auto* self = static_cast<RenderSystem::Impl*>(context);
    const bool bloom = self->features.has("bloom");
    /* y/w carry the uv window: the scene rendered into the scaled sub-rectangle
       of the full-size HDR target, and the tonemap reads it back up. x is 1
       when the surface presents linear and the pass owns the encode. */
    const float uv_x = static_cast<float>(self->render_width) / static_cast<float>(self->target_width);
    const float uv_y = static_cast<float>(self->render_height) / static_cast<float>(self->target_height);
    const float encode = surfaceEncodes(*commands.device) ? 0.0F : 1.0F;
    self->runPost(commands, self->ldr_resource, self->composite_pipeline,
                  commands.imageSlot(self->hdr_resource),
                  bloom ? commands.imageSlot(self->blur_b_resource) : 0U, 0U,
                  Vec4{encode, uv_x, bloom ? 1.0F : 0.0F, uv_y});
}

void RenderSystem::Impl::fxaaEntry(void* context, GraphExecuteContext& commands)
{
    auto* self = static_cast<RenderSystem::Impl*>(context);
    const float uv_x = static_cast<float>(self->render_width) / static_cast<float>(self->target_width);
    const float uv_y = static_cast<float>(self->render_height) / static_cast<float>(self->target_height);
    self->runPost(commands, self->ldr_aa_resource, self->fxaa_pipeline,
                  commands.imageSlot(self->ldr_resource), 0U, 0U,
                  Vec4{1.0F / static_cast<float>(self->target_width),
                       1.0F / static_cast<float>(self->target_height), uv_x, uv_y});
}

namespace
{

/* VkImageLayout value -> RHI layout enum, for barriers that follow work done
   outside this device (tgfx reports its target's layout as a Vk value). */
ResonateRenderImageLayout rhiLayoutOfVk(std::uint32_t vk_layout)
{
    switch (vk_layout)
    {
        case 1U: return RESONATE_RENDER_LAYOUT_GENERAL;
        case 2U: return RESONATE_RENDER_LAYOUT_COLOR_ATTACHMENT;
        case 3U: return RESONATE_RENDER_LAYOUT_DEPTH_ATTACHMENT;
        case 5U: return RESONATE_RENDER_LAYOUT_SHADER_READ_ONLY;
        case 6U: return RESONATE_RENDER_LAYOUT_TRANSFER_SRC;
        case 7U: return RESONATE_RENDER_LAYOUT_TRANSFER_DST;
        case 1000000002U: return RESONATE_RENDER_LAYOUT_PRESENT;
        default: break;
    }
    return RESONATE_RENDER_LAYOUT_UNDEFINED;
}

} // namespace

void RenderSystem::Impl::blendEntry(void* context, GraphExecuteContext& commands)
{
    auto* self = static_cast<RenderSystem::Impl*>(context);
    ResonateRenderDevice& device = *commands.device;
    const ResourceId scene =
        self->features.antialiases() ? self->ldr_aa_resource : self->ldr_resource;
    /* With tgfx ready the blend pass samples the tgfx target directly — the
       overlay pass is skipped entirely, which is also the Mali-compatible
       path. The barrier guards the target's last write (tgfx's own submit or
       the readback upload) before this pass reads it. */
    std::uint32_t ui_slot = commands.imageSlot(self->ui_resource);
    if (self->ui_tgfx_ready)
    {
        ResonateRenderBarrierDesc barrier = {};
        barrier.struct_size = sizeof(barrier);
        barrier.image = self->ui_text_image;
        barrier.after_stages = RESONATE_RENDER_STAGE_FRAGMENT;
        barrier.after_access = RESONATE_RENDER_ACCESS_SHADER_READ;
        barrier.after_layout = RESONATE_RENDER_LAYOUT_SHADER_READ_ONLY;
        ResonateRenderImageLayout tgfx_layout = RESONATE_RENDER_LAYOUT_SHADER_READ_ONLY;
        if (self->ui_target_attached)
        {
            /* The sampled layout must match what tgfx reported, and the image
               goes back to that layout after this pass: tgfx's next draw
               starts from its own bookkeeping, so the barrier's layouts have
               to match the state tgfx left the image in. */
            const ResonateRenderImageLayout reported =
                rhiLayoutOfVk(resonate_tgfx_ui_target_layout(self->tgfx_ui));
            tgfx_layout = reported != RESONATE_RENDER_LAYOUT_UNDEFINED
                              ? reported
                              : RESONATE_RENDER_LAYOUT_GENERAL;
            barrier.before_stages = RESONATE_RENDER_STAGE_COLOR_OUTPUT;
            barrier.before_access = RESONATE_RENDER_ACCESS_COLOR_WRITE;
            barrier.before_layout = tgfx_layout;
        }
        else
        {
            barrier.before_stages = RESONATE_RENDER_STAGE_TRANSFER;
            barrier.before_access = RESONATE_RENDER_ACCESS_TRANSFER_WRITE;
            barrier.before_layout = RESONATE_RENDER_LAYOUT_SHADER_READ_ONLY;
        }
        device.barrier(device.self, &barrier);
        ui_slot = self->ui_text_slot;
        self->runPost(commands, self->canvas_resource, self->blend_pipeline,
                      commands.imageSlot(scene), 0U, ui_slot, Vec4{0.0F, 0.0F,
                           static_cast<float>(self->render_width) / static_cast<float>(self->target_width),
                           static_cast<float>(self->render_height) / static_cast<float>(self->target_height)});
        if (self->ui_target_attached && tgfx_layout != RESONATE_RENDER_LAYOUT_SHADER_READ_ONLY)
        {
            /* Return the target to tgfx's layout for its next draw. */
            ResonateRenderBarrierDesc restore = barrier;
            restore.before_stages = RESONATE_RENDER_STAGE_FRAGMENT;
            restore.before_access = RESONATE_RENDER_ACCESS_SHADER_READ;
            restore.before_layout = RESONATE_RENDER_LAYOUT_SHADER_READ_ONLY;
            restore.after_stages = RESONATE_RENDER_STAGE_COLOR_OUTPUT;
            restore.after_access = RESONATE_RENDER_ACCESS_COLOR_WRITE;
            restore.after_layout = tgfx_layout;
            device.barrier(device.self, &restore);
        }
        return;
    }
    self->runPost(commands, self->canvas_resource, self->blend_pipeline,
                  commands.imageSlot(scene), 0U, ui_slot, Vec4{0.0F, 0.0F,
                           static_cast<float>(self->render_width) / static_cast<float>(self->target_width),
                           static_cast<float>(self->render_height) / static_cast<float>(self->target_height)});
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

void RenderSystem::setUiFontPaths(std::vector<std::string> paths)
{
#if defined(RESONATE_WITH_TGFX)
    impl_->ui_font_paths = std::move(paths);
#else
    (void)paths;
#endif
}

void RenderSystem::setRenderScale(float scale)
{
    impl_->render_scale = std::clamp(scale, Impl::kMinRenderScale, 1.0F);
}

void RenderSystem::setTargetFrameMs(float milliseconds)
{
    impl_->target_frame_ms = milliseconds > 0.0F ? milliseconds : 0.0F;
}

bool RenderSystem::setHdrFormat(const std::string& name, std::string& out_error)
{
    if (name != "auto" && name != "rgba16f" && name != "b10g11r11")
    {
        out_error = "unknown hdr format '" + name + "' (auto, rgba16f or b10g11r11)";
        return false;
    }
    impl_->hdr_format_name = name;
    /* The graph is built from the format: the next frame recompiles it. */
    impl_->initialized = false;
    out_error.clear();
    return true;
}

void RenderSystem::setShadowSize(std::uint32_t size)
{
    impl_->shadow_size = size;
    /* The shadow maps are graph resources: the next frame recompiles it. */
    impl_->initialized = false;
}

void RenderSystem::setPresentMode(const std::string& name)
{
    if (name != "auto" && name != "fifo" && name != "mailbox" && name != "immediate")
    {
        impl_->complain(("unknown present mode '" + name + "'").c_str());
        return;
    }
    impl_->present_mode_name = name;
    impl_->applyPresentMode();
}

const std::string& RenderSystem::presentModeName() const
{
    return impl_->present_mode_name;
}

void RenderSystem::Impl::applyPresentMode()
{
    if (device == nullptr || present_mode_name == "auto" ||
        device->header.struct_size <
            offsetof(ResonateRenderDevice, set_present_mode) + sizeof(device->set_present_mode) ||
        device->set_present_mode == nullptr)
    {
        return;
    }
    uint32_t mode = RESONATE_RENDER_PRESENT_FIFO;
    if (present_mode_name == "mailbox")
    {
        mode = RESONATE_RENDER_PRESENT_MAILBOX;
    }
    else if (present_mode_name == "immediate")
    {
        mode = RESONATE_RENDER_PRESENT_IMMEDIATE;
    }
    device->set_present_mode(device->self, mode);
}

void RenderSystem::setMsaa(std::uint32_t samples)
{
    impl_->msaa = samples;
    /* The scene pass and its pipeline carry the sample count: the next frame
       recompiles both. */
    impl_->initialized = false;
}

float RenderSystem::renderScale() const
{
    return impl_->render_scale;
}

float RenderSystem::targetFrameMs() const
{
    return impl_->target_frame_ms;
}

std::uint32_t RenderSystem::msaa() const
{
    return impl_->msaa;
}

std::uint32_t RenderSystem::shadowSize() const
{
    return impl_->shadow_size;
}

const std::string& RenderSystem::hdrFormatName() const
{
    return impl_->hdr_format_name;
}

std::string RenderSystem::featureChain() const
{
    std::string chain;
    for (const std::string& entry : impl_->features.chain)
    {
        if (!chain.empty())
        {
            chain += ", ";
        }
        chain += entry;
    }
    return chain;
}

const std::string& RenderSystem::antialiasing() const
{
    return impl_->features.antialiasing;
}

float RenderSystem::bloomThreshold() const
{
    return impl_->features.bloom_threshold;
}

void RenderSystem::setUiImageTexture(std::uint32_t id, std::string path)
{
#if defined(RESONATE_WITH_TGFX)
    impl_->ui_image_requests.push_back(Impl::UiImageRequest{id, std::move(path)});
#else
    (void)id;
    (void)path;
#endif
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
#if defined(RESONATE_WITH_TGFX)
    /* tgfx holds the shared device; release it before the module detaches. */
    impl.releaseTgfx();
#endif
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
    const auto extract_begin = std::chrono::steady_clock::now();
    if (!impl.extract(world))
    {
        return;
    }
    if (impl.profile.enabled)
    {
        impl.profile.extract_ms.fetch_add(std::chrono::duration<double, std::milli>(
                                              std::chrono::steady_clock::now() - extract_begin)
                                              .count());
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

        const auto frame_begin = std::chrono::steady_clock::now();
        if (device != nullptr && canvas != nullptr)
        {
            renderFrame(*snapshot);
        }
        if (target_frame_ms > 0.0F && warmup_frames > 0U)
        {
            --warmup_frames;
            sample_since = std::chrono::steady_clock::now();
            sample_ms = 0.0;
            sample_frames = 0U;
        }
        else if (target_frame_ms > 0.0F)
        {
            /* One decision per window: a step every frame would chase noise and
               rebuild the graph constantly. */
            sample_ms += std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - frame_begin)
                             .count();
            ++sample_frames;
            const double window =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - sample_since)
                    .count();
            if (window >= 0.5 && sample_frames >= 10U)
            {
                const double average = sample_ms / static_cast<double>(sample_frames);
                /* Frame time is roughly linear in pixels, so the square root of
                   the budget ratio is the scale that would hit the budget; the
                   step is bounded so one noisy window cannot swing resolution. */
                float wanted = render_scale;
                if (average > target_frame_ms * 1.15 || average < target_frame_ms * 0.85)
                {
                    const float ratio = static_cast<float>(target_frame_ms / average);
                    wanted = std::clamp(render_scale * std::sqrt(ratio), render_scale - 0.15F,
                                        render_scale + 0.1F);
                    wanted = std::clamp(wanted, kMinRenderScale, 1.0F);
                }
                if (wanted != render_scale)
                {
                    render_scale = wanted;
                    std::fprintf(stderr, "renderer: render scale %.2f for %.1f ms\n",
                                 static_cast<double>(render_scale), average);
                }
                sample_ms = 0.0;
                sample_frames = 0U;
                sample_since = std::chrono::steady_clock::now();
            }
        }
        if (profile.enabled)
        {
            profile.frame_ms.fetch_add(std::chrono::duration<double, std::milli>(
                                           std::chrono::steady_clock::now() - frame_begin)
                                           .count());
            const double reported = std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                                                  profile_since)
                                        .count();
            if (reported >= 1.0)
            {
                const double frames = profile_frames;
                const double scale = frames > 0.0 ? 1.0 / frames : 0.0;
                std::fprintf(stderr,
                             "renderer: profile %.0f fps: frame %.2f ms (ui %.2f, graph %.2f), "
                             "extract %.2f, publish wait %.2f, scale %.2f at %ux%u\n",
                             frames / reported, profile.frame_ms.exchange(0.0) * scale,
                             profile.ui_ms.exchange(0.0) * scale,
                             profile.graph_ms.exchange(0.0) * scale,
                             profile.extract_ms.exchange(0.0) * scale,
                             profile.publish_wait_ms.exchange(0.0) * scale,
                             static_cast<double>(render_scale), render_width, render_height);
                profile_frames = 0;
                profile_since = std::chrono::steady_clock::now();
            }
            ++profile_frames;
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
        static const bool trace_rebuild = std::getenv("RESONATE_FRAME_TRACE") != nullptr;
        if (trace_rebuild)
        {
            std::fprintf(stderr, "renderer: rebuild (scale %.2f) draining\n",
                         static_cast<double>(render_scale));
        }
        const ResonateStatus drained = device->wait_idle(device->self);
        if (trace_rebuild)
        {
            std::fprintf(stderr, "renderer: rebuild drained (%d), resetting\n",
                         static_cast<int>(drained));
        }
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
        if (trace_rebuild)
        {
            std::fprintf(stderr, "renderer: rebuild reset, initializing\n");
        }
        if (!initialize())
        {
            initialized = false;
            return false;
        }
        initialized = true;
        if (trace_rebuild)
        {
            std::fprintf(stderr, "renderer: rebuild done\n");
        }
    }
    return ensurePipelines();
}

void RenderSystem::Impl::renderFrame(FrameSnapshot& snapshot)
{
    /* Diagnostic rebuild stress: drop the plan so prepareTargets drains and
       re-initializes, retiring every graph image at the same shape. */
    ++rendered_frames;
    if (rebuild_stress != 0U && rendered_frames % rebuild_stress == 0U)
    {
        initialized = false;
    }
    /* Read before the first `initialize`: the graph is built from the scale, and
       reading it later would build at full size and never revisit it. */
    if (!prepareTargets(snapshot.target_width, snapshot.target_height))
    {
        return;
    }
    /* The scaled extent is state, not graph shape: the passes below render into
       this sub-rectangle of the full-size targets. */
    render_width = std::max(1U, static_cast<std::uint32_t>(
                                    static_cast<float>(target_width) * render_scale + 0.5F));
    render_height = std::max(1U, static_cast<std::uint32_t>(
                                     static_cast<float>(target_height) * render_scale + 0.5F));
    /* The cluster tile is a share of the scene's viewport, not of the output:
       the fragment's position spans the scaled sub-rectangle. */
    snapshot.frame_data.light_count_and_pad.y =
        static_cast<float>(render_width) / static_cast<float>(kClustersX);
    snapshot.frame_data.light_count_and_pad.z =
        static_cast<float>(render_height) / static_cast<float>(kClustersY);
#if defined(RESONATE_WITH_TGFX)
    /* The text layer is uploaded outside the frame; the UI pass only samples
       it. A failed overlay leaves the atlas glyph quads in place. */
    const auto ui_begin = std::chrono::steady_clock::now();
    renderTgfxUi(snapshot);
    if (profile.enabled)
    {
        profile.ui_ms.fetch_add(std::chrono::duration<double, std::milli>(
                                    std::chrono::steady_clock::now() - ui_begin)
                                    .count());
    }
#endif
    /* Before the pass: the atlas slot must resolve inside the same frame the
       glyphs first appear, or frame one draws them as solid rectangles. */
    updateFontAtlas(snapshot);
    std::string error;
    const auto graph_begin = std::chrono::steady_clock::now();
    const bool executed = executor->execute(error, &Impl::prepareEntry, this);
    if (profile.enabled)
    {
        profile.graph_ms.fetch_add(std::chrono::duration<double, std::milli>(
                                       std::chrono::steady_clock::now() - graph_begin)
                                       .count());
    }
    if (!executed && !error.empty())
    {
        complain(error.c_str());
    }
    if (!albedo_ready && !albedo_attempted)
    {
        /* Created after the first frame on purpose: the Mali driver samples an
           image uploaded before the first present as a constant. A failed
           creation leaves the scene skipped rather than sampling the frame. */
        albedo_attempted = true;
        device->wait_idle(device->self);
        albedo_ready = createAlbedoTextures(snapshot);
        if (!albedo_ready)
        {
            complain("albedo textures unavailable");
        }
    }
}

} // namespace resonate::renderer
