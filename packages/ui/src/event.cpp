#include <resonate/ui/event.h>

#include <algorithm>
#include <cstddef>
#include <vector>

#include <resonate/hierarchy/tree.h>
#include <resonate/ui/hit_test.h>

#include "resonate.ui/components.gen.h"

namespace resonate::ui
{
namespace
{

/* Wheel detents scroll this many logical pixels when the view names no row
   step; a step scrolls by whole rows, which keeps a clipped list from showing
   half rows at its edges. */
constexpr float kWheelPixels = 48.0F;

/* One scratch path per thread: dispatch runs once per event and the tree only
   grows, so the buffer is allocated once and reused. */
std::vector<ecs::Entity>& scratch()
{
    static thread_local std::vector<ecs::Entity> path;
    return path;
}

/* The widget the event is about: the topmost widget at the point, or its
   nearest focusable ancestor. A non-focusable Input (a disabled widget) shields
   its ancestors and leaves the event without a target. */
ecs::Entity eventTarget(ecs::World& world, ecs::Entity canvas, float x, float y)
{
    ecs::Entity hit = hitTest(world, canvas, x, y);
    for (std::uint32_t depth = 0U; hit.valid() && depth <= hierarchy::kMaxDepth; ++depth)
    {
        const Input* input = world.get<Input>(hit);
        if (input != nullptr)
        {
            return input->focusable ? hit : ecs::Entity{};
        }
        hit = hierarchy::parentOf(world, hit);
    }
    return {};
}

/* The target and every ancestor up to the canvas, target first. */
void collectPath(ecs::World& world, ecs::Entity canvas, ecs::Entity target,
                 std::vector<ecs::Entity>& path)
{
    path.clear();
    for (std::uint32_t depth = 0U; target.valid() && depth <= hierarchy::kMaxDepth; ++depth)
    {
        path.push_back(target);
        if (target == canvas)
        {
            return;
        }
        target = hierarchy::parentOf(world, target);
    }
}

bool invoke(ecs::World& world, const ecs::Entity* entities, std::size_t count, EventPhase phase,
            Event& event)
{
    for (std::size_t index = 0U; index < count; ++index)
    {
        const ecs::Entity entity = entities[index];
        if (!world.alive(entity))
        {
            continue;
        }
        const Listener* listener = world.get<Listener>(entity);
        if (listener == nullptr || listener->handler == nullptr ||
            (listener->kinds & eventBit(event.kind)) == 0U)
        {
            continue;
        }
        if (listener->handler(listener->context, world, entity, phase, event))
        {
            event.consumed = true;
            return true;
        }
    }
    return false;
}

/* The built-in action a ScrollView performs for a wheel event. The first view on
   the path that can still move owns the event. */
bool scrollChain(ecs::World& world, const ecs::Entity* entities, std::size_t count,
                 const Event& event)
{
    for (std::size_t index = 0U; index < count; ++index)
    {
        const ecs::Entity entity = entities[index];
        if (!world.alive(entity))
        {
            continue;
        }
        ScrollView* view = world.get<ScrollView>(entity);
        const ComputedRect* rect = world.get<ComputedRect>(entity);
        if (view == nullptr || rect == nullptr)
        {
            continue;
        }
        const float max_scroll = std::max(0.0F, view->content_height - rect->height);
        const float step = view->step > 0.0F ? view->step : kWheelPixels;
        const float wanted = std::clamp(view->scroll + event.wheel * step, 0.0F, max_scroll);
        if (wanted == view->scroll)
        {
            continue;
        }
        view->scroll = wanted;
        world.markChanged<ScrollView>(entity);
        return true;
    }
    return false;
}

} // namespace

bool dispatchEvent(ecs::World& world, ecs::Entity canvas, Event& event)
{
    if (!world.alive(canvas))
    {
        return false;
    }
    const ecs::Entity target = eventTarget(world, canvas, event.x, event.y);
    if (!target.valid())
    {
        return false;
    }

    std::vector<ecs::Entity>& path = scratch();
    collectPath(world, canvas, target, path);
    if (path.empty())
    {
        return false;
    }
    const std::size_t count = path.size();

    /* Capture runs canvas first, target last; the target phase is that last
       entry; bubble runs the target's parent upward to the canvas. */
    std::reverse(path.begin(), path.end());
    if (invoke(world, path.data(), count, EventPhase::Capture, event))
    {
        return true;
    }
    if (invoke(world, path.data() + (count - 1U), 1U, EventPhase::Target, event))
    {
        return true;
    }
    if (invoke(world, path.data(), count - 1U, EventPhase::Bubble, event))
    {
        return true;
    }

    if (event.kind == EventKind::Wheel)
    {
        std::reverse(path.begin(), path.end());
        if (scrollChain(world, path.data(), count, event))
        {
            event.consumed = true;
            return true;
        }
    }
    return false;
}

} // namespace resonate::ui
