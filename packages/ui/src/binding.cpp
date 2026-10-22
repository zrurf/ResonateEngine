#include <resonate/ui/binding.h>

#include <cstddef>
#include <cstring>

#include "binding_internal.h"
#include "resonate.ui/components.gen.h"

namespace resonate::ui
{

void updateBindings(ecs::World& world, ecs::Entity canvas)
{
    const Bindings* bindings = world.get<Bindings>(canvas);
    if (bindings == nullptr || !world.alive(bindings->records))
    {
        return;
    }
    const auto* records = static_cast<const detail::BindingRecord*>(world.blobData(bindings->records));
    if (records == nullptr)
    {
        return;
    }
    const std::size_t count = world.blobSize(bindings->records) / sizeof(detail::BindingRecord);
    for (std::size_t index = 0; index < count; ++index)
    {
        const detail::BindingRecord& record = records[index];
        if (record.kind != 0U)
        {
            continue;
        }
        const void* source =
            world.get(record.source, static_cast<ecs::ComponentIndex>(record.source_component));
        void* target =
            world.get(record.target, static_cast<ecs::ComponentIndex>(record.target_component));
        if (source == nullptr || target == nullptr)
        {
            continue;
        }

        const auto* source_bytes = static_cast<const std::byte*>(source) + record.source_offset;
        auto* target_bytes = static_cast<std::byte*>(target) + record.target_offset;
        float value = 0.0F;
        float previous = 0.0F;
        std::memcpy(&value, source_bytes, sizeof(value));
        std::memcpy(&previous, target_bytes, sizeof(previous));
        if (value == previous)
        {
            continue;
        }
        std::memcpy(target_bytes, &value, sizeof(value));
        world.markChanged(record.target, static_cast<ecs::ComponentIndex>(record.target_component));
    }
}

} // namespace resonate::ui
