#ifndef RESONATE_HIERARCHY_SYSTEM_H
#define RESONATE_HIERARCHY_SYSTEM_H

#include <memory>

#include <resonate/core/job.h>
#include <resonate/ecs/command_buffer.h>
#include <resonate/ecs/world.h>

/*
 * Transform propagation: every transform node's WorldTransform is recomputed
 * from its LocalTransform and its parent's WorldTransform. The nodes are
 * processed level by level — same-depth subtrees are disjoint, so a level's
 * nodes are written in parallel and the next level starts once every parent of
 * it has been written.
 *
 * Derived values are not stamped as changes: what a consumer reacts to is the
 * LocalTransform write, while WorldTransform is recomputed for every node on
 * every run.
 */
namespace resonate::hierarchy
{

/* One per world; keeps the registered query and the level scratch between
   frames. */
class PropagationSystem
{
  public:
    PropagationSystem();
    ~PropagationSystem();

    PropagationSystem(const PropagationSystem&) = delete;
    PropagationSystem& operator=(const PropagationSystem&) = delete;

    /* Borrowed; null runs the levels on the calling thread. */
    JobSystem* jobs = nullptr;

    /* One pass. Callable directly (tests, tools) or through the frame loop. */
    void run(ecs::World& world);

    /* The world-aware system entry a run registers with addEngineSystem; the
       command buffer and the delta are unused. */
    static void invoke(void* context, ecs::World& world, ecs::CommandBuffer& commands,
                       float delta_seconds);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace resonate::hierarchy

#endif /* RESONATE_HIERARCHY_SYSTEM_H */
