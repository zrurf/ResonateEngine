#ifndef RESONATE_UI_DISPLAY_LIST_H
#define RESONATE_UI_DISPLAY_LIST_H

#include <cstddef>
#include <memory>

#include <resonate/core/math.h>
#include <resonate/core/span.h>
#include <resonate/ecs/world.h>

/*
 * One frame's UI geometry, in paint order.
 *
 * The list is the boundary between the retained tree and a renderer: widgets
 * are walked parent first, siblings by ascending RectTransform::z and then
 * hierarchy order, and every widget carrying Background contributes one
 * rectangle. The caller reuses one list across frames; the scratch the walk
 * needs grows to the tree once and is then allocation-free.
 */
namespace resonate::ui
{

struct Quad
{
    float x = 0.0F;
    float y = 0.0F;
    float width = 0.0F;
    float height = 0.0F;
    Vec4 color{1.0F, 1.0F, 1.0F, 1.0F};
};

class DisplayList
{
  public:
    DisplayList();
    ~DisplayList();

    DisplayList(DisplayList&&) noexcept;
    DisplayList& operator=(DisplayList&&) noexcept;
    DisplayList(const DisplayList&) = delete;
    DisplayList& operator=(const DisplayList&) = delete;

    void clear();
    void push(const Quad& quad);

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] Span<const Quad> quads() const noexcept;

  private:
    struct Impl;

    bool paint(ecs::World& world, ecs::Entity entity, std::uint32_t depth);
    friend bool buildDisplayList(ecs::World& world, ecs::Entity root, DisplayList& out);

    std::unique_ptr<Impl> impl_;
};

/* Fills `out` with the subtree rooted at `root` in paint order. The root may be
   any widget; a missing Canvas is not an error, so a subtree can be rendered as
   a panel. Reports and answers false when a Background widget has no
   ComputedRect or the tree exceeds the hierarchy's depth bound. */
bool buildDisplayList(ecs::World& world, ecs::Entity root, DisplayList& out);

} // namespace resonate::ui

#endif /* RESONATE_UI_DISPLAY_LIST_H */
