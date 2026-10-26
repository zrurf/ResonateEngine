#ifndef RESONATE_RENDERER_RENDERER_H
#define RESONATE_RENDERER_RENDERER_H

/* The engine's forward renderer.
 *
 * Each frame the render system extracts the scene — the camera, every mesh
 * instance placed by its world transform — into plain frame data, and runs a
 * compiled graph against the run's device capability. The device appears when
 * the render module attaches, so a headless run extracts nothing and draws
 * nothing: the system returns until a device is in the registry.
 *
 * `setRegistry` hands over the run's capability registry, which is where the
 * device is found; call it before the frame loop starts. The scene the demo
 * draws is seeded by `composeDemoScene` until scenes load from disk.
 */

#include <memory>
#include <string>
#include <vector>

#include <resonate/ecs/command_buffer.h>
#include <resonate/ecs/world.h>

struct ResonateCapabilityRegistry;

namespace resonate::renderer
{

/* The view's post-chain selection. v1 covers the post chain; the scene,
   lighting and shadow passes are fixed. `chain` names the ordered post
   features — "bloom" is optional, "tonemap" is required — and `antialiasing`
   names the AA slot, "fxaa" or "none". */
struct FeatureStack
{
    std::vector<std::string> chain;
    std::string antialiasing;
    float bloom_threshold = 1.0F;

    [[nodiscard]] bool has(const std::string& name) const;
    [[nodiscard]] bool antialiases() const; /* the AA slot is not "none" */

    static FeatureStack defaults();
};

/* Parses the comma-separated chain and the AA slot name, validating names,
   duplicates, order and the slot. A refusal names the offending entry. */
bool parseFeatureStack(const std::string& chain, const std::string& antialiasing,
                       FeatureStack& out_stack, std::string& out_error);

/* The rules parseFeatureStack applies; callable on a hand-built stack. */
bool validateFeatureStack(const FeatureStack& stack, std::string& out_error);

class RenderSystem
{
  public:
    RenderSystem();
    ~RenderSystem();

    RenderSystem(const RenderSystem&) = delete;
    RenderSystem& operator=(const RenderSystem&) = delete;

    /* The registry the device capability is resolved through. Borrowed; the
       host outlives the frame loop. */
    void setRegistry(ResonateCapabilityRegistry* registry);

    /* The post-chain selection, validated by parseFeatureStack before this
       call; the defaults apply when it is never called. */
    void setFeatures(const FeatureStack& stack);

    /* Font files the tgfx text overlay loads, first that parses wins. Call
       before the frame loop; a platform without a tgfx build ignores it. */
    void setUiFontPaths(std::vector<std::string> paths);

    /* Registers the Picture texture `id` and the image file that backs it;
       decoded through tgfx on the render thread and uploaded once. Call before
       the frame loop; a platform without a tgfx build ignores it. */
    void setUiImageTexture(std::uint32_t id, std::string path);

    /* The fraction of the target the 3D and post targets render at, 0.25..1.
       The UI keeps full resolution, so text stays sharp while a mobile GPU
       spends its budget on fewer pixels. Changing it mid-run is allowed: the
       graph is rebuilt between frames. */
    void setRenderScale(float scale);

    /* A positive frame time enables adaptive scaling: the render scale moves
       within its bounds to hold roughly this budget, so a weak GPU trades
       resolution for frame rate and a strong one keeps full resolution. Zero
       keeps the fixed scale. */
    void setTargetFrameMs(float milliseconds);

    /* The HDR scene target's format: "auto" picks the tier's choice (packed
       B10G11R11 on a mobile GPU, RGBA16F otherwise); "rgba16f" and
       "b10g11r11" name one explicitly. Answers false with the reason for an
       unknown name. Call before the frame loop. */
    [[nodiscard]] bool setHdrFormat(const std::string& name, std::string& out_error);

    /* The shadow map's extent in texels, square; zero keeps the tier's choice
       (1024 on a mobile GPU, 2048 otherwise). Call before the frame loop. */
    void setShadowSize(std::uint32_t size);

    /* Registers the render components; idempotent, and a refusal (a name
       collision) answers false. */
    static bool registerComponents(ecs::World& world);

    /* The engine-system entry a run registers with addEngineSystem (RENDER
       stage); the command buffer and the delta are unused. */
    static void invoke(void* context, ecs::World& world, ecs::CommandBuffer& commands,
                       float delta_seconds);

    /* Extracts the scene and publishes the frame to the render thread, which
       owns the device calls. Callable directly (tests, tools). */
    void run(ecs::World& world);

    /* Blocks until the render thread has drained every published frame; for
       tests and tools that read the device state afterwards. */
    void waitIdle();

    /* Attaches the target, compiles the graph and creates every pipeline
       before the frame loop starts (Application's startup hook). Safe before
       the first run; a no-op after release. */
    void warmup();

    /* Releases the graph's device resources. The run calls it before the
       modules detach, while the device that created them is still alive; safe
       to call twice. */
    void release();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/* Spawns the demo scene: a camera looking at a ground plane and a field of
   colored boxes, all straight ECS entities. */
void composeDemoScene(ecs::World& world);

} // namespace resonate::renderer

#endif /* RESONATE_RENDERER_RENDERER_H */
