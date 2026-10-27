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

    /* A widget carrying Transition animates its Background colour instead of
       jumping: the target is recorded here and the tween system walks the
       colour over, starting from what is on screen. */
    const Transition* transition = world.get<Transition>(entity);
    const bool animates = transition != nullptr && transition->duration_ms > 0U;
    Vec4 tween_target{0.0F, 0.0F, 0.0F, 0.0F};
    bool wants_tween = false;

    /* A field the target state overrides is written once, straight to the state
       value; the others return to their normal value. */
    const auto apply = [&](const StateStyleRecord& record)
    {
        const auto component = static_cast<ecs::ComponentIndex>(record.component);
        if (animates && record.size == sizeof(Vec4) && record.component != 0U)
        {
            void* data = world.get(entity, component);
            const Background* background = world.get<Background>(entity);
            if (data != nullptr && background != nullptr &&
                record.offset == offsetof(Background, color))
            {
                std::memcpy(&tween_target, record.value, sizeof(Vec4));
                wants_tween = true;
                return;
            }
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

    /* The tween is attached after the record loop: attaching it moves the
       entity to another archetype, which would invalidate nothing held here but
       is cheaper done once. It starts from the colour on screen (a mid-tween
       change starts from where the last one got to). */
    if (wants_tween)
    {
        const Background* background = world.get<Background>(entity);
        if (background == nullptr)
        {
            return;
        }
        const float duration = static_cast<float>(transition->duration_ms) * 0.001F;
        ColorTween* tween = world.get<ColorTween>(entity);
        if (tween == nullptr)
        {
            (void)world.add(entity, ColorTween{background->color, tween_target, 0.0F, duration});
        }
        else
        {
            tween->from = background->color;
            tween->to = tween_target;
            tween->elapsed = 0.0F;
            tween->duration = duration;
            world.markChanged<ColorTween>(entity);
        }
    }
}

} // namespace resonate::ui::detail
