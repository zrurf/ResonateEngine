#ifndef RESONATE_ASSET_META_H
#define RESONATE_ASSET_META_H

/*
 * The sidecar beside a source asset: its stable GUID, the importer that turns
 * the source into products, and the content hash of the source the cook last
 * saw. The sidecar is text so a rename or a re-cook leaves a readable diff; the
 * identity is deliberate, never handed out by a directory walk.
 */

#include <map>
#include <string>
#include <string_view>

#include <resonate/asset/identity.h>

namespace resonate::asset
{

struct AssetMeta
{
    AssetId id{};          /* the GUID; the sub id belongs to a product, not the source */
    std::string importer;  /* empty when no importer has claimed the source */
    std::string source_hash; /* lower-case BLAKE3 hex; empty when never hashed */
    /* The importer's own key/value state, authored under [settings]; values are
       strings, and the canonical form writes them sorted by key. */
    std::map<std::string, std::string> settings;
};

/* Reads a sidecar; the GUID is required and must not be the null id. Unknown
   keys are ignored, so a newer writer's sidecar still loads here. */
[[nodiscard]] bool parseMeta(std::string_view text, AssetMeta& out, std::string& error);

/* The canonical sidecar text: the header, the GUID, then the optional fields.
   Two serializations of one sidecar are byte-identical; a minimal sidecar
   round-trips the shipped form exactly. */
[[nodiscard]] std::string formatMeta(const AssetMeta& meta);

[[nodiscard]] bool readMetaFile(const std::string& path, AssetMeta& out, std::string& error);
[[nodiscard]] bool writeMetaFile(const std::string& path, const AssetMeta& meta,
                                 std::string& error);

/* A fresh version-7 GUID: a millisecond timestamp prefix with a random tail,
   so ids sort near their creation time and stay unpredictable. */
[[nodiscard]] AssetId generateGuid();

} // namespace resonate::asset

#endif /* RESONATE_ASSET_META_H */
