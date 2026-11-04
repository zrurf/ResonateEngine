#include "state_style_internal.h"

#include <cstddef>
#include <cstring>

#include <resonate/ecs/world.h>

#include "resonate.ui/components.gen.h"

namespace resonate::ui::detail
{

void applyStateStyles(ecs::World& world, ecs::Entity entity, std::uint32_t state)
{
    const StateStyles* styles = world.get<StateStyles>(entity);
    if (styles == nullptr || !world.alive(styles->records))
    {
        return;
    }
    const auto* records = static_cast<const StateStyleRecord*>(world.blobData(styles->records));
    if (records == nullptr)
    {
        return;
    }
    const std::size_t count = world.blobSize(styles->records) / sizeof(StateStyleRecord);

    /* A widget carrying Transition animates the first vec4 property a state
       override changes instead of jumping: the target is recorded here and the
       transition system walks the value over, starting from what is on
       screen. The record itself is not written, so the old value holds until
       the transition starts. */
    const Transition* transition = world.get<Transition>(entity);
    const bool animates = transition != nullptr && transition->duration_ms > 0U;
    ecs::ComponentIndex tween_component = ecs::kInvalidComponent;
    std::uint32_t tween_offset = 0U;
    Vec4 tween_target{0.0F, 0.0F, 0.0F, 0.0F};
    bool wants_tween = false;

    /* A field the target state overrides is written once, straight to the state
       value; the others return to their normal value. */
    const auto apply = [&](const StateStyleRecord& record)
    {
        const auto component = static_cast<ecs::ComponentIndex>(record.component);
        if (animates && !wants_tween && record.component != 0U &&
            record.size == sizeof(Vec4) && world.get(entity, component) != nullptr)
        {
            std::memcpy(&tween_target, record.value, sizeof(Vec4));
            tween_component = component;
            tween_offset = record.offset;
            wants_tween = true;
            return;
        }
        void* data = world.get(entity, component);
        if (data == nullptr)
        {
            return;
        }
        auto* bytes = static_cast<std::byte*>(data) + record.offset;
        if (std::memcmp(bytes, record.value, record.size) == 0)
        {
            return;
        }
        std::memcpy(bytes, record.value, record.size);
        world.markChanged(entity, component);
    };

    for (std::size_t index = 0; index < count; ++index)
    {
        const StateStyleRecord& record = records[index];
        if (record.state != 0U)
        {
            continue;
        }
        bool overridden = false;
        if (state != 0U)
        {
            for (std::size_t candidate = 0; candidate < count; ++candidate)
            {
                const StateStyleRecord& other = records[candidate];
                if (other.state == state && other.component == record.component &&
                    other.offset == record.offset)
                {
                    overridden = true;
                    break;
                }
            }
        }
        if (!overridden)
        {
            apply(record);
        }
    }
    if (state != 0U)
    {
        for (std::size_t index = 0; index < count; ++index)
        {
            if (records[index].state == state)
            {
                apply(records[index]);
            }
        }
    }

    /* The transition is attached after the record loop: attaching it moves the
       entity to another archetype, and retargeting keeps `from` at what is on
       screen, so a state change mid-tween does not jump. */
    if (wants_tween)
    {
        void* data = world.get(entity, tween_component);
        if (data == nullptr)
        {
            return;
        }
        const Vec4 current =
            *reinterpret_cast<const Vec4*>(static_cast<const std::byte*>(data) + tween_offset);
        const float duration = static_cast<float>(transition->duration_ms) * 0.001F;
        const float delay = static_cast<float>(transition->delay_ms) * 0.001F;
        TransitionState* tween = world.get<TransitionState>(entity);
        if (tween == nullptr)
        {
            (void)world.add(entity, TransitionState{tween_component, tween_offset, current,
                                                    tween_target, 0.0F, duration, delay,
                                                    transition->ease});
        }
        else
        {
            tween->component = tween_component;
            tween->offset = tween_offset;
            tween->from = current;
            tween->to = tween_target;
            tween->elapsed = 0.0F;
            tween->duration = duration;
            tween->delay = delay;
            tween->ease = transition->ease;
            world.markChanged<TransitionState>(entity);
        }
    }
}

} // namespace resonate::ui::detail
