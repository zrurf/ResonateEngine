#include <resonate/core/blake3.h>

#include <cstring>

namespace resonate::hash
{

Digest blake3(const void* data, std::size_t size) noexcept
{
    ::blake3_hasher hasher;
    ::blake3_hasher_init(&hasher);
    ::blake3_hasher_update(&hasher, data, size);
    Digest digest{};
    ::blake3_hasher_finalize(&hasher, digest.data(), digest.size());
    return digest;
}

Digest blake3Keyed(const void* data, std::size_t size, const void* key,
                   std::size_t key_size) noexcept
{
    if (key == nullptr || key_size != BLAKE3_KEY_LEN)
    {
        return blake3(data, size);
    }
    ::blake3_hasher hasher;
    ::blake3_hasher_init_keyed(&hasher, static_cast<const std::uint8_t*>(key));
    ::blake3_hasher_update(&hasher, data, size);
    Digest digest{};
    ::blake3_hasher_finalize(&hasher, digest.data(), digest.size());
    return digest;
}

Hasher::Hasher() noexcept
{
    ::blake3_hasher_init(&hasher_);
}

Hasher::Hasher(const void* key, std::size_t key_size) noexcept
{
    if (key != nullptr && key_size == BLAKE3_KEY_LEN)
    {
        ::blake3_hasher_init_keyed(&hasher_, static_cast<const std::uint8_t*>(key));
        return;
    }
    ::blake3_hasher_init(&hasher_);
}

Hasher::~Hasher() noexcept = default;

void Hasher::update(const void* data, std::size_t size) noexcept
{
    ::blake3_hasher_update(&hasher_, data, size);
}

Digest Hasher::finalize() const noexcept
{
    Digest digest{};
    ::blake3_hasher_finalize(&hasher_, digest.data(), digest.size());
    return digest;
}

} // namespace resonate::hash
