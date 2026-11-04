#include <resonate/ui/transition.h>

#include <algorithm>
#include <cstddef>
#include <vector>

#include "resonate.ui/components.gen.h"

namespace resonate::ui
{
namespace
{

/* The authored curve over the elapsed fraction: quadratic in, quadratic out,
   and the symmetric blend, matching CSS's ease-in/ease-out sense. */
float easeAmount(Ease curve, float t)
{
    t = std::clamp(t, 0.0F, 1.0F);
    switch (curve)
    {
        case Ease::In:
            return t * t;
        case Ease::Out:
            return 1.0F - (1.0F - t) * (1.0F - t);
        case Ease::InOut:
            return t < 0.5F ? 2.0F * t * t : 1.0F - 2.0F * (1.0F - t) * (1.0F - t);
        case Ease::Linear:
        default:
            return t;
    }
}

} // namespace

bool TransitionSystem::bind(ecs::World& world)
{
    if (bound_world_ == &world && query_.valid())
    {
        return true;
    }
    const ecs::ComponentIndex all[] = {ecs::ComponentTraits<TransitionState>::index};
    query_ = world.createQuery(ecs::QueryDesc{all, {}, {}});
    bound_world_ = &world;
    return query_.valid();
}

void TransitionSystem::advance(ecs::World& world, float delta_seconds)
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
                TransitionState* transition = state->world->get<TransitionState>(entity);
                if (transition == nullptr)
                {
                    state->finished.push_back(entity);
                    continue;
                }
                void* data = state->world->get(
                    entity, static_cast<ecs::ComponentIndex>(transition->component));
                if (data == nullptr)
                {
                    state->finished.push_back(entity);
                    continue;
                }
                transition->elapsed += state->delta;
                const float delay = transition->delay > 0.0F ? transition->delay : 0.0F;
                const float duration =
                    transition->duration > 0.0F ? transition->duration : 0.0F;
                /* Delay holds the old value; a zero duration lands on the
                   target the first frame past the delay. */
                float t = 0.0F;
                if (transition->elapsed > delay)
                {
                    t = duration > 0.0F
                            ? std::clamp((transition->elapsed - delay) / duration, 0.0F, 1.0F)
                            : 1.0F;
                }
                const float amount = easeAmount(transition->ease, t);
                const Vec4& from = transition->from;
                const Vec4& to = transition->to;
                auto* value = reinterpret_cast<Vec4*>(static_cast<std::byte*>(data) +
                                                      transition->offset);
                *value = Vec4{from.x + (to.x - from.x) * amount,
                              from.y + (to.y - from.y) * amount,
                              from.z + (to.z - from.z) * amount,
                              from.w + (to.w - from.w) * amount};
                state->world->markChanged(entity,
                                          static_cast<ecs::ComponentIndex>(transition->component));
                if (transition->elapsed >= delay + duration)
                {
                    state->finished.push_back(entity);
                }
            }
        },
        &update);

    /* Removing the finished transitions changes archetypes, so it happens after
       the chunk walk. */
    for (const ecs::Entity entity : update.finished)
    {
        if (world.alive(entity))
        {
            world.remove<TransitionState>(entity);
        }
    }
}

} // namespace resonate::ui
