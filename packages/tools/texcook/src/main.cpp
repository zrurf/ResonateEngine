/* Cooks the P6 PPM sources under content/render/textures into KTX2 files with
 * UASTC Basis payloads. The runtime transcoder turns them back into RGBA8;
 * compressed GPU formats slot in behind the same container later.
 *
 * Identity and skip: each source carries a random GUID in a .meta sidecar (the
 * tool creates one on first sight), and a products manifest beside the output
 * records source -> guid -> product path -> fingerprint. A source whose
 * fingerprint still matches is not re-cooked, so a build only pays for what
 * changed. */

#include <basisu/encoder/basisu_comp.h>
#include <basisu/encoder/basisu_enc.h>

#if !defined(TOML_EXCEPTIONS)
#    define TOML_EXCEPTIONS 0
#endif

#include <toml++/toml.hpp>

#include <resonate/asset/meta.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace
{

namespace asset = resonate::asset;

constexpr const char* kCookSalt = "resonate.texcook.uastc-ktx2.v1";

bool readPpm(const std::filesystem::path& path, std::uint32_t& out_width, std::uint32_t& out_height,
             std::vector<std::uint8_t>& out_rgba)
{
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open())
    {
        return false;
    }
    const std::vector<char> data((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());
    std::size_t position = 0;
    const auto token = [&data, &position]() -> std::string
    {
        while (position < data.size())
        {
            const char c = data[position];
            if (c == '#')
            {
                while (position < data.size() && data[position] != '\n')
                {
                    ++position;
                }
                continue;
            }
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
            {
                ++position;
            }
            else
            {
                break;
            }
        }
        const std::size_t start = position;
        while (position < data.size() && data[position] != ' ' && data[position] != '\t' &&
               data[position] != '\r' && data[position] != '\n')
        {
            ++position;
        }
        return std::string(data.begin() + static_cast<std::ptrdiff_t>(start),
                           data.begin() + static_cast<std::ptrdiff_t>(position));
    };

    const std::string magic = token();
    const int width = std::atoi(token().c_str());
    const int height = std::atoi(token().c_str());
    const int maxval = std::atoi(token().c_str());
    if (magic != "P6" || width <= 0 || height <= 0 || maxval != 255)
    {
        return false;
    }
    ++position; /* the separator after maxval */
    const std::size_t pixels = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    if (position + pixels * 3U > data.size())
    {
        return false;
    }

    out_width = static_cast<std::uint32_t>(width);
    out_height = static_cast<std::uint32_t>(height);
    out_rgba.resize(pixels * 4U);
    for (std::size_t pixel = 0; pixel < pixels; ++pixel)
    {
        const auto* rgb =
            reinterpret_cast<const std::uint8_t*>(data.data() + position + pixel * 3U);
        out_rgba[pixel * 4U] = rgb[0];
        out_rgba[pixel * 4U + 1U] = rgb[1];
        out_rgba[pixel * 4U + 2U] = rgb[2];
        out_rgba[pixel * 4U + 3U] = 255U;
    }
    return true;
}

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

/* The source's identity from its .meta sidecar, creating the sidecar when it
   does not exist. False when a sidecar exists but does not parse: an identity
   is never silently regenerated. */
bool identityOf(const std::filesystem::path& source, std::string& out_guid)
{
    const std::filesystem::path meta = source.string() + ".meta";
    asset::AssetMeta identity;
    std::string error;
    if (std::filesystem::exists(meta))
    {
        if (!asset::readMetaFile(meta.string(), identity, error))
        {
            std::fprintf(stderr, "texcook: %s: %s\n", meta.string().c_str(), error.c_str());
            return false;
        }
    }
    else
    {
        /* A fresh identity. The sidecar is the only place one is born, and it
           is committed, so the identity outlives every cook. */
        identity.id = asset::generateGuid();
        identity.importer = "texture.ppm";
        if (!asset::writeMetaFile(meta.string(), identity, error))
        {
            std::fprintf(stderr, "texcook: %s: %s\n", meta.string().c_str(), error.c_str());
            return false;
        }
    }
    out_guid = asset::formatGuid(identity.id);
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr, "usage: texcook <source-directory> <output-directory>\n");
        return 1;
    }
    basisu::basisu_encoder_init(false);
    const std::filesystem::path source_directory = argv[1];
    const std::filesystem::path output_directory = argv[2];
    std::error_code error;
    std::filesystem::create_directories(output_directory, error);

    /* What the last run cooked: fingerprint and product path. */
    struct Previous
    {
        std::string fingerprint;
        std::string path;
    };
    std::vector<Previous> previous;
    {
        const std::string text = readFile(output_directory / "products.toml");
        toml::parse_result result = toml::parse(text);
        if (result)
        {
            if (const toml::array* products = result.table()["product"].as_array())
            {
                for (const toml::node& entry : *products)
                {
                    const toml::table* product = entry.as_table();
                    if (product == nullptr)
                    {
                        continue;
                    }
                    Previous record;
                    record.fingerprint = product->at_path("fingerprint").value_or(std::string{});
                    record.path = product->at_path("path").value_or(std::string{});
                    previous.push_back(std::move(record));
                }
            }
        }
    }

    std::vector<std::filesystem::path> sources;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(source_directory, error))
    {
        if (entry.path().extension() == ".ppm")
        {
            sources.push_back(entry.path());
        }
    }
    std::sort(sources.begin(), sources.end());

    std::string manifest = "# Written by texcook; the runtime's asset identity reads it.\n";
    int cooked = 0;
    int skipped = 0;
    for (const std::filesystem::path& source : sources)
    {
        std::string guid;
        if (!identityOf(source, guid))
        {
            return 1;
        }
        const std::string bytes = readFile(source);
        const std::string fingerprint =
            hex(fnv1a(bytes.data(), bytes.size(),
                      fnv1a(kCookSalt, std::strlen(kCookSalt), 1469598103934665603ULL)));
        const std::string product = source.stem().string() + ".ktx2";

        bool fresh = true;
        for (const Previous& record : previous)
        {
            if (record.fingerprint == fingerprint && record.path == product &&
                std::filesystem::exists(output_directory / product))
            {
                fresh = false;
                break;
            }
        }
        if (fresh)
        {
            std::uint32_t width = 0U;
            std::uint32_t height = 0U;
            std::vector<std::uint8_t> rgba;
            if (!readPpm(source, width, height, rgba))
            {
                std::fprintf(stderr, "texcook: %s: not a readable P6 PPM\n",
                             source.string().c_str());
                return 1;
            }

            basisu::basis_compressor_params params;
            params.m_source_images.push_back(basisu::image(rgba.data(), width, height, 4));
            params.m_uastc = true;
            params.m_create_ktx2_file = true;
            params.m_perceptual = false;
            params.m_status_output = false;
            params.m_write_output_basis_files = true;
            params.m_out_filename = (output_directory / product).string();

            basisu::job_pool pool(1);
            params.m_pJob_pool = &pool;
            basisu::basis_compressor compressor;
            if (!compressor.init(params) ||
                compressor.process() != basisu::basis_compressor::cECSuccess)
            {
                std::fprintf(stderr, "texcook: %s: basis encoding failed\n",
                             source.string().c_str());
                return 1;
            }
            std::printf("texcook: %s -> %s (%ux%u)\n", source.filename().string().c_str(),
                        params.m_out_filename.c_str(), width, height);
            ++cooked;
        }
        else
        {
            ++skipped;
        }

        manifest += "[[product]]\n";
        manifest += "source = \"" + source.filename().string() + "\"\n";
        manifest += "guid = \"" + guid + "\"\n";
        manifest += "fingerprint = \"" + fingerprint + "\"\n";
        manifest += "sub_id = 1\n";
        manifest += "path = \"" + product + "\"\n";
    }

    const std::filesystem::path manifest_path = output_directory / "products.toml";
    std::ofstream out(manifest_path, std::ios::binary);
    if (!out.is_open())
    {
        std::fprintf(stderr, "texcook: %s: cannot write the manifest\n",
                     manifest_path.string().c_str());
        return 1;
    }
    out << manifest;
    std::printf("texcook: %d cooked, %d skipped -> %s\n", cooked, skipped,
                output_directory.string().c_str());
    return 0;
}
