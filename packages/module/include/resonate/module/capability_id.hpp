#ifndef RESONATE_MODULE_CAPABILITY_ID_HPP
#define RESONATE_MODULE_CAPABILITY_ID_HPP

#include <cstdint>
#include <cstring>

/*
 * XXH3, header-only. Inline rather than linked so a plugin still gets its ids
 * from the headers alone: the whole point of this tier is that a plugin links no
 * library and can be built by a different compiler than the host.
 */
#define XXH_INLINE_ALL
#include <xxhash.h>

#include "resonate/module/abi.h"

namespace resonate
{

/* A capability id: XXH3-128 of a name such as "Resonate.Physics.World", equal to
   resonate_id_make of the same string.
 *
 * Not constexpr: XXH3 is a library function and no part of it is usable in a
 * constant expression. That is the trade for having one implementation instead
 * of a hand-written hash that a second hand-written hash had to match. */
class Id
{
  public:
    Id(const char* name)
    {
        const XXH128_hash_t hash = XXH3_128bits(name, std::strlen(name));
        lo_ = hash.low64;
        hi_ = hash.high64;
    }

    /* For an id that arrives already hashed, such as a manifest entry read at run
       time. */
    constexpr Id(ResonateId value) noexcept : lo_(value.lo), hi_(value.hi)
    {
    }

    [[nodiscard]] constexpr ResonateId value() const noexcept
    {
        return ResonateId{lo_, hi_};
    }

    friend constexpr bool operator==(const Id& a, const Id& b) noexcept
    {
        return a.lo_ == b.lo_ && a.hi_ == b.hi_;
    }

  private:
    std::uint64_t lo_ = 0;
    std::uint64_t hi_ = 0;
};

namespace detail
{

/* Left undefined; a capability header must specialise it, or the capability has
   no stable name and will not compile. */
template <typename T> struct CapabilityTraits;

} // namespace detail
} // namespace resonate

#endif /* RESONATE_MODULE_CAPABILITY_ID_HPP */
