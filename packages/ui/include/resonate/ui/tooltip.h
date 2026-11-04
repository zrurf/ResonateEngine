#ifndef RESONATE_UI_TOOLTIP_H
#define RESONATE_UI_TOOLTIP_H

#include <resonate/ecs/entity.h>
#include <resonate/ecs/world.h>

namespace resonate::ui
{

/* Shows the tooltip a hovered widget owns: a child node carrying Tooltip stays
   hidden until the pointer dwells on its parent for the component's delay, then
   is placed near the pointer, clamped to the canvas, and painted over the tree.
   One instance per world: bind once, advance every frame after the input sample
   and before layout. */
class TooltipSystem
{
  public:
    [[nodiscard]] bool bind(ecs::World& world);
    void advance(ecs::World& world, ecs::Entity canvas, float pointer_x, float pointer_y,
                 float delta_seconds);

  private:
    void hide(ecs::World& world);

    ecs::World* bound_world_ = nullptr;
    /* The hovered widget the dwell belongs to, and the tooltip it showed. */
    ecs::Entity owner_{};
    ecs::Entity shown_{};
    float dwell_ = 0.0F;
};

} // namespace resonate::ui

#endif /* RESONATE_UI_TOOLTIP_H */
