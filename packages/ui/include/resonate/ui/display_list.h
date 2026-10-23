#ifndef RESONATE_UI_DISPLAY_LIST_H
#define RESONATE_UI_DISPLAY_LIST_H

#include <cstddef>
#include <cstdint>
#include <vector>

#include <resonate/core/math.h>
#include <resonate/core/span.h>
#include <resonate/ecs/entity.h>
#include <resonate/ecs/world.h>

/*
 * One frame's UI geometry, in paint order.
 *
 * The list is the boundary between the retained tree and a renderer: widgets
 * are walked parent first, siblings by ascending RectTransform::z and then
 * hierarchy order, and every widget carrying Background, Image or Text
 * contributes geometry; the focused text field also contributes its caret. The
 * caller reuses one list across frames; the scratch the walk needs grows to the
 * tree once and is then allocation-free.
 */
namespace resonate::ui
{

/* Engine-owned texture ids that need no asset registry. 0 is the white texel
   solid quads sample; 1 is the built-in checker; 2 is the font atlas. */
inline constexpr std::uint32_t kTextureWhite = 0U;
inline constexpr std::uint32_t kTextureChecker = 1U;
inline constexpr std::uint32_t kTextureFont = 2U;

struct Quad
{
    float x = 0.0F;
    float y = 0.0F;
    float width = 0.0F;
    float height = 0.0F;
    Vec4 color{1.0F, 1.0F, 1.0F, 1.0F};
    std::uint32_t texture = kTextureWhite;
    Vec2 uv_min{0.0F, 0.0F};
    Vec2 uv_max{1.0F, 1.0F};
};

class DisplayList
{
  public:
    DisplayList() = default;
    ~DisplayList() = default;

    DisplayList(DisplayList&&) noexcept = default;
    DisplayList& operator=(DisplayList&&) noexcept = default;
    DisplayList(const DisplayList&) = delete;
    DisplayList& operator=(const DisplayList&) = delete;

    void clear();
    void push(const Quad& quad);

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] Span<const Quad> quads() const noexcept;

  private:
    struct OrderEntry
    {
        ecs::Entity entity;
        std::int32_t z = 0;
    };

    bool paint(ecs::World& world, ecs::Entity entity, std::uint32_t depth);
    friend bool buildDisplayList(ecs::World& world, ecs::Entity root, DisplayList& out);

    std::vector<Quad> quads_;
    std::vector<OrderEntry> order_;
    ecs::Entity focused_{};
};

/* Fills `out` with the subtree rooted at `root` in paint order. The root may be
   any widget; a missing Canvas is not an error, so a subtree can be rendered as
   a panel. Reports and answers false when a Background widget has no
   ComputedRect or the tree exceeds the hierarchy's depth bound. */
bool buildDisplayList(ecs::World& world, ecs::Entity root, DisplayList& out);

} // namespace resonate::ui

#endif /* RESONATE_UI_DISPLAY_LIST_H */
