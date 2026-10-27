#include <resonate/ui/transition.h>

#include <algorithm>
#include <vector>

#include "resonate.ui/components.gen.h"

namespace resonate::ui
{

bool ColorTransitionSystem::bind(ecs::World& world)
{
    if (bound_world_ == &world && query_.valid())
    {
        return true;
    }
    const ecs::ComponentIndex all[] = {ecs::ComponentTraits<ColorTween>::index};
    query_ = world.createQuery(ecs::QueryDesc{all, {}, {}});
    bound_world_ = &world;
    return query_.valid();
}

void ColorTransitionSystem::advance(ecs::World& world, float delta_seconds)
{
    if (bound_world_ != &world || !query_.valid() || !(delta_seconds > 0.0F))
    {
        return;
    }
    struct Update
    {
        ecs::World* world = nullptr;
        float delta = 0.0F;
        std::vector<ecs::Entity> finished;
    };
    Update update{&world, delta_seconds, {}};
    query_.forEachChunk(
        [](void* context, ecs::ChunkView view)
        {
            auto* state = static_cast<Update*>(context);
            for (std::uint32_t row = 0; row < view.count; ++row)
            {
                const ecs::Entity entity = view.entities[row];
                ColorTween* tween = state->world->get<ColorTween>(entity);
                Background* background = state->world->get<Background>(entity);
                if (tween == nullptr || background == nullptr)
                {
                    state->finished.push_back(entity);
                    continue;
                }
                tween->elapsed += state->delta;
                const float duration = tween->duration > 0.0F ? tween->duration : 0.0F;
                const float t =
                    duration > 0.0F ? std::clamp(tween->elapsed / duration, 0.0F, 1.0F) : 1.0F;
                background->color = Vec4{tween->from.x + (tween->to.x - tween->from.x) * t,
                                         tween->from.y + (tween->to.y - tween->from.y) * t,
                                         tween->from.z + (tween->to.z - tween->from.z) * t,
                                         tween->from.w + (tween->to.w - tween->from.w) * t};
                state->world->markChanged<Background>(entity);
                if (t >= 1.0F)
                {
                    state->finished.push_back(entity);
                }
            }
        },
        &update);

    /* Removing the finished tweens changes archetypes, so it happens after the
       chunk walk. */
    for (const ecs::Entity entity : update.finished)
    {
        if (world.alive(entity))
        {
            world.remove<ColorTween>(entity);
        }
    }
}

} // namespace resonate::ui
