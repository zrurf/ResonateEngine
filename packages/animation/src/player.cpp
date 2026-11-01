#include <resonate/animation/player.h>

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include "resonate.animation/components.gen.h"

#include <resonate/hierarchy/tree.h>

namespace resonate::animation
{

bool AnimationPlayer::registerComponents(ecs::World& world)
{
    bool ok = world.registerComponent<AnimationState>() != ecs::kInvalidComponent;
    ok = world.registerComponent<Animation>() != ecs::kInvalidComponent && ok;
    ok = world.registerComponent<MachineState>() != ecs::kInvalidComponent && ok;
    return ok;
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

std::uint32_t AnimationPlayer::addMachine(StateMachine machine)
{
    machines_.push_back(std::move(machine));
    return static_cast<std::uint32_t>(machines_.size() - 1U);
}

const StateMachine* AnimationPlayer::machine(std::uint32_t index) const
{
    return index < machines_.size() ? &machines_[index] : nullptr;
}

void AnimationPlayer::advanceMachines(ecs::World& world, float delta_seconds)
{
    const ecs::ComponentIndex components[] = {ecs::ComponentTraits<MachineState>::index};
    ecs::QueryDesc desc{};
    desc.all = resonate::Span<const ecs::ComponentIndex>(components, 1U);
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
            ecs::Column<MachineState> states = ecs::column<MachineState>(view);
            for (std::uint32_t row = 0; row < view.count; ++row)
            {
                MachineState& state = states.data[row];
                const StateMachine* definition = update->player->machine(state.machine);
                if (definition == nullptr || definition->states.empty())
                {
                    continue;
                }
                const ecs::Entity entity = view.entities[row];
                AnimationState* playback = update->world->get<AnimationState>(entity);
                if (playback == nullptr || !playback->playing)
                {
                    continue;
                }
                advance(*definition, state.runtime, update->delta);
                const MachineRuntime& runtime = state.runtime;
                const State& current = definition->states[runtime.current];
                playback->clip = current.clip;
                playback->looping = current.looping;
                playback->time = runtime.time;
                playback->previous_clip = runtime.previous < definition->states.size()
                                              ? definition->states[runtime.previous].clip
                                              : current.clip;
                playback->previous_looping =
                    runtime.previous < definition->states.size()
                        ? definition->states[runtime.previous].looping
                        : current.looping;
                playback->previous_time = runtime.previous_time;
                playback->blend = blendWeight(runtime);
                playback->driven = true;
            }
        },
        &update);
}

bool AnimationPlayer::loadManifest(const std::string& path, std::string& error)
{
    if (!identity_.loadFile(path, error))
    {
        error = path + ": " + error;
        return false;
    }
    /* A product's path is relative to the manifest's own directory. */
    const std::size_t separator = path.find_last_of("/\\");
    manifest_directory_ =
        separator == std::string::npos ? std::string() : path.substr(0U, separator);
    return true;
}

bool AnimationPlayer::clipFor(const asset::AssetId& id, std::uint32_t& out_index,
                              std::string& error)
{
    const std::string key = std::to_string(id.guid_low) + ":" + std::to_string(id.guid_high) +
                            ":" + std::to_string(id.sub_id);
    const auto cached = authored_.find(key);
    if (cached != authored_.end())
    {
        out_index = cached->second;
        return true;
    }
    const std::optional<std::string> located = identity_.locate(id);
    if (!located.has_value())
    {
        error = "the manifest does not know this clip id";
        return false;
    }
    const std::string full = manifest_directory_.empty()
                                 ? *located
                                 : manifest_directory_ + "/" + *located;
    Clip clip;
    if (!Clip::loadFile(full, clip, error))
    {
        error = full + ": " + error;
        return false;
    }
    const std::uint32_t index = addClip(std::move(clip));
    authored_.emplace(key, index);
    out_index = index;
    return true;
}

void AnimationPlayer::materialize(ecs::World& world)
{
    const ecs::ComponentIndex authored[] = {ecs::ComponentTraits<Animation>::index};
    ecs::QueryDesc desc{};
    desc.all = resonate::Span<const ecs::ComponentIndex>(authored, 1U);
    ecs::Query query = world.createQuery(desc);
    if (!query.valid())
    {
        return;
    }
    /* The walk only collects: adding AnimationState moves the entity to a new
       archetype, and the world refuses a structural change inside a query. */
    struct Authored
    {
        ecs::Entity entity;
        Animation component;
    };
    std::vector<Authored> pending;
    query.forEachChunk(
        [](void* context, ecs::ChunkView view)
        {
            auto* collected = static_cast<std::vector<Authored>*>(context);
            const ecs::Column<Animation> components = ecs::column<Animation>(view);
            for (std::uint32_t row = 0; row < view.count; ++row)
            {
                collected->push_back({view.entities[row], components.data[row]});
            }
        },
        &pending);
    for (const Authored& record : pending)
    {
        if (world.get<AnimationState>(record.entity) != nullptr)
        {
            continue;
        }
        if (record.component.clip_low == 0U && record.component.clip_high == 0U)
        {
            continue; /* The node composition's default: no reference, no state. */
        }
        const asset::AssetId id{record.component.clip_low, record.component.clip_high, 1U};
        std::uint32_t index = 0U;
        std::string error;
        if (!clipFor(id, index, error))
        {
            const std::string key = std::to_string(id.guid_low) + ":" +
                                    std::to_string(id.guid_high) + ":" +
                                    std::to_string(id.sub_id);
            if (failed_.insert(key).second)
            {
                world.report(ecs::World::Report::Warning, "animation: %s", error.c_str());
            }
            continue;
        }
        AnimationState state;
        state.clip = index;
        state.looping = record.component.looping;
        state.speed = record.component.speed;
        world.add(record.entity, state);
    }
}

void AnimationPlayer::invoke(ecs::World& world, float delta_seconds)
{
    materialize(world);
    advanceMachines(world, delta_seconds);

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
                if (!state.driven)
                {
                    state.time += update->delta * state.speed;
                    if (!state.looping && played->duration > 0.0F &&
                        state.time > played->duration)
                    {
                        state.time = played->duration;
                        state.playing = false;
                    }
                }
                const ecs::Entity entity = view.entities[row];
                hierarchy::LocalTransform* transform =
                    update->world->get<hierarchy::LocalTransform>(entity);
                if (transform == nullptr)
                {
                    continue;
                }
                const Clip* outgoing = update->player->clip(state.previous_clip);
                if (state.blend < 1.0F && outgoing != nullptr && state.previous_clip != state.clip)
                {
                    Clip::blend(*outgoing, *played, state.blend, state.previous_time, state.time,
                                state.previous_looping, state.looping, *transform);
                }
                else
                {
                    played->sample(state.time, state.looping, *transform);
                }
                (void)update->world->markChanged<hierarchy::LocalTransform>(entity);
            }
        },
        &update);
}

} // namespace resonate::animation
