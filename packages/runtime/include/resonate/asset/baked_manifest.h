#ifndef RESONATE_ASSET_BAKED_MANIFEST_H
#define RESONATE_ASSET_BAKED_MANIFEST_H

#include <cstdint>
#include <string>
#include <vector>

/* A baked manifest never throws: the readers answer status codes, like every
   other error crossing these boundaries. The macro has to precede the include,
   and every TU that pulls this header must agree with it. */
#if !defined(TOML_EXCEPTIONS)
#    define TOML_EXCEPTIONS 0
#endif

#include <toml++/toml.hpp>

namespace resonate::asset
{

/* Packaged builds ship their manifests baked: the TOML is parsed once at
   package time and stored as a compact binary form the runtime reads straight
   into a table, so no text parser runs and no source paths travel with the
   build. The source tree keeps the TOML; the readers try the baked twin first
   and fall back to the text.

   The format ('RBM1') covers the subset the engine's manifests use: tables,
   arrays, strings, 64-bit integers, booleans and doubles. */

inline constexpr char kBakedManifestExtension[] = ".rman";

/* products.toml -> products.rman */
std::string bakedTwinOf(const std::string& path);

/* Whichever of the baked twin and the text form exists, preferring the baked
   one; empty when neither does. Callers keep naming the logical (.toml) path
   and hand the answer's *original* form to the readers, which try the twin
   themselves. */
std::string resolveManifestPath(const std::string& path);

/* Serializes the table; false for a value kind the format does not carry. */
bool bakeTable(const toml::table& in, std::vector<std::uint8_t>& out, std::string& error);

/* Reads one baked file back into a table. */
bool loadBaked(const std::string& path, toml::table& out, std::string& error);

} // namespace resonate::asset

#endif /* RESONATE_ASSET_BAKED_MANIFEST_H */
