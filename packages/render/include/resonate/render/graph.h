#ifndef RESONATE_RENDER_GRAPH_H
#define RESONATE_RENDER_GRAPH_H

/*
 * The render graph: what the frame's passes declare, and the plan compiling it.
 *
 * A frame is declared, not hand-ordered: passes name the resources they read
 * and write — in the sync2 vocabulary of stage/access/layout — and compile()
 * derives everything the executor needs. Execution order is declaration order
 * compacted by culling: the assembler places passes in chain order, and
 * compilation never reorders them.
 *
 * compile() runs entirely on the CPU. What it produces:
 *
 *   - the live passes, in declaration order, with the barriers each one needs
 *     before it runs — write-after-read, layout changes and the first touch of
 *     an imported resource all come from the declarations, never hand-written;
 *     a pass is live through what it writes: the exports and the reads of
 *     live passes are what keep writers alive, so a pass that only reads has
 *     to write something for the frame to keep it;
 *   - final barriers, taking exported resources to the state their importer
 *     declared (the swapchain backbuffer to present, for instance);
 *   - alias groups: declared (transient) resources whose lifetimes do not
 *     overlap share physical memory, grouped first-fit by kind; imported
 *     resources never alias;
 *   - per-resource lifetimes for the materializer that backs the plan with
 *     real device resources.
 *
 * Passes whose platform tags the compile target does not carry are culled
 * with the dead ones.
 */

#include <resonate/render/device.h>

#include <cstdint>
#include <string>
#include <vector>

namespace resonate::render
{

using ResourceId = std::uint32_t;
constexpr ResourceId kInvalidResource = UINT32_MAX;

/* The sync2 vocabulary a use is declared in. */
using StageFlags = std::uint32_t;
constexpr StageFlags kStageNone = 0U;
constexpr StageFlags kStageVertex = 1U << 0;
constexpr StageFlags kStageFragment = 1U << 1;
constexpr StageFlags kStageColorOutput = 1U << 2;
constexpr StageFlags kStageDepthOutput = 1U << 3;
constexpr StageFlags kStageCompute = 1U << 4;
constexpr StageFlags kStageTransfer = 1U << 5;

using AccessFlags = std::uint32_t;
constexpr AccessFlags kAccessNone = 0U;
constexpr AccessFlags kAccessShaderRead = 1U << 0;
constexpr AccessFlags kAccessShaderWrite = 1U << 1;
constexpr AccessFlags kAccessColorWrite = 1U << 2;
constexpr AccessFlags kAccessDepthRead = 1U << 3;
constexpr AccessFlags kAccessDepthWrite = 1U << 4;
constexpr AccessFlags kAccessTransferRead = 1U << 5;
constexpr AccessFlags kAccessTransferWrite = 1U << 6;

enum class ImageLayout : std::uint8_t
{
    Undefined = 0,
    ColorAttachment,
    DepthAttachment,
    ShaderReadOnly,
    TransferSrc,
    TransferDst,
    Present,
};

enum class Queue : std::uint8_t
{
    Graphics = 0,
    Compute,
    Transfer,
};

/* Tags a pass requires of the compile target; zero runs everywhere. */
using PlatformTags = std::uint32_t;
constexpr PlatformTags kTagDesktop = 1U << 0;
constexpr PlatformTags kTagHandheld = 1U << 1;
constexpr PlatformTags kTagAll = UINT32_MAX;

enum class ResourceKind : std::uint8_t
{
    Image = 0,
    Buffer,
};

struct Use
{
    ResourceId resource = kInvalidResource;
    StageFlags stages = kStageNone;
    AccessFlags access = kAccessNone;
    /* Images only; buffers ignore it. */
    ImageLayout layout = ImageLayout::Undefined;
};

struct GraphExecuteContext;

using GraphExecuteFn = void (*)(void* context, GraphExecuteContext& commands);

/* One pass of the frame. The uses arrays are copied at declaration time. */
struct PassDesc
{
    const char* name = nullptr;
    Queue queue = Queue::Graphics;
    PlatformTags tags = 0U;
    const Use* reads = nullptr;
    std::uint32_t read_count = 0U;
    const Use* writes = nullptr;
    std::uint32_t write_count = 0U;
    void* context = nullptr;
    GraphExecuteFn execute = nullptr;
};

/* What compile() hands the executor: self-contained, holding copies of the
   live passes and everything derived from the declarations. */
struct Barrier
{
    ResourceId resource = kInvalidResource;
    StageFlags before_stages = kStageNone;
    StageFlags after_stages = kStageNone;
    AccessFlags before_access = kAccessNone;
    AccessFlags after_access = kAccessNone;
    ImageLayout before_layout = ImageLayout::Undefined;
    ImageLayout after_layout = ImageLayout::Undefined;
};

struct PlanResource
{
    const char* name = nullptr;
    ResourceKind kind = ResourceKind::Image;
    bool imported = false;
    /* Images. */
    ResonateRenderFormat format = RESONATE_RENDER_FORMAT_UNKNOWN;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    /* Buffers. */
    std::uint64_t size_bytes = 0U;
    /* An imported resource's state outside the frame: the swapchain backbuffer
       starts the frame undefined and must end in present, for instance. */
    ImageLayout initial_layout = ImageLayout::Undefined;
    ImageLayout final_layout = ImageLayout::Undefined;
    bool exported = false;
};

struct PlanPass
{
    const char* name = nullptr;
    Queue queue = Queue::Graphics;
    void* context = nullptr;
    GraphExecuteFn execute = nullptr;
    std::vector<Use> reads;
    std::vector<Use> writes;
};

struct PlanStep
{
    std::uint32_t pass = 0U; /* index into GraphPlan::passes */
    std::vector<Barrier> barriers;
};

/* Declared (transient) resources whose lifetimes do not overlap may share
   physical memory; the materializer sizes the group's backing from its
   members. */
struct AliasGroup
{
    ResourceKind kind = ResourceKind::Image;
    std::vector<ResourceId> members;
};

struct ResourceLifetime
{
    std::uint32_t first_step = UINT32_MAX;
    std::uint32_t last_step = UINT32_MAX;
};

struct GraphPlan
{
    std::vector<PlanResource> resources;
    std::vector<PlanPass> passes;
    std::vector<PlanStep> steps;
    std::vector<Barrier> final_barriers;
    std::vector<AliasGroup> aliases;
    /* Indexed by resource id; UINT32_MAX first means the resource has no use
       among the live passes. */
    std::vector<ResourceLifetime> lifetimes;
};

class RenderGraph
{
  public:
    /* Transient resources: created (and aliased) per frame by the
       materializer. UNKNOWN-format images are refused — a transient target
       knows its format. */
    ResourceId declareImage(const char* name, ResonateRenderFormat format, std::uint32_t width,
                            std::uint32_t height);
    ResourceId declareBuffer(const char* name, std::uint64_t size_bytes);

    /* External resources the executor does not create: the swapchain
       backbuffer's initial and final states are the importer's contract. */
    ResourceId importImage(const char* name, ImageLayout initial_layout, ImageLayout final_layout);
    ResourceId importBuffer(const char* name);

    /* Copies the descriptor; the arrays may be temporaries. One use per
       resource per pass. */
    void addPass(const PassDesc& desc);

    /* The resources something outside the graph consumes — the backbuffer the
       present waits on, a texture a UI pass reads next frame. */
    void exportResource(ResourceId resource);

    /* Derives the plan. False on a declaration error, naming it in `error`:
       unknown resources, a resource used twice by one pass, an UNKNOWN-format
       transient image, an export of nothing. `target_tags` is the platform the
       frame compiles for; a pass whose tags the target does not carry is
       culled with the dead ones (kTagAll compiles everything). */
    bool compile(GraphPlan& out_plan, std::string& out_error,
                 PlatformTags target_tags = kTagAll) const;

    /* For tests and tools. */
    [[nodiscard]] std::uint32_t resourceCount() const
    {
        return static_cast<std::uint32_t>(resources_.size());
    }
    [[nodiscard]] std::uint32_t passCount() const
    {
        return static_cast<std::uint32_t>(passes_.size());
    }

  private:
    struct DeclaredUse
    {
        Use use;
        bool write = false;
    };

    struct DeclaredPass
    {
        PassDesc desc{};
        std::vector<DeclaredUse> reads;
        std::vector<DeclaredUse> writes;
    };

    struct DeclaredResource
    {
        PlanResource plan{};
    };

    static bool isWrite(AccessFlags access);

    std::vector<DeclaredResource> resources_;
    std::vector<DeclaredPass> passes_;
    std::vector<ResourceId> exports_;
};

} // namespace resonate::render

#endif /* RESONATE_RENDER_GRAPH_H */
