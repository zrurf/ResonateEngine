#ifndef RESONATE_UI_BINDING_INTERNAL_H
#define RESONATE_UI_BINDING_INTERNAL_H

#include <cstdint>
#include <string>
#include <string_view>

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

/* A reference a document authored by key, waiting for the node it names: the
   adapter records it while the tree is built and resolves it once every node
   (and the key index) exists. */
struct PendingReference
{
    enum class Kind : std::uint8_t
    {
        SplitterTarget,
    };

    Kind kind = Kind::SplitterTarget;
    ecs::Entity entity{};
    /* Copied: the component element a field is read from may be a resolved
       temporary, so a view into it would dangle. */
    std::string key{};
};

} // namespace resonate::ui::detail

#endif /* RESONATE_UI_BINDING_INTERNAL_H */
