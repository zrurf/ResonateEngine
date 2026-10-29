#ifndef RESONATE_ASSET_IDENTITY_H
#define RESONATE_ASSET_IDENTITY_H

/*
 * What a reference is made of, and how a cook maps it back to bytes.
 *
 * A source asset carries a random 128-bit GUID in a .meta sidecar beside it;
 * the identity survives renames, moves and re-cooks. A cook turns one source
 * into one or more products, each named by the source's GUID plus a sub id
 * that stays stable across cooks. A products manifest, written beside the
 * products, is the bridge: the runtime reads it and answers "where are the
 * bytes for this id" without knowing how a source was cooked.
 */

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace resonate::asset
{

/* A source asset's identity: the GUID its sidecar carries plus the index of
   the product within that source's cook. All-zero is the null id. */
struct AssetId
{
    std::uint64_t guid_low = 0U;
    std::uint64_t guid_high = 0U;
    std::uint32_t sub_id = 0U;

    [[nodiscard]] bool valid() const noexcept
    {
        return (guid_low != 0U || guid_high != 0U) && sub_id != 0U;
    }

    friend bool operator==(const AssetId&, const AssetId&) = default;
};

/* The canonical 8-4-4-4-12 hex form the sidecar stores, lower case. */
[[nodiscard]] std::string formatGuid(const AssetId& id);

/* Parses the canonical form (dashes optional); false with the id untouched
   otherwise. The sub id is not part of a GUID. */
[[nodiscard]] bool parseGuid(std::string_view text, AssetId& out);

/* One product a manifest records: an id and where its bytes sit, relative to
   the manifest's own directory. */
struct Product
{
    AssetId id{};
    std::string path;
};

/* The identities a set of manifests made visible. */
class Identity
{
  public:
    /* Reads a products manifest ([[product]] tables with guid, sub_id and
       path). False with a named error; an id named twice is one. */
    bool loadProducts(std::string_view toml, std::string& error);

    /* The product's path, or nothing for an id no loaded manifest knows. */
    [[nodiscard]] std::optional<std::string> locate(const AssetId& id) const;

    [[nodiscard]] std::size_t size() const noexcept { return products_.size(); }

  private:
    std::vector<Product> products_;
};

} // namespace resonate::asset

#endif /* RESONATE_ASSET_IDENTITY_H */
