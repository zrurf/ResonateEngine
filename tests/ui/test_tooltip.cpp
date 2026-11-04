#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <string>
#include <vector>

#include <resonate/core/allocator.h>
#include <resonate/dom/document.h>
#include <resonate/ecs/world.h>
#include <resonate/hierarchy/tree.h>
#include <resonate/ui/display_list.h>
#include <resonate/ui/document.h>
#include <resonate/ui/document_schema.h>
#include <resonate/ui/hit_test.h>
#include <resonate/ui/input.h>
#include <resonate/ui/layout.h>
#include <resonate/ui/tooltip.h>

#include "resonate.ui/components.gen.h"

namespace
{

using resonate::ecs::Entity;
using resonate::ecs::World;

/* A wide button whose tooltip is a small label: wide enough for the pointer to
   reach the canvas edges while still hovering it. */
const char* const kDocument = R"xml(<document version="1" schemas="resonate.ui">
  <Screen key="screen">
    <StyleSheet key="styles">
      <StyleRule selector=".panel">
        <Background color="0.1 0.1 0.1 1"/>
      </StyleRule>
      <StyleRule selector=".action">
        <Background color="0.2 0.2 0.2 1"/>
      </StyleRule>
      <StyleRule selector=".after">
        <Background color="0.3 0.3 0.3 1"/>
      </StyleRule>
      <StyleRule selector=".tip">
        <Background color="1 1 1 1"/>
      </StyleRule>
    </StyleSheet>
    <Panel key="panel" class="panel" direction="Column" x="0" y="0" width="320" height="240">
      <Button key="action" class="action" x="0" y="0" width="300" height="200">
        <Label key="tip" class="tip" width="64" height="22">
          <Tooltip delay_ms="200" offset_x="12" offset_y="40"/>
        </Label>
      </Button>
      <Box key="after" class="after" x="0" y="200" width="320" height="40"/>
    </Panel>
  </Screen>
</document>)xml";

struct Fixture
{
    World world{resonate::systemAllocator()};
    Entity root{};
    Entity action{};
    Entity tip{};
    Entity after{};

    Fixture()
    {
        REQUIRE(resonate::hierarchy::registerComponents(world));
        REQUIRE(resonate::ui::registerComponents(world));
        std::vector<resonate::dom::Diagnostic> diagnostics;
        auto document = resonate::dom::Document::parse(kDocument, &resonate::ui::documentSchema(),
                                                       diagnostics);
        REQUIRE(document != nullptr);
        REQUIRE(diagnostics.empty());
        root = resonate::ui::instantiate(world, *document);
        REQUIRE(root.valid());
        REQUIRE(resonate::ui::layout(world, root, 320.0F, 240.0F));
        action = resonate::ui::findByKey(world, root, "action");
        tip = resonate::ui::findByKey(world, root, "tip");
        after = resonate::ui::findByKey(world, root, "after");
        REQUIRE(action.valid());
        REQUIRE(tip.valid());
        REQUIRE(after.valid());
    }
};

bool shown(World& world, Entity entity)
{
    const resonate::ui::Shown* component = world.get<resonate::ui::Shown>(entity);
    return component != nullptr && component->shown;
}

} // namespace

TEST_CASE("ui tooltip: the node starts hidden and out of flow", "[ui][tooltip]")
{
    Fixture fixture;

    const auto* transform = fixture.world.get<resonate::ui::RectTransform>(fixture.tip);
    REQUIRE(transform != nullptr);
    CHECK(transform->position == resonate::ui::PositionMode::Absolute);
    CHECK(transform->anchor_min.x == 0.0F);
    CHECK(transform->anchor_max.x == 0.0F);
    CHECK_FALSE(shown(fixture.world, fixture.tip));

    /* Out of flow: the box after it starts where the button ends. */
    const auto* after_rect = fixture.world.get<resonate::ui::ComputedRect>(fixture.after);
    REQUIRE(after_rect != nullptr);
    CHECK(std::fabs(after_rect->y - 200.0F) < 0.01F);

    /* Hidden widgets paint nothing. */
    resonate::ui::DisplayList list;
    REQUIRE(resonate::ui::buildDisplayList(fixture.world, fixture.root, list));
    const auto* tip_rect = fixture.world.get<resonate::ui::ComputedRect>(fixture.tip);
    REQUIRE(tip_rect != nullptr);
    for (const resonate::ui::Quad& quad : list.quads())
    {
        const bool same_box = quad.x == tip_rect->x && quad.y == tip_rect->y &&
                              quad.width == tip_rect->width;
        CHECK_FALSE(same_box);
    }
}

TEST_CASE("ui tooltip: dwelling shows it near the pointer and leaving hides it", "[ui][tooltip]")
{
    Fixture fixture;
    resonate::ui::TooltipSystem tooltips;
    REQUIRE(tooltips.bind(fixture.world));

    REQUIRE(resonate::ui::updateInput(fixture.world, fixture.root, 100.0F, 80.0F, false));
    tooltips.advance(fixture.world, fixture.root, 100.0F, 80.0F, 0.1F);
    CHECK_FALSE(shown(fixture.world, fixture.tip));

    /* Past the delay it appears at the pointer plus the offsets. */
    tooltips.advance(fixture.world, fixture.root, 100.0F, 80.0F, 0.15F);
    CHECK(shown(fixture.world, fixture.tip));
    REQUIRE(resonate::ui::layout(fixture.world, fixture.root, 320.0F, 240.0F));
    const auto* rect = fixture.world.get<resonate::ui::ComputedRect>(fixture.tip);
    REQUIRE(rect != nullptr);
    CHECK(std::fabs(rect->x - 112.0F) < 0.5F);
    CHECK(std::fabs(rect->y - 120.0F) < 0.5F);

    /* Near the far corner the box is clamped inside the canvas. */
    REQUIRE(resonate::ui::updateInput(fixture.world, fixture.root, 295.0F, 195.0F, false));
    tooltips.advance(fixture.world, fixture.root, 295.0F, 195.0F, 0.05F);
    REQUIRE(resonate::ui::layout(fixture.world, fixture.root, 320.0F, 240.0F));
    rect = fixture.world.get<resonate::ui::ComputedRect>(fixture.tip);
    REQUIRE(rect != nullptr);
    CHECK(std::fabs(rect->x - 256.0F) < 0.5F);
    CHECK(std::fabs(rect->y - 218.0F) < 0.5F);

    /* Leaving the widget hides it. */
    REQUIRE(resonate::ui::updateInput(fixture.world, fixture.root, 500.0F, 500.0F, false));
    tooltips.advance(fixture.world, fixture.root, 500.0F, 500.0F, 0.05F);
    CHECK_FALSE(shown(fixture.world, fixture.tip));
}

TEST_CASE("ui tooltip: a press hides the tooltip until the dwell starts again", "[ui][tooltip]")
{
    Fixture fixture;
    resonate::ui::TooltipSystem tooltips;
    REQUIRE(tooltips.bind(fixture.world));

    REQUIRE(resonate::ui::updateInput(fixture.world, fixture.root, 100.0F, 80.0F, false));
    tooltips.advance(fixture.world, fixture.root, 100.0F, 80.0F, 0.25F);
    REQUIRE(shown(fixture.world, fixture.tip));

    REQUIRE(resonate::ui::updateInput(fixture.world, fixture.root, 100.0F, 80.0F, true));
    tooltips.advance(fixture.world, fixture.root, 100.0F, 80.0F, 0.01F);
    CHECK_FALSE(shown(fixture.world, fixture.tip));

    /* Release and dwell again: the timer restarted with the press. */
    REQUIRE(resonate::ui::updateInput(fixture.world, fixture.root, 100.0F, 80.0F, false));
    tooltips.advance(fixture.world, fixture.root, 100.0F, 80.0F, 0.1F);
    CHECK_FALSE(shown(fixture.world, fixture.tip));
    tooltips.advance(fixture.world, fixture.root, 100.0F, 80.0F, 0.15F);
    CHECK(shown(fixture.world, fixture.tip));
}

TEST_CASE("ui tooltip: the display list paints it last and unclipped", "[ui][tooltip]")
{
    Fixture fixture;
    resonate::ui::TooltipSystem tooltips;
    REQUIRE(tooltips.bind(fixture.world));
    REQUIRE(resonate::ui::updateInput(fixture.world, fixture.root, 100.0F, 80.0F, false));
    tooltips.advance(fixture.world, fixture.root, 100.0F, 80.0F, 0.25F);
    REQUIRE(shown(fixture.world, fixture.tip));
    REQUIRE(resonate::ui::layout(fixture.world, fixture.root, 320.0F, 240.0F));

    resonate::ui::DisplayList list;
    REQUIRE(resonate::ui::buildDisplayList(fixture.world, fixture.root, list));
    REQUIRE_FALSE(list.quads().empty());
    const resonate::ui::Quad& last = list.quads()[list.quads().size() - 1U];
    const auto* rect = fixture.world.get<resonate::ui::ComputedRect>(fixture.tip);
    REQUIRE(rect != nullptr);
    CHECK(std::fabs(last.x - rect->x) < 0.01F);
    CHECK(std::fabs(last.y - rect->y) < 0.01F);
    CHECK(std::fabs(last.width - rect->width) < 0.01F);
    /* The whole target, not a clip. */
    CHECK(last.clip.width <= 0.0F);

    /* The sibling after the button painted before the tooltip. */
    const auto* after_rect = fixture.world.get<resonate::ui::ComputedRect>(fixture.after);
    REQUIRE(after_rect != nullptr);
    bool after_seen = false;
    for (const resonate::ui::Quad& quad : list.quads())
    {
        if (std::fabs(quad.y - after_rect->y) < 0.01F &&
            std::fabs(quad.width - after_rect->width) < 0.01F)
        {
            after_seen = true;
            break;
        }
    }
    CHECK(after_seen);
}

TEST_CASE("ui tooltip: pointer hits pass through the tooltip", "[ui][tooltip]")
{
    Fixture fixture;
    resonate::ui::TooltipSystem tooltips;
    REQUIRE(tooltips.bind(fixture.world));
    REQUIRE(resonate::ui::updateInput(fixture.world, fixture.root, 100.0F, 80.0F, false));
    tooltips.advance(fixture.world, fixture.root, 100.0F, 80.0F, 0.25F);
    REQUIRE(shown(fixture.world, fixture.tip));
    REQUIRE(resonate::ui::layout(fixture.world, fixture.root, 320.0F, 240.0F));

    /* The tooltip now covers the pointer's neighbour; a hit there is the
       button that owns it, never the tooltip. */
    const auto* rect = fixture.world.get<resonate::ui::ComputedRect>(fixture.tip);
    REQUIRE(rect != nullptr);
    CHECK(resonate::ui::hitTest(fixture.world, fixture.root, rect->x + 2.0F, rect->y + 2.0F) ==
          fixture.action);
}
