#ifndef RESONATE_ECS_BLOB_H
#define RESONATE_ECS_BLOB_H

#include <cstdint>

namespace resonate::ecs
{

/* A handle to an entity-owned blob: an untyped, variable-length buffer that
   lives outside the chunks. Generation-tagged like an entity handle, so a
   handle to a reclaimed blob fails instead of reading another blob's bytes;
   the analogue of law 6 is that the bytes' address is only stable until
   something structural happens. */
struct BlobHandle
{
    std::uint32_t index = 0;
    std::uint32_t generation = 0;

    [[nodiscard]] constexpr bool valid() const noexcept
    {
        return generation != 0;
    }

    friend constexpr bool operator==(const BlobHandle& a, const BlobHandle& b) noexcept
    {
        return a.index == b.index && a.generation == b.generation;
    }
};

} // namespace resonate::ecs

#endif /* RESONATE_ECS_BLOB_H */
