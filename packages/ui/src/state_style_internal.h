#ifndef RESONATE_UI_STATE_STYLE_INTERNAL_H
#define RESONATE_UI_STATE_STYLE_INTERNAL_H

#include <cstddef>
#include <cstdint>

#include <resonate/ecs/entity.h>
#include <resonate/ecs/world.h>

namespace resonate::ui::detail
{

/* One compiled state-style value. A widget's StateStyles blob holds records
   for every field any state rule touches: state 0 restores the normal value,
   1..3 are hovered, pressed and disabled. `value` covers exactly `size` bytes;
   16 is a vec4, the largest value a style currently declares. */
struct StateStyleRecord
{
    std::uint32_t component = 0;
    std::uint32_t offset = 0;
    std::uint32_t size = 0;
    std::uint32_t state = 0;
    std::byte value[16]{};
};

inline constexpr std::uint32_t kStateStyleValueSize = sizeof(StateStyleRecord::value);

/* Writes the records for `state`, after restoring the normal values, and marks
   every component whose bytes moved. A widget without StateStyles is a no-op. */
void applyStateStyles(ecs::World& world, ecs::Entity entity, std::uint32_t state);

} // namespace resonate::ui::detail

#endif /* RESONATE_UI_STATE_STYLE_INTERNAL_H */
