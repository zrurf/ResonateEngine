#include <resonate/ui/input.h>

#include <resonate/ui/hit_test.h>

#include <resonate/hierarchy/tree.h>

#include "state_style_internal.h"
#include "resonate.ui/components.gen.h"

namespace resonate::ui
{
namespace
{

void setState(ecs::World& world, ecs::Entity entity, WidgetState state)
{
    Input* input = world.get<Input>(entity);
    if (input == nullptr || input->state == state)
    {
        return;
    }
    input->state = state;
    world.markChanged<Input>(entity);
    detail::applyStateStyles(world, entity, static_cast<std::uint32_t>(state));
}

/* The nearest Input-bearing ancestor of the hit widget. A non-focusable Input
   answers no one, which also stops the walk: a disabled child must not hand the
   pointer to the widget above it. */
ecs::Entity interactiveAt(ecs::World& world, ecs::Entity canvas, float x, float y)
{
    ecs::Entity entity = hitTest(world, canvas, x, y);
    for (std::uint32_t depth = 0U; entity.valid() && depth <= hierarchy::kMaxDepth; ++depth)
    {
        const Input* input = world.get<Input>(entity);
        if (input != nullptr)
        {
            return input->focusable ? entity : ecs::Entity{};
        }
        entity = hierarchy::parentOf(world, entity);
    }
    return ecs::Entity{};
}

} // namespace

bool updateInput(ecs::World& world, ecs::Entity canvas, float x, float y, bool down)
{
    PointerState* pointer = world.get<PointerState>(canvas);
    if (pointer == nullptr)
    {
        world.report(ecs::World::Report::Error,
                     "ui input: entity %u:%u is not a canvas with PointerState", canvas.index,
                     canvas.generation);
        return false;
    }

    /* A release was reported once; clear it before this frame's sample so a
       reader sees it for exactly one update. */
    if (pointer->activated.valid())
    {
        if (world.alive(pointer->activated))
        {
            Input* input = world.get<Input>(pointer->activated);
            if (input != nullptr && input->activated)
            {
                input->activated = false;
                world.markChanged<Input>(pointer->activated);
            }
        }
        pointer->activated = ecs::Entity{};
        world.markChanged<PointerState>(canvas);
    }
    if (pointer->press.valid() && !world.alive(pointer->press))
    {
        pointer->press = ecs::Entity{};
        world.markChanged<PointerState>(canvas);
    }

    const ecs::Entity hover = interactiveAt(world, canvas, x, y);
    const ecs::Entity previous = pointer->hover;
    if (previous != hover)
    {
        pointer->hover = hover;
        world.markChanged<PointerState>(canvas);
        if (previous.valid() && world.alive(previous))
        {
            setState(world, previous, WidgetState::Normal);
        }
    }

    if (down && !pointer->down)
    {
        pointer->down = true;
        pointer->press = hover;
        world.markChanged<PointerState>(canvas);
        if (hover.valid())
        {
            setState(world, hover, WidgetState::Pressed);
        }
    }
    else if (!down && pointer->down)
    {
        pointer->down = false;
        world.markChanged<PointerState>(canvas);
        const ecs::Entity press = pointer->press;
        if (press.valid() && world.alive(press))
        {
            if (press == hover)
            {
                Input* input = world.get<Input>(press);
                if (input != nullptr)
                {
                    input->activated = true;
                    world.markChanged<Input>(press);
                    pointer->activated = press;
                    world.markChanged<PointerState>(canvas);
                }
            }
            setState(world, press, press == hover ? WidgetState::Hovered : WidgetState::Normal);
        }
        pointer->press = ecs::Entity{};
        world.markChanged<PointerState>(canvas);
    }

    if (pointer->down && pointer->press.valid())
    {
        setState(world, pointer->press,
                 pointer->press == hover ? WidgetState::Pressed : WidgetState::Normal);
    }
    if (hover.valid())
    {
        setState(world, hover,
                 pointer->down && pointer->press == hover ? WidgetState::Pressed
                                                          : WidgetState::Hovered);
    }
    return true;
}

} // namespace resonate::ui
