#include <resonate/ui/tooltip.h>

#include <algorithm>

#include <resonate/hierarchy/tree.h>

#include "resonate.ui/components.gen.h"

namespace resonate::ui
{
namespace
{

/* The first child of `owner` that marks itself a tooltip; a second would be
   ambiguous and is ignored. */
ecs::Entity tooltipOf(ecs::World& world, ecs::Entity owner)
{
    for (const ecs::Entity child : hierarchy::children(world, owner))
    {
        if (world.get<Tooltip>(child) != nullptr)
        {
            return child;
        }
    }
    return ecs::Entity{};
}

/* Target pixels per layout unit; the canvas is laid out at target size divided
   by its scale. */
float canvasScale(ecs::World& world, ecs::Entity canvas)
{
    const Canvas* component = world.get<Canvas>(canvas);
    return component != nullptr && component->scale > 0.0F ? component->scale : 1.0F;
}

} // namespace

bool TooltipSystem::bind(ecs::World& world)
{
    bound_world_ = &world;
    owner_ = ecs::Entity{};
    shown_ = ecs::Entity{};
    dwell_ = 0.0F;
    return true;
}

void TooltipSystem::hide(ecs::World& world)
{
    if (shown_.valid() && world.alive(shown_))
    {
        if (Shown* shown = world.get<Shown>(shown_))
        {
            if (shown->shown)
            {
                shown->shown = false;
                world.markChanged<Shown>(shown_);
            }
        }
    }
    shown_ = ecs::Entity{};
}

void TooltipSystem::advance(ecs::World& world, ecs::Entity canvas, float pointer_x, float pointer_y,
                            float delta_seconds)
{
    if (bound_world_ != &world || !canvas.valid() || !world.alive(canvas))
    {
        return;
    }
    const PointerState* pointer = world.get<PointerState>(canvas);
    /* A press drags or activates: a tooltip must not appear under the finger. */
    const ecs::Entity owner =
        pointer != nullptr && !pointer->down ? pointer->hover : ecs::Entity{};
    const ecs::Entity tooltip = owner.valid() ? tooltipOf(world, owner) : ecs::Entity{};
    if (!tooltip.valid())
    {
        hide(world);
        owner_ = ecs::Entity{};
        dwell_ = 0.0F;
        return;
    }
    if (owner != owner_)
    {
        hide(world);
        owner_ = owner;
        dwell_ = 0.0F;
    }
    dwell_ += delta_seconds > 0.0F ? delta_seconds : 0.0F;
    const Tooltip* tip = world.get<Tooltip>(tooltip);
    if (tip == nullptr || dwell_ < static_cast<float>(tip->delay_ms) * 0.001F)
    {
        return;
    }

    /* Showing is a structural change the first time; the transform is fetched
       afterwards, when nothing can move the entity any more. */
    Shown* shown = world.get<Shown>(tooltip);
    if (shown == nullptr)
    {
        shown = static_cast<Shown*>(world.add(tooltip, Shown{true}));
        if (shown == nullptr)
        {
            return;
        }
    }
    else if (!shown->shown)
    {
        shown->shown = true;
        world.markChanged<Shown>(tooltip);
    }
    shown_ = tooltip;

    RectTransform* transform = world.get<RectTransform>(tooltip);
    if (transform == nullptr)
    {
        return;
    }
    /* The box in layout units: an authored size wins, a zero one takes what the
       last layout measured. The pointer arrives in target pixels, like the
       input sample, so it enters layout space the same way. */
    const float scale = canvasScale(world, canvas);
    const float pointer_layout_x = pointer_x / scale;
    const float pointer_layout_y = pointer_y / scale;
    float width = transform->width;
    float height = transform->height;
    if (const ComputedRect* rect = world.get<ComputedRect>(tooltip))
    {
        if (!(width > 0.0F))
        {
            width = rect->width / scale;
        }
        if (!(height > 0.0F))
        {
            height = rect->height / scale;
        }
    }
    float x = pointer_layout_x + tip->offset_x;
    float y = pointer_layout_y + tip->offset_y;
    if (const ComputedRect* view = world.get<ComputedRect>(canvas))
    {
        const float view_width = view->width / scale;
        const float view_height = view->height / scale;
        x = view_width > width ? std::clamp(x, 0.0F, view_width - width) : 0.0F;
        y = view_height > height ? std::clamp(y, 0.0F, view_height - height) : 0.0F;
    }

    /* Layout places a child from its parent's origin: the authored anchors are
       zero, so the local offset is the difference from the parent's corner. */
    const ecs::Entity parent = hierarchy::parentOf(world, tooltip);
    float parent_x = 0.0F;
    float parent_y = 0.0F;
    if (const ComputedRect* box = parent.valid() ? world.get<ComputedRect>(parent) : nullptr)
    {
        parent_x = box->x / scale;
        parent_y = box->y / scale;
    }
    transform->x = x - parent_x + transform->pivot.x * width;
    transform->y = y - parent_y + transform->pivot.y * height;
    world.markChanged<RectTransform>(tooltip);
}

} // namespace resonate::ui
