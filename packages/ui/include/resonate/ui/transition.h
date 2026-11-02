#ifndef RESONATE_UI_TRANSITION_H
#define RESONATE_UI_TRANSITION_H

#include <resonate/ecs/query.h>
#include <resonate/ecs/world.h>

namespace resonate::ui
{

/* Advances the animated state changes: a widget carrying ColorTransition tweens its
   Background colour when a state style changes it, instead of jumping. One
   instance per world: bind once, advance every frame before layout. */
class ColorTransitionSystem
{
  public:
    [[nodiscard]] bool bind(ecs::World& world);
    void advance(ecs::World& world, float delta_seconds);

  private:
    ecs::Query query_{};
    ecs::World* bound_world_ = nullptr;
};

} // namespace resonate::ui

#endif /* RESONATE_UI_TRANSITION_H */
