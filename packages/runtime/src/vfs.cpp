#include <resonate/asset/vfs.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <string_view>
#include <utility>

namespace resonate::asset
{
namespace
{

std::string joined(const std::string& root, const std::string& relative)
{
    return (std::filesystem::path(root) / relative).string();
}

bool readAll(const std::string& path, std::vector<std::byte>& out, std::string& error)
{
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open())
    {
        error = "cannot read " + path;
        return false;
    }
    in.seekg(0, std::ios::end);
    const std::streamoff length = in.tellg();
    if (length < 0)
    {
        error = "cannot read " + path;
        return false;
    }
    in.seekg(0, std::ios::beg);
    std::vector<std::byte> data(static_cast<std::size_t>(length));
    if (!data.empty() &&
        !in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size())))
    {
        error = "cannot read " + path;
        return false;
    }
    out = std::move(data);
    return true;
}

} // namespace

std::unique_ptr<DirectoryVfs> DirectoryVfs::open(const std::string& manifest_path,
                                                 std::string& error)
{
    Identity products;
    if (!products.loadFile(manifest_path, error))
    {
        return nullptr;
    }
    const std::filesystem::path manifest(manifest_path);
    return std::unique_ptr<DirectoryVfs>(
        new DirectoryVfs(manifest.parent_path().string(), std::move(products)));
}

bool DirectoryVfs::read(const AssetId& id, std::vector<std::byte>& out,
                        std::string& error) const
{
    const std::optional<std::string> located = products_.locate(id);
    if (!located)
    {
        error = "the store has no product " + formatGuid(id) + ":" + std::to_string(id.sub_id);
        return false;
    }
    std::vector<std::byte> data;
    if (!readAll(joined(root_, *located), data, error))
    {
        return false;
    }
    out = std::move(data);
    return true;
}

std::optional<std::string> DirectoryVfs::locate(const AssetId& id) const
{
    const std::optional<std::string> located = products_.locate(id);
    if (!located)
    {
        return std::nullopt;
    }
    return joined(root_, *located);
}

std::unique_ptr<CasVfs> CasVfs::open(const std::string& root, const ContentKey& key,
                                     std::string& error)
{
    Identity products;
    if (!products.loadFile(joined(root, "products.toml"), error))
    {
        return nullptr;
    }
    return std::unique_ptr<CasVfs>(new CasVfs(root, key, std::move(products)));
}

bool CasVfs::read(const AssetId& id, std::vector<std::byte>& out, std::string& error) const
{
    const std::optional<std::string> located = products_.locate(id);
    if (!located)
    {
        error = "the store has no product " + formatGuid(id) + ":" + std::to_string(id.sub_id);
        return false;
    }
    std::vector<std::byte> manifest_text;
    if (!readAll(joined(root_, *located), manifest_text, error))
    {
        return false;
    }
    CasManifest manifest;
    if (!parseManifest(
            std::string_view(reinterpret_cast<const char*>(manifest_text.data()),
                             manifest_text.size()),
            manifest, error))
    {
        return false;
    }
    std::vector<std::byte> bytes;
    if (!readBuffer(root_, key_, manifest, bytes, error))
    {
        return false;
    }
    out = std::move(bytes);
    return true;
}

std::optional<std::string> CasVfs::locate(const AssetId& id) const
{
    const std::optional<std::string> located = products_.locate(id);
    if (!located)
    {
        return std::nullopt;
    }
    return joined(root_, *located);
}

} // namespace resonate::asset
