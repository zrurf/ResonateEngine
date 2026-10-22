#ifndef RESONATE_UI_BINDING_INTERNAL_H
#define RESONATE_UI_BINDING_INTERNAL_H

#include <cstdint>

#include <resonate/ecs/entity.h>

namespace resonate::ui::detail
{

/* One compiled binding. Records live in the canvas's Bindings blob; `kind` is
   the value type (0 = f32) and the offsets are byte offsets into the source and
   target component payloads. */
struct BindingRecord
{
    ecs::Entity source;
    ecs::Entity target;
    std::uint32_t source_component = 0;
    std::uint32_t target_component = 0;
    std::uint32_t source_offset = 0;
    std::uint32_t target_offset = 0;
    std::uint32_t kind = 0;
    std::uint32_t padding = 0;
};

} // namespace resonate::ui::detail

#endif /* RESONATE_UI_BINDING_INTERNAL_H */
