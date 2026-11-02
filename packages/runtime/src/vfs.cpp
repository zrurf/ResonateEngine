#include <resonate/asset/vfs.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <utility>

namespace resonate::asset
{
namespace
{

std::string joined(const std::string& root, const std::string& relative)
{
    return (std::filesystem::path(root) / relative).string();
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
    const std::string path = joined(root_, *located);
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

std::optional<std::string> DirectoryVfs::locate(const AssetId& id) const
{
    const std::optional<std::string> located = products_.locate(id);
    if (!located)
    {
        return std::nullopt;
    }
    return joined(root_, *located);
}

} // namespace resonate::asset
