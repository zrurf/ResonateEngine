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

/* The built-in scroll action shared by the wheel and a touch drag: the first
   viewport on the chain from the widget under the point upward that can still
   move absorbs the motion; one already clamped at its edge defers to its
   ancestor. `wanted` maps a candidate viewport to the scroll it would take. */
template <typename Wanted>
bool scrollWalk(ecs::World& world, ecs::Entity canvas, float x, float y, Wanted wanted)
{
    ecs::Entity entity = hitTest(world, canvas, x, y);
    for (std::uint32_t depth = 0U; entity.valid() && depth <= hierarchy::kMaxDepth; ++depth)
    {
        ScrollView* view = world.get<ScrollView>(entity);
        const ComputedRect* rect = world.get<ComputedRect>(entity);
        if (view != nullptr && rect != nullptr)
        {
            const float max_scroll = std::max(0.0F, view->content_height - rect->height);
            const float target = std::clamp(wanted(*view), 0.0F, max_scroll);
            if (target != view->scroll)
            {
                view->scroll = target;
                world.markChanged<ScrollView>(entity);
                return true;
            }
        }
        entity = hierarchy::parentOf(world, entity);
    }
    return false;
}

} // namespace

bool activateFocused(ecs::World& world, ecs::Entity canvas)
{
    const PointerState* pointer = world.get<PointerState>(canvas);
    if (pointer == nullptr || !pointer->focused.valid() || !world.alive(pointer->focused))
    {
        return false;
    }
    const ComputedRect* rect = world.get<ComputedRect>(pointer->focused);
    if (rect == nullptr)
    {
        return false;
    }
    Event event{};
    event.kind = EventKind::Click;
    event.x = rect->x + rect->width * 0.5F;
    event.y = rect->y + rect->height * 0.5F;
    return dispatchEvent(world, canvas, event, pointer->focused);
}

bool capturePointer(ecs::World& world, ecs::Entity canvas, ecs::Entity entity)
{
    PointerState* pointer = world.get<PointerState>(canvas);
    if (pointer == nullptr)
    {
        return false;
    }
    if (pointer->capture != entity)
    {
        pointer->capture = entity;
        world.markChanged<PointerState>(canvas);
    }
    return true;
}

void releasePointer(ecs::World& world, ecs::Entity canvas)
{
    PointerState* pointer = world.get<PointerState>(canvas);
    if (pointer != nullptr && pointer->capture.valid())
    {
        pointer->capture = ecs::Entity{};
        world.markChanged<PointerState>(canvas);
    }
}

bool dragScroll(ecs::World& world, ecs::Entity canvas, float x, float y, float pixels)
{
    return scrollWalk(world, canvas, x, y,
                      [pixels](const ScrollView& view) { return view.scroll - pixels; });
}

bool dispatchEvent(ecs::World& world, ecs::Entity canvas, Event& event, ecs::Entity forced_target)
{
    if (!world.alive(canvas))
    {
        return false;
    }
    std::vector<ecs::Entity>& path = scratch();
    std::size_t count = 0U;
    const ecs::Entity target = forced_target.valid() ? forced_target
                                                     : eventTarget(world, canvas, event.x, event.y);
    if (target.valid())
    {
        collectPath(world, canvas, target, path);
        count = path.size();
    }

    /* Capture runs canvas first, target last; the target phase is that last
       entry; bubble runs the target's parent upward to the canvas. */
    if (count > 0U)
    {
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
    }

    if (event.kind == EventKind::Click && count > 0U)
    {
        /* The click's default actions, nearest widget first: a click inside an
           open dropdown popup selects that item row and closes the box, any
           other click on the box toggles it; a tab sets its container's page;
           a checkbox toggles. A handler that consumed the click kept it all. */
        const ecs::Entity target = path[count - 1U];
        for (std::size_t index = count; index > 0U; --index)
        {
            const ecs::Entity entity = path[index - 1U];
            if (Dropdown* dropdown = world.get<Dropdown>(entity))
            {
                for (const ecs::Entity child : hierarchy::children(world, entity))
                {
                    if (world.get<DropdownList>(child) == nullptr ||
                        !hierarchy::isAncestorOf(world, child, target))
                    {
                        continue;
                    }
                    std::uint32_t item = 0U;
                    for (const ecs::Entity row : hierarchy::children(world, child))
                    {
                        if (hierarchy::isAncestorOf(world, row, target))
                        {
                            dropdown->selected = item;
                            break;
                        }
                        ++item;
                    }
                    dropdown->open = false;
                    world.markChanged<Dropdown>(entity);
                    event.consumed = true;
                    return true;
                }
                dropdown->open = !dropdown->open;
                world.markChanged<Dropdown>(entity);
                event.consumed = true;
                return true;
            }
            if (const Tab* tab = world.get<Tab>(entity))
            {
                ecs::Entity parent = hierarchy::parentOf(world, entity);
                for (std::uint32_t depth = 0U; parent.valid() && depth <= hierarchy::kMaxDepth;
                     ++depth)
                {
                    if (Tabs* tabs = world.get<Tabs>(parent))
                    {
                        tabs->active = tab->index;
                        world.markChanged<Tabs>(parent);
                        event.consumed = true;
                        return true;
                    }
                    parent = hierarchy::parentOf(world, parent);
                }
                continue;
            }
            if (Checkbox* checkbox = world.get<Checkbox>(entity))
            {
                checkbox->checked = !checkbox->checked;
                world.markChanged<Checkbox>(entity);
                event.consumed = true;
                return true;
            }
            if (Selectable* selectable = world.get<Selectable>(entity))
            {
                /* Rows are one choice within their parent: selecting one
                   clears the rest. */
                const ecs::Entity parent = hierarchy::parentOf(world, entity);
                if (parent.valid())
                {
                    for (const ecs::Entity sibling : hierarchy::children(world, parent))
                    {
                        Selectable* other = world.get<Selectable>(sibling);
                        if (other != nullptr && other->selected)
                        {
                            other->selected = false;
                            world.markChanged<Selectable>(sibling);
                        }
                    }
                }
                if (!selectable->selected)
                {
                    selectable->selected = true;
                    world.markChanged<Selectable>(entity);
                }
                event.consumed = true;
                return true;
            }
        }
    }
    if (event.kind == EventKind::Wheel)
    {
        const float detents = event.wheel;
        if (scrollWalk(world, canvas, event.x, event.y, [detents](const ScrollView& view) {
                const float step = view.step > 0.0F ? view.step : kWheelPixels;
                return view.scroll + detents * step;
            }))
        {
            event.consumed = true;
            return true;
        }
    }
    return false;
}

} // namespace resonate::ui
