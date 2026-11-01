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

#if !defined(TOML_EXCEPTIONS)
#    define TOML_EXCEPTIONS 0
#endif

#include <toml++/toml.hpp>

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

constexpr const char* kIndexSalt = "resonate.assetindex.source.v1";

std::string readFile(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open())
    {
        return {};
    }
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::uint64_t fnv1a(const void* bytes, std::size_t size, std::uint64_t seed)
{
    const auto* data = static_cast<const std::uint8_t*>(bytes);
    std::uint64_t hash = seed;
    for (std::size_t index = 0U; index < size; ++index)
    {
        hash ^= data[index];
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::string hex(std::uint64_t value)
{
    static const char* kDigits = "0123456789abcdef";
    std::string text(16U, '0');
    for (int shift = 60; shift >= 0; shift -= 4)
    {
        text[static_cast<std::size_t>((60 - shift) / 4)] = kDigits[(value >> shift) & 0x0FU];
    }
    return text;
}

/* The sidecar's GUID; false with a report when it exists but carries nothing
   usable. The indexer never creates one. */
bool guidOf(const std::filesystem::path& source, std::string& out_guid)
{
    const std::filesystem::path meta = source.string() + ".meta";
    const std::string text = readFile(meta);
    if (text.empty())
    {
        std::fprintf(stderr, "assetindex: %s: cannot read the sidecar\n", meta.string().c_str());
        return false;
    }
    toml::parse_result result = toml::parse(text);
    if (!result)
    {
        std::fprintf(stderr, "assetindex: %s: %s\n", meta.string().c_str(),
                     result.error().description().data());
        return false;
    }
    const std::string guid = result.table()["guid"].value_or(std::string{});
    if (guid.size() != 36U)
    {
        std::fprintf(stderr, "assetindex: %s: the sidecar carries no usable guid\n",
                     meta.string().c_str());
        return false;
    }
    out_guid = guid;
    return true;
}

/* Lexicographic by path, so the manifest is stable across runs. */
struct Entry
{
    std::string source; /* relative to the content root */
    std::string guid;
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
        std::string guid;
        if (!guidOf(source, guid))
        {
            return 1;
        }
        const auto seen = by_guid.find(guid);
        if (seen != by_guid.end())
        {
            std::fprintf(stderr, "assetindex: %s and %s share the guid %s\n",
                         seen->second.c_str(), source.string().c_str(), guid.c_str());
            return 1;
        }
        by_guid.emplace(guid, source.string());
        const std::string bytes = readFile(source);
        Entry record;
        std::error_code relative_error;
        record.source =
            std::filesystem::relative(source, content_directory, error).generic_string();
        record.product = std::filesystem::relative(source, output_directory, relative_error)
                             .generic_string();
        record.guid = guid;
        record.fingerprint =
            hex(fnv1a(bytes.data(), bytes.size(),
                      fnv1a(kIndexSalt, std::strlen(kIndexSalt), 1469598103934665603ULL)));
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
        out << "fingerprint = \"" << record.fingerprint << "\"\n";
        out << "sub_id = 1\n";
        out << "path = \"" << record.product << "\"\n\n";
    }
    std::printf("assetindex: %zu source(s) -> %s\n", entries.size(), manifest.string().c_str());
    return 0;
}
