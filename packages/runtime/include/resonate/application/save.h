#ifndef RESONATE_APPLICATION_SAVE_H
#define RESONATE_APPLICATION_SAVE_H

/*
 * User data: saves and settings live outside the asset tree, in the platform's
 * application-data directory, and are written atomically so a crash leaves
 * either the old file or the new one. A save carries a version so a loader can
 * migrate it, and a hash so a truncated or edited file does not parse as
 * state.
 */

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <resonate/core/span.h>

namespace resonate::save
{

/* One save's versioned envelope around an opaque payload. The payload's shape
   belongs to the domain that wrote it; the version is what its migration chain
   keys on. */
struct SaveFile
{
    std::uint32_t version = 1U;
    std::vector<std::byte> payload;
};

[[nodiscard]] std::vector<std::byte> encodeSave(const SaveFile& file);
[[nodiscard]] bool decodeSave(Span<const std::byte> bytes, SaveFile& out, std::string& error);

/* A directory of named saves: `<root>/saves/<name>.sav`. The root is the
   platform's application-data directory, which the host resolves. */
class SaveStore
{
  public:
    explicit SaveStore(std::string root);

    [[nodiscard]] const std::string& root() const noexcept
    {
        return root_;
    }

    /* The file `name` maps to; nothing for a name that is not a plain file
       stem (empty, a path separator, a leading dot, `..`). */
    [[nodiscard]] std::optional<std::string> pathOf(const std::string& name) const;

    /* Replaces the save atomically: a reader sees the old file or the new
       one, never a partial write. */
    [[nodiscard]] bool write(const std::string& name, const SaveFile& file, std::string& error);
    [[nodiscard]] bool read(const std::string& name, SaveFile& out, std::string& error) const;
    [[nodiscard]] bool remove(const std::string& name, std::string& error);

  private:
    std::string root_;
};

/* The platform's per-application data directory. `RESONATE_USER_DATA` wins
   when a host stages its own (Android and iOS set it); otherwise the desktop
   conventions apply. Empty when the platform names none. */
[[nodiscard]] std::string defaultUserDataRoot(const std::string& application);

} // namespace resonate::save

#endif /* RESONATE_APPLICATION_SAVE_H */
