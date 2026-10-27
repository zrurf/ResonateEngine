#ifndef RESONATE_ECS_COMPONENT_H
#define RESONATE_ECS_COMPONENT_H

#include <cstdint>

namespace resonate::ecs
{

/* Dense index of a registered component type: what archetype masks and chunk
   layouts are built from. Assigned by registration, valid until the world dies. */
using ComponentIndex = std::uint16_t;

inline constexpr ComponentIndex kInvalidComponent = 0xFFFF;

/* Archetype and query masks are a fixed set of u64 words: the component
   universe holds kMaxComponentTypes types, registering past the last bit is
   refused rather than silently truncating the mask, and the registration path
   warns once the universe passes three quarters of it. 512 types is well past
   any game's vocabulary; a design with no cap at all replaces the mask with
   sorted component lists and a merge walk, at a per-archetype matching cost the
   bench would have to clear first. */
inline constexpr std::uint32_t kComponentMaskWords = 8U;
inline constexpr std::uint32_t kMaxComponentTypes = kComponentMaskWords * 64U;

/* The set of component types an archetype stores or a query names. */
struct ComponentMask
{
    std::uint64_t words[kComponentMaskWords] = {};

    static constexpr std::uint32_t wordOf(ComponentIndex component) noexcept
    {
        return component >> 6U;
    }

    static constexpr std::uint64_t bitOf(ComponentIndex component) noexcept
    {
        return std::uint64_t{1} << (component & 63U);
    }

    constexpr void set(ComponentIndex component) noexcept
    {
        words[wordOf(component)] |= bitOf(component);
    }

    constexpr void clear(ComponentIndex component) noexcept
    {
        words[wordOf(component)] &= ~bitOf(component);
    }

    [[nodiscard]] constexpr bool has(ComponentIndex component) const noexcept
    {
        return (words[wordOf(component)] & bitOf(component)) != 0U;
    }

    [[nodiscard]] constexpr bool empty() const noexcept
    {
        for (std::uint32_t word = 0U; word < kComponentMaskWords; ++word)
        {
            if (words[word] != 0U)
            {
                return false;
            }
        }
        return true;
    }

    friend constexpr bool operator==(const ComponentMask&, const ComponentMask&) noexcept =
        default;
};

/* True when `mask` contains every bit of `required`. */
constexpr bool containsAll(const ComponentMask& mask, const ComponentMask& required) noexcept
{
    for (std::uint32_t word = 0U; word < kComponentMaskWords; ++word)
    {
        if ((mask.words[word] & required.words[word]) != required.words[word])
        {
            return false;
        }
    }
    return true;
}

/* True when the two masks share at least one bit. */
constexpr bool intersects(const ComponentMask& left, const ComponentMask& right) noexcept
{
    for (std::uint32_t word = 0U; word < kComponentMaskWords; ++word)
    {
        if ((left.words[word] & right.words[word]) != 0U)
        {
            return true;
        }
    }
    return false;
}

/* True when `mask` contains no bit of `excluded`. */
constexpr bool containsNone(const ComponentMask& mask, const ComponentMask& excluded) noexcept
{
    return !intersects(mask, excluded);
}

namespace detail
{

/* What a component type gets unless its traits say otherwise: the type's own
   layout, and the index registration assigns. A specialisation derives from
   this, because specialising a class template replaces it whole — the members
   not restated would otherwise disappear. */
template <typename T> struct ComponentDefaults
{
    static constexpr std::uint32_t size = sizeof(T);
    static constexpr std::uint32_t alignment = alignof(T);

    /* Assigned by the first World::registerComponent<T>() in the process. The
       component universe is shared by every world — a schema-owned type has one
       identity — while which types a particular world registered is that
       world's own state. */
    static inline ComponentIndex index = kInvalidComponent;
};

} // namespace detail

/* A component type describes its own storage. Size and alignment default to the
   type's own layout; the index is filled by World::registerComponent<T>() and
   read by the typed accessors. Using a type that was never registered, or whose
   registration failed, is reported and fails rather than touching an unrelated
   type. */
template <typename T> struct ComponentTraits : detail::ComponentDefaults<T>
{
    /* The stable name is what a component is addressed by outside C++ (tools,
       serialisation, the C# side); a type without one does not compile into the
       ECS. Borrowed: it must outlive the world, which is what a string literal
       or a generated constant does. */
    static constexpr const char* name = nullptr;
};

/* Names a component type. The schema compiler emits these; a hand-written
   component uses the macro so the layout and index stay in one place. It goes
   inside `namespace resonate::ecs`, beside the specialisations the rest of the
   engine writes the same way; a type with a layout the storage cannot use
   overrides size/alignment itself. */
#define RESONATE_COMPONENT(TYPE, NAME)                                                             \
    template <> struct ComponentTraits<TYPE> : detail::ComponentDefaults<TYPE>                     \
    {                                                                                              \
        static constexpr const char* name = NAME;                                                  \
    }

} // namespace resonate::ecs

#endif /* RESONATE_ECS_COMPONENT_H */
