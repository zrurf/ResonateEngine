/* Indexes the content tree's identity sidecars into one products manifest.
 * Unlike texcook this writes no products: a .meta beside a source *is* the
 * product, with sub id 1, and the manifest simply makes the id addressable.
 * Only sources that already carry a sidecar are indexed -- identity is a
 * deliberate authoring act, not something a walk hands out.
 *
 * The render/textures subtree is skipped: texcook owns that manifest and an id
 * named in two of them is a load error. The manifest lands in the build tree
 * and its paths reach back into the source content, which is where a desktop
 * run reads it; a shipped or staged layout composes its own. */

#include <resonate/asset/cas.h>
#include <resonate/asset/identity.h>
#include <resonate/asset/meta.h>
#include <resonate/core/blake3.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <system_error>
#include <vector>

namespace
{

std::string readFile(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open())
    {
        return {};
    }
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

/* The sidecar's GUID and importer state; the runtime's parser is the one
   contract, so the indexer cannot accept a sidecar the engine would refuse. */
bool metaOf(const std::filesystem::path& source, resonate::asset::AssetMeta& out)
{
    std::string error;
    if (!resonate::asset::readMetaFile(source.string() + ".meta", out, error))
    {
        std::fprintf(stderr, "assetindex: %s\n", error.c_str());
        return false;
    }
    return true;
}

/* Lexicographic by path, so the manifest is stable across runs. */
struct Entry
{
    std::string source; /* relative to the content root */
    std::string guid;
    std::string importer;
    std::string fingerprint;
    std::string product; /* relative to the manifest */
};

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr, "usage: assetindex <content-directory> <output-directory>\n");
        return 1;
    }
    const std::filesystem::path content_directory = argv[1];
    const std::filesystem::path output_directory = argv[2];
    const std::filesystem::path textures = content_directory / "render" / "textures";

    std::error_code error;
    std::vector<Entry> entries;
    std::map<std::string, std::string> by_guid;
    auto iterator = std::filesystem::recursive_directory_iterator(
        content_directory, std::filesystem::directory_options::skip_permission_denied, error);
    if (error)
    {
        std::fprintf(stderr, "assetindex: %s: cannot walk the content tree\n",
                     content_directory.string().c_str());
        return 1;
    }
    for (const std::filesystem::directory_entry& entry : iterator)
    {
        const std::filesystem::path& path = entry.path();
        if (path.extension() != ".meta" || !entry.is_regular_file(error) || error)
        {
            continue;
        }
        const std::filesystem::path source(path.string().substr(
            0U, path.string().size() - std::strlen(".meta")));
        if (!std::filesystem::is_regular_file(source, error) || error)
        {
            std::fprintf(stderr, "assetindex: %s: the sidecar has no source beside it\n",
                         path.string().c_str());
            continue;
        }
        if (source.string().rfind(textures.string(), 0U) == 0U)
        {
            continue; /* texcook's manifest owns the textures. */
        }
        resonate::asset::AssetMeta meta;
        if (!metaOf(source, meta))
        {
            return 1;
        }
        const std::string guid = resonate::asset::formatGuid(meta.id);
        const auto seen = by_guid.find(guid);
        if (seen != by_guid.end())
        {
            std::fprintf(stderr, "assetindex: %s and %s share the guid %s\n",
                         seen->second.c_str(), source.string().c_str(), guid.c_str());
            return 1;
        }
        by_guid.emplace(guid, source.string());
        const std::string bytes = readFile(source);
        const resonate::hash::Digest digest =
            resonate::hash::blake3(bytes.data(), bytes.size());
        const std::string fingerprint = resonate::asset::formatDigest(digest);
        if (!meta.source_hash.empty() && meta.source_hash != fingerprint)
        {
            std::fprintf(stderr,
                         "assetindex: %s changed since the importer last hashed it\n",
                         source.string().c_str());
        }
        Entry record;
        std::error_code relative_error;
        record.source =
            std::filesystem::relative(source, content_directory, error).generic_string();
        record.product = std::filesystem::relative(source, output_directory, relative_error)
                             .generic_string();
        record.guid = guid;
        record.importer = meta.importer;
        record.fingerprint = fingerprint;
        entries.push_back(std::move(record));
    }
    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) { return a.source < b.source; });

    std::filesystem::create_directories(output_directory, error);
    const std::filesystem::path manifest = output_directory / "products.toml";
    std::ofstream out(manifest, std::ios::binary);
    if (!out.is_open())
    {
        std::fprintf(stderr, "assetindex: %s: cannot write the manifest\n",
                     manifest.string().c_str());
        return 1;
    }
    out << "# Written by assetindex; the runtime's asset identity reads it.\n";
    for (const Entry& record : entries)
    {
        out << "[[product]]\n";
        out << "source = \"" << record.source << "\"\n";
        out << "guid = \"" << record.guid << "\"\n";
        if (!record.importer.empty())
        {
            out << "importer = \"" << record.importer << "\"\n";
        }
        out << "fingerprint = \"" << record.fingerprint << "\"\n";
        out << "sub_id = 1\n";
        out << "path = \"" << record.product << "\"\n\n";
    }
    std::printf("assetindex: %zu source(s) -> %s\n", entries.size(), manifest.string().c_str());
    return 0;
}
