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

    /* Registers the render components; idempotent, and a refusal (a name
       collision) answers false. */
    static bool registerComponents(ecs::World& world);

    /* The engine-system entry a run registers with addEngineSystem (RENDER
       stage); the command buffer and the delta are unused. */
    static void invoke(void* context, ecs::World& world, ecs::CommandBuffer& commands,
                       float delta_seconds);

    /* One extract-and-draw pass; callable directly (tests, tools). */
    void run(ecs::World& world);

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
