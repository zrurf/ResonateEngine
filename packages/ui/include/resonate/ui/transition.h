#ifndef RESONATE_UI_TRANSITION_H
#define RESONATE_UI_TRANSITION_H

#include <resonate/ecs/query.h>
#include <resonate/ecs/world.h>

namespace resonate::ui
{

/* Advances the animated state changes: a widget carrying Transition walks the
   property a state style overrode from the value on screen to the state's
   value, after the authored delay and along its easing curve. One instance per
   world: bind once, advance every frame before layout. */
class TransitionSystem
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
