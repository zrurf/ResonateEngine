#include <resonate/animation/player.h>

#include <resonate/hierarchy/tree.h>

namespace resonate::animation
{

bool AnimationPlayer::registerComponents(ecs::World& world)
{
    return world.registerComponent<AnimationState>() != ecs::kInvalidComponent;
}

std::uint32_t AnimationPlayer::addClip(Clip clip)
{
    clips_.push_back(std::move(clip));
    return static_cast<std::uint32_t>(clips_.size() - 1U);
}

const Clip* AnimationPlayer::clip(std::uint32_t index) const
{
    return index < clips_.size() ? &clips_[index] : nullptr;
}

void AnimationPlayer::invoke(ecs::World& world, float delta_seconds)
{
    const ecs::ComponentIndex states[] = {ecs::ComponentTraits<AnimationState>::index};
    ecs::QueryDesc desc{};
    desc.all = resonate::Span<const ecs::ComponentIndex>(states, 1U);
    ecs::Query query = world.createQuery(desc);
    if (!query.valid())
    {
        return;
    }
    struct Update
    {
        ecs::World* world = nullptr;
        AnimationPlayer* player = nullptr;
        float delta = 0.0F;
    };
    Update update{&world, this, delta_seconds};
    query.forEachChunk(
        [](void* context, ecs::ChunkView view)
        {
            auto* update = static_cast<Update*>(context);
            ecs::Column<AnimationState> states = ecs::column<AnimationState>(view);
            for (std::uint32_t row = 0; row < view.count; ++row)
            {
                AnimationState& state = states.data[row];
                if (!state.playing)
                {
                    continue;
                }
                const Clip* played = update->player->clip(state.clip);
                if (played == nullptr)
                {
                    continue;
                }
                state.time += update->delta * state.speed;
                if (!state.looping && played->duration > 0.0F && state.time > played->duration)
                {
                    state.time = played->duration;
                    state.playing = false;
                }
                const ecs::Entity entity = view.entities[row];
                if (hierarchy::LocalTransform* transform =
                        update->world->get<hierarchy::LocalTransform>(entity))
                {
                    played->sample(state.time, state.looping, *transform);
                    (void)update->world->markChanged<hierarchy::LocalTransform>(entity);
                }
            }
        },
        &update);
}

} // namespace resonate::animation
