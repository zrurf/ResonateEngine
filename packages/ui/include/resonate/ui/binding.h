#ifndef RESONATE_UI_BINDING_H
#define RESONATE_UI_BINDING_H

#include <resonate/ecs/entity.h>
#include <resonate/ecs/world.h>

namespace resonate::ui
{

/* Copies every bound source field into its target and marks the target
   component changed when the value moved. Call it after gameplay wrote the
   source values and before layout. */
void updateBindings(ecs::World& world, ecs::Entity canvas);

} // namespace resonate::ui

#endif /* RESONATE_UI_BINDING_H */
