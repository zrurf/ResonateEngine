#ifndef RESONATE_UI_ICON_REGISTRY_H
#define RESONATE_UI_ICON_REGISTRY_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

#include <resonate/core/math.h>

namespace resonate::ui
{

/* Maps icon names to the sub-rect of the shipped atlas that holds them. The
   registry carries the atlas texture id as well, because an Icon node resolves
   to one textured quad whose texture and UV rect both come from here: the map
   file names fractions, so the atlas can be re-cooked without touching
   documents. */
class IconRegistry
{
  public:
    struct Icon
    {
        Vec2 uv_min{};
        Vec2 uv_max{};
    };

    /* Parses the cooker's map: one "name u0 v0 u1 v1" line per icon with the
       fractions of the atlas, '#' starting a comment line. Answers false with
       `error` on a malformed line. */
    bool load(std::string_view text, std::string& error);

    /* The texture id every icon samples; the icon is not drawable until one is
       set. */
    void setTexture(std::uint32_t texture) noexcept { texture_ = texture; }
    [[nodiscard]] std::uint32_t texture() const noexcept { return texture_; }

    /* The entry, or nullptr for an unknown name. */
    [[nodiscard]] const Icon* find(std::string_view name) const;

    [[nodiscard]] std::size_t size() const noexcept { return icons_.size(); }

  private:
    std::unordered_map<std::string, Icon> icons_;
    std::uint32_t texture_ = 0U;
};

} // namespace resonate::ui

#endif /* RESONATE_UI_ICON_REGISTRY_H */
