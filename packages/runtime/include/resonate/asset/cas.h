#ifndef RESONATE_ASSET_CAS_H
#define RESONATE_ASSET_CAS_H

/*
 * The local content-addressed chunk store: a product's bytes split into
 * content-defined chunks, each named by its keyed BLAKE3 hash. A manifest is
 * the ordered chunk list that reassembles one product; an unchanged chunk is
 * shared across versions, which is what makes a patch a list of missing
 * chunks.
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <resonate/core/blake3.h>
#include <resonate/core/span.h>

namespace resonate::asset
{

/* The salt a work's chunks are hashed with. The cook and the runtime must be
   given the same key; a different key reads nothing. */
using ContentKey = std::array<std::uint8_t, 32>;

inline constexpr std::size_t kChunkMinBytes = 16U * 1024U;
inline constexpr std::size_t kChunkMaxBytes = 256U * 1024U;
/* A chunk ends where these low bits of the rolling hash are zero: about 64 KiB
   between cuts. */
inline constexpr std::uint64_t kChunkBoundaryMask = 0xFFFFU;

struct ChunkRef
{
    hash::Digest hash{};
    std::uint32_t size = 0U;
};

struct CasManifest
{
    std::uint64_t total_bytes = 0U;
    std::vector<ChunkRef> chunks;
};

/* The chunks of `data`, in order; the spans borrow it. Boundaries depend only
   on the bytes, so an edit moves at most the chunks around it. */
[[nodiscard]] std::vector<Span<const std::byte>> splitChunks(Span<const std::byte> data);

/* Writes the chunks under `<root>/chunks/<xx>/<hex>`; a chunk already there is
   left alone. Fills `out_manifest`; `error` names the file that failed. */
[[nodiscard]] bool storeBuffer(const std::string& root, const ContentKey& key,
                               Span<const std::byte> data, CasManifest& out_manifest,
                               std::string& error);

/* Reads the manifest's chunks into `out`, verifying each keyed hash. */
[[nodiscard]] bool readBuffer(const std::string& root, const ContentKey& key,
                              const CasManifest& manifest, std::vector<std::byte>& out,
                              std::string& error);

/* The manifest as TOML: `total_bytes`, then one `[[chunk]]` table per chunk.
   Two serializations of one manifest are byte-identical. */
[[nodiscard]] std::string formatManifest(const CasManifest& manifest);
[[nodiscard]] bool parseManifest(std::string_view text, CasManifest& out, std::string& error);

/* The chunk file's path under a store root. */
[[nodiscard]] std::string chunkPath(const std::string& root, const hash::Digest& hash);

/* Lower-case hex of a digest, and its parser (exactly 64 hex digits). */
[[nodiscard]] std::string formatDigest(const hash::Digest& digest);
[[nodiscard]] bool parseDigest(std::string_view text, hash::Digest& out);

} // namespace resonate::asset

#endif /* RESONATE_ASSET_CAS_H */
