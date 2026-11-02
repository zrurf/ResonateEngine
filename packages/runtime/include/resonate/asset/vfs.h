#ifndef RESONATE_ASSET_VFS_H
#define RESONATE_ASSET_VFS_H

/*
 * How the runtime reaches an asset's bytes. A reference is an AssetId; the
 * backend decides where that id lives — files beside a products manifest in a
 * development tree, a chunk store in a packaged build, or a remote container
 * later. Loaders speak to this interface, so which one a run uses is a
 * startup choice, not a code path per asset.
 */

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <resonate/asset/identity.h>

namespace resonate::asset
{

class Vfs
{
  public:
    Vfs() = default;
    virtual ~Vfs() = default;

    Vfs(const Vfs&) = delete;
    Vfs& operator=(const Vfs&) = delete;

    /* The product's bytes. False with a reason for an unknown id or a read
       failure; `out` is untouched then. */
    [[nodiscard]] virtual bool read(const AssetId& id, std::vector<std::byte>& out,
                                    std::string& error) const = 0;

    /* Where the product lives, for tooling and diagnostics; nothing for an id
       this backend does not know. */
    [[nodiscard]] virtual std::optional<std::string> locate(const AssetId& id) const = 0;
};

/* Products stored as plain files beside a products manifest: the development
   tree and an unpacked build use this backend. */
class DirectoryVfs final : public Vfs
{
  public:
    /* `manifest_path` names the products manifest; product paths resolve
       relative to its directory. */
    [[nodiscard]] static std::unique_ptr<DirectoryVfs> open(const std::string& manifest_path,
                                                            std::string& error);

    [[nodiscard]] bool read(const AssetId& id, std::vector<std::byte>& out,
                            std::string& error) const override;
    [[nodiscard]] std::optional<std::string> locate(const AssetId& id) const override;

    /* The manifest's products, for an editor or a tool that enumerates them. */
    [[nodiscard]] const Identity& products() const noexcept
    {
        return products_;
    }

  private:
    DirectoryVfs(std::string root, Identity products)
        : root_(std::move(root)), products_(std::move(products))
    {
    }

    std::string root_;
    Identity products_;
};

} // namespace resonate::asset

#endif /* RESONATE_ASSET_VFS_H */
