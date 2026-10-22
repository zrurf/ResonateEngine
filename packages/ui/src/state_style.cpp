#include "state_style_internal.h"

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

    /* A field the target state overrides is written once, straight to the state
       value; the others return to their normal value. */
    const auto apply = [&](const StateStyleRecord& record)
    {
        void* data = world.get(entity, static_cast<ecs::ComponentIndex>(record.component));
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
        world.markChanged(entity, static_cast<ecs::ComponentIndex>(record.component));
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
}

} // namespace resonate::ui::detail
