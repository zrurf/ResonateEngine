#ifndef RESONATE_CORE_BLAKE3_H
#define RESONATE_CORE_BLAKE3_H

/*
 * The engine's BLAKE3 surface: the hash a cook and a runtime share for content
 * addressing. The unkeyed form is the plain hash; the keyed form is what a
 * product's salt selects, so a chunk table from one work is useless for
 * another. The stream form is for files too large to hold at once.
 */

#include <array>
#include <cstddef>
#include <cstdint>

#include <blake3.h>

namespace resonate::hash
{

using Digest = std::array<std::uint8_t, 32>;

[[nodiscard]] Digest blake3(const void* data, std::size_t size) noexcept;
[[nodiscard]] Digest blake3Keyed(const void* data, std::size_t size, const void* key,
                                 std::size_t key_size) noexcept;

/* The hash of the empty input, so a caller with no data has the same answer
   either way. */
inline constexpr Digest kEmptyDigest{0xafU, 0x13U, 0x49U, 0xb9U, 0xf5U, 0xf9U, 0xa1U, 0xa6U,
                                     0xa0U, 0x40U, 0x4dU, 0xeaU, 0x36U, 0xdcU, 0xc9U, 0x49U,
                                     0x9bU, 0xcbU, 0x25U, 0xc9U, 0xadU, 0xc1U, 0x12U, 0xb7U,
                                     0xccU, 0x9aU, 0x93U, 0xcaU, 0xe4U, 0x1fU, 0x32U, 0x62U};

class Hasher
{
  public:
    Hasher() noexcept;
    /* A 32-byte key is the keyed mode; another size is refused and leaves the
       hasher unkeyed. */
    Hasher(const void* key, std::size_t key_size) noexcept;
    ~Hasher() noexcept;

    Hasher(const Hasher&) = delete;
    Hasher& operator=(const Hasher&) = delete;

    void update(const void* data, std::size_t size) noexcept;
    [[nodiscard]] Digest finalize() const noexcept;

  private:
    ::blake3_hasher hasher_;
};

} // namespace resonate::hash

#endif /* RESONATE_CORE_BLAKE3_H */
