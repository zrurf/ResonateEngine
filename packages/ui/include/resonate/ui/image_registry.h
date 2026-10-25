#ifndef RESONATE_UI_IMAGE_REGISTRY_H
#define RESONATE_UI_IMAGE_REGISTRY_H

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

namespace resonate::ui
{

/* Maps authoring image paths to Picture texture ids. Startup registers the
   shipped files; instantiate resolves a node's `path` through it, so a
   document names files and never raw texture ids. */
class ImageRegistry
{
  public:
    /* Ids below this belong to the engine: 0 is the white texel, 1 the
       built-in checker and 2 the font atlas. */
    static constexpr std::uint32_t kFirstId = 16U;
    static constexpr std::uint32_t kUnregistered = 0U;

    /* Registers a path; re-registering answers the existing id. */
    std::uint32_t registerImage(std::string_view path);

    /* The registered id, or kUnregistered. */
    [[nodiscard]] std::uint32_t find(std::string_view path) const;

  private:
    std::unordered_map<std::string, std::uint32_t> ids_;
};

} // namespace resonate::ui

#endif /* RESONATE_UI_IMAGE_REGISTRY_H */
