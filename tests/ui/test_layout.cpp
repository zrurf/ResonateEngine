#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <resonate/core/allocator.h>
#include <resonate/ecs/command_buffer.h>
#include <resonate/hierarchy/tree.h>
#include <resonate/dom/document.h>
#include <resonate/ui/document.h>
#include <resonate/ui/document_schema.h>
#include <resonate/ui/layout.h>

#include "resonate.ui/components.gen.h"

namespace
{

using resonate::ecs::CommandBuffer;
using resonate::ecs::ComponentIndex;
using resonate::ecs::Entity;
using resonate::ecs::World;
using resonate::ui::Align;
using resonate::ui::Canvas;
using resonate::ui::ComputedRect;
using resonate::ui::FlexDirection;
using resonate::ui::Justify;
using resonate::ui::LayoutGroup;
using resonate::ui::LayoutItem;
using resonate::ui::PositionMode;
using resonate::ui::SelfAlign;
using resonate::ui::RectTransform;

bool near(float actual, float expected)
{
    return std::fabs(actual - expected) < 0.001F;
}

struct Reports
{
    std::vector<std::string> lines;

    static void sink(void* user_data, World::Report, const char* message)
    {
        static_cast<Reports*>(user_data)->lines.emplace_back(message != nullptr ? message : "");
    }

    [[nodiscard]] bool contains(const char* fragment) const
    {
        for (const std::string& line : lines)
        {
            if (line.find(fragment) != std::string::npos)
            {
                return true;
            }
        }
        return false;
    }
};

RectTransform sized(float x, float y, float width, float height)
{
    RectTransform rect;
    rect.x = x;
    rect.y = y;
    rect.width = width;
    rect.height = height;
    return rect;
}

RectTransform anchored(RectTransform rect, float min_x, float min_y, float max_x, float max_y)
{
    rect.anchor_min = resonate::Vec2{min_x, min_y};
    rect.anchor_max = resonate::Vec2{max_x, max_y};
    return rect;
}

struct Fixture
{
    World world{resonate::systemAllocator()};
    CommandBuffer commands{world};
    Entity canvas{};

    Fixture()
    {
        REQUIRE(resonate::hierarchy::registerComponents(world));
        REQUIRE(resonate::ui::registerComponents(world));
        canvas = world.create();
        REQUIRE(canvas.valid());
        Canvas root;
        world.add(canvas, root);
        world.add(canvas, ComputedRect{});
    }

    Entity add(Entity parent, RectTransform rect, const LayoutGroup* group = nullptr)
    {
        const Entity entity = world.create();
        REQUIRE(entity.valid());
        world.add(entity, rect);
        world.add(entity, ComputedRect{});
        if (group != nullptr)
        {
            world.add(entity, *group);
        }
        if (parent.valid())
        {
            resonate::hierarchy::setParent(commands, entity, parent);
        }
        return entity;
    }

    void finish()
    {
        world.play(commands);
    }

    void requireRect(Entity entity, float x, float y, float width, float height) const
    {
        const ComputedRect* rect = world.get<ComputedRect>(entity);
        REQUIRE(rect != nullptr);
        CHECK(near(rect->x, x));
        CHECK(near(rect->y, y));
        CHECK(near(rect->width, width));
        CHECK(near(rect->height, height));
    }
};

} // namespace

TEST_CASE("ui layout: anchors place a widget in its parent", "[ui][layout]")
{
    Fixture fixture;
    const Entity plain = fixture.add(fixture.canvas, sized(10.0F, 20.0F, 30.0F, 40.0F));
    const Entity bottom_right = fixture.add(
        fixture.canvas, anchored(sized(-30.0F, -40.0F, 30.0F, 40.0F), 1.0F, 1.0F, 1.0F, 1.0F));
    const Entity stretched = fixture.add(
        fixture.canvas, anchored(sized(10.0F, 5.0F, -20.0F, -10.0F), 0.0F, 0.0F, 1.0F, 1.0F));
    RectTransform pivoted = sized(0.0F, 0.0F, 20.0F, 10.0F);
    pivoted.pivot = resonate::Vec2{0.5F, 0.5F};
    const Entity centered = fixture.add(fixture.canvas, pivoted);
    fixture.finish();

    REQUIRE(resonate::ui::layout(fixture.world, fixture.canvas, 200.0F, 100.0F));

    fixture.requireRect(plain, 10.0F, 20.0F, 30.0F, 40.0F);
    fixture.requireRect(bottom_right, 170.0F, 60.0F, 30.0F, 40.0F);
    fixture.requireRect(stretched, 10.0F, 5.0F, 180.0F, 90.0F);
    fixture.requireRect(centered, -10.0F, -5.0F, 20.0F, 10.0F);
}

TEST_CASE("ui layout: a row justifies and aligns its children", "[ui][layout]")
{
    Fixture fixture;
    LayoutGroup group;
    group.direction = FlexDirection::Row;
    group.justify = Justify::Center;
    group.align = Align::Center;
    group.gap = 5.0F;
    group.padding_left = 10.0F;
    group.padding_top = 10.0F;
    group.padding_right = 10.0F;
    group.padding_bottom = 10.0F;
    fixture.world.add(fixture.canvas, group);

    const Entity first = fixture.add(fixture.canvas, sized(0.0F, 0.0F, 50.0F, 20.0F));
    const Entity second = fixture.add(fixture.canvas, sized(0.0F, 0.0F, 30.0F, 40.0F));
    fixture.finish();

    REQUIRE(resonate::ui::layout(fixture.world, fixture.canvas, 300.0F, 100.0F));

    fixture.requireRect(first, 107.5F, 40.0F, 50.0F, 20.0F);
    fixture.requireRect(second, 162.5F, 30.0F, 30.0F, 40.0F);
}

TEST_CASE("ui layout: a column stretches its children across the cross axis", "[ui][layout]")
{
    Fixture fixture;
    LayoutGroup group;
    group.direction = FlexDirection::Column;
    group.align = Align::Stretch;
    fixture.world.add(fixture.canvas, group);

    const Entity first = fixture.add(fixture.canvas, sized(0.0F, 0.0F, 10.0F, 30.0F));
    const Entity second = fixture.add(fixture.canvas, sized(0.0F, 0.0F, 10.0F, 50.0F));
    fixture.finish();

    REQUIRE(resonate::ui::layout(fixture.world, fixture.canvas, 100.0F, 200.0F));

    fixture.requireRect(first, 0.0F, 0.0F, 100.0F, 30.0F);
    fixture.requireRect(second, 0.0F, 30.0F, 100.0F, 50.0F);
}

TEST_CASE("ui layout: an absolute child keeps its anchors and skips the flow", "[ui][layout]")
{
    Fixture fixture;
    LayoutGroup group;
    group.direction = FlexDirection::Row;
    group.gap = 10.0F;
    fixture.world.add(fixture.canvas, group);

    const Entity first = fixture.add(fixture.canvas, sized(0.0F, 0.0F, 20.0F, 10.0F));
    RectTransform absolute = anchored(sized(-10.0F, -10.0F, 10.0F, 10.0F), 1.0F, 1.0F, 1.0F, 1.0F);
    absolute.position = PositionMode::Absolute;
    const Entity pinned = fixture.add(fixture.canvas, absolute);
    const Entity second = fixture.add(fixture.canvas, sized(0.0F, 0.0F, 30.0F, 10.0F));
    fixture.finish();

    REQUIRE(resonate::ui::layout(fixture.world, fixture.canvas, 100.0F, 100.0F));

    /* Pinned does not consume the row's cursor: the second flow child starts
       after the first and its gap. */
    fixture.requireRect(first, 0.0F, 0.0F, 20.0F, 10.0F);
    fixture.requireRect(pinned, 90.0F, 90.0F, 10.0F, 10.0F);
    fixture.requireRect(second, 30.0F, 0.0F, 30.0F, 10.0F);
}

TEST_CASE("ui layout: canvas scale converts logical rects to target pixels", "[ui][layout]")
{
    Fixture fixture;
    const Entity child = fixture.add(fixture.canvas, sized(10.0F, 5.0F, 20.0F, 10.0F));
    fixture.finish();

    Canvas scaled;
    scaled.scale = 2.0F;
    *fixture.world.get<Canvas>(fixture.canvas) = scaled;

    REQUIRE(resonate::ui::layout(fixture.world, fixture.canvas, 200.0F, 100.0F));

    fixture.requireRect(fixture.canvas, 0.0F, 0.0F, 200.0F, 100.0F);
    fixture.requireRect(child, 20.0F, 10.0F, 40.0F, 20.0F);
}

TEST_CASE("ui layout: only moved rectangles are marked changed", "[ui][layout]")
{
    Fixture fixture;
    const Entity child = fixture.add(fixture.canvas, sized(10.0F, 5.0F, 20.0F, 10.0F));
    fixture.finish();

    int changes = 0;
    const auto observer = [](void* user_data, World&, Entity, ComponentIndex)
    { ++*static_cast<int*>(user_data); };
    REQUIRE(fixture.world.addObserver<ComputedRect>(&changes, observer));

    REQUIRE(resonate::ui::layout(fixture.world, fixture.canvas, 200.0F, 100.0F));
    CHECK(changes == 2);

    changes = 0;
    REQUIRE(resonate::ui::layout(fixture.world, fixture.canvas, 200.0F, 100.0F));
    CHECK(changes == 0);

    /* Moving one widget stamps only that widget; its neighbour stays clean. */
    fixture.world.get<RectTransform>(child)->x = 30.0F;
    REQUIRE(resonate::ui::layout(fixture.world, fixture.canvas, 200.0F, 100.0F));
    CHECK(changes == 1);
}

TEST_CASE("ui layout: a widget without its transform is refused", "[ui][layout]")
{
    Reports reports;
    Fixture fixture;
    fixture.world.setReportSink(&reports, &Reports::sink);

    const Entity incomplete = fixture.world.create();
    REQUIRE(incomplete.valid());
    fixture.world.add(incomplete, ComputedRect{});
    resonate::hierarchy::setParent(fixture.commands, incomplete, fixture.canvas);
    fixture.finish();

    CHECK_FALSE(resonate::ui::layout(fixture.world, fixture.canvas, 200.0F, 100.0F));
    CHECK(reports.contains("RectTransform"));
}

TEST_CASE("ui layout: a root without a canvas is refused", "[ui][layout]")
{
    Reports reports;
    Fixture fixture;
    fixture.world.setReportSink(&reports, &Reports::sink);

    const Entity entity = fixture.world.create();
    REQUIRE(entity.valid());
    fixture.world.add(entity, ComputedRect{});

    CHECK_FALSE(resonate::ui::layout(fixture.world, entity, 200.0F, 100.0F));
    CHECK(reports.contains("not a canvas"));
}

TEST_CASE("ui layout: nested containers lay out inside their parent's content box", "[ui][layout]")
{
    Fixture fixture;
    LayoutGroup column;
    column.direction = FlexDirection::Column;
    column.align = Align::Stretch;
    column.padding_left = 10.0F;
    column.padding_top = 10.0F;
    column.padding_right = 10.0F;
    column.padding_bottom = 10.0F;
    fixture.world.add(fixture.canvas, column);

    LayoutGroup row;
    row.direction = FlexDirection::Row;
    row.gap = 5.0F;
    row.padding_left = 5.0F;
    row.padding_top = 5.0F;
    row.padding_right = 5.0F;
    row.padding_bottom = 5.0F;
    const Entity panel = fixture.add(fixture.canvas, sized(0.0F, 0.0F, 0.0F, 50.0F), &row);
    const Entity first = fixture.add(panel, sized(0.0F, 0.0F, 20.0F, 10.0F));
    const Entity second = fixture.add(panel, sized(0.0F, 0.0F, 20.0F, 10.0F));
    fixture.finish();

    REQUIRE(resonate::ui::layout(fixture.world, fixture.canvas, 200.0F, 200.0F));

    /* The column stretches the panel to the content width; the panel's own
       padding then moves the row's children in. */
    fixture.requireRect(panel, 10.0F, 10.0F, 180.0F, 50.0F);
    fixture.requireRect(first, 15.0F, 15.0F, 20.0F, 10.0F);
    fixture.requireRect(second, 40.0F, 15.0F, 20.0F, 10.0F);
}

TEST_CASE("ui layout: space-between spreads the free space over the gaps", "[ui][layout]")
{
    Fixture fixture;
    LayoutGroup row;
    row.justify = Justify::SpaceBetween;
    fixture.world.add(fixture.canvas, row);

    const Entity first = fixture.add(fixture.canvas, sized(0.0F, 0.0F, 10.0F, 10.0F));
    const Entity second = fixture.add(fixture.canvas, sized(0.0F, 0.0F, 10.0F, 10.0F));
    const Entity third = fixture.add(fixture.canvas, sized(0.0F, 0.0F, 10.0F, 10.0F));
    fixture.finish();

    REQUIRE(resonate::ui::layout(fixture.world, fixture.canvas, 100.0F, 10.0F));

    fixture.requireRect(first, 0.0F, 0.0F, 10.0F, 10.0F);
    fixture.requireRect(second, 45.0F, 0.0F, 10.0F, 10.0F);
    fixture.requireRect(third, 90.0F, 0.0F, 10.0F, 10.0F);
}

TEST_CASE("ui layout: a wrapping row flows onto further lines", "[ui][layout]")
{
    const char* const text = R"xml(<document version="1" schemas="resonate.ui">
  <Screen key="hud">
    <Panel key="flow" x="0" y="0" width="120" height="200" direction="Row" wrap="true"
           align="Start" gap="0">
      <Box key="a" width="50" height="20" color="1 0 0 1"/>
      <Box key="b" width="50" height="20" color="0 1 0 1"/>
      <Box key="c" width="50" height="20" color="0 0 1 1"/>
    </Panel>
  </Screen>
</document>)xml";
    std::vector<resonate::dom::Diagnostic> diagnostics;
    auto document = resonate::dom::Document::parse(text, &resonate::ui::documentSchema(),
                                                   diagnostics);
    REQUIRE(document != nullptr);
    CHECK(diagnostics.empty());
    World world{resonate::systemAllocator()};
    REQUIRE(resonate::hierarchy::registerComponents(world));
    REQUIRE(resonate::ui::registerComponents(world));
    const Entity root = resonate::ui::instantiate(world, *document);
    REQUIRE(root.valid());
    REQUIRE(resonate::ui::layout(world, root, 200.0F, 300.0F));
    const Entity flow = resonate::hierarchy::children(world, root)[0];
    const auto boxes = resonate::hierarchy::children(world, flow);
    REQUIRE(boxes.size() == 3U);
    const auto rect_of = [&](Entity entity)
    {
        const resonate::ui::ComputedRect* rect = world.get<resonate::ui::ComputedRect>(entity);
        REQUIRE(rect != nullptr);
        return *rect;
    };
    /* Two 50 px boxes share the first line; the third wraps below them. */
    CHECK(near(rect_of(boxes[0]).x, 0.0F));
    CHECK(near(rect_of(boxes[1]).x, 50.0F));
    CHECK(near(rect_of(boxes[2]).x, 0.0F));
    CHECK(near(rect_of(boxes[0]).y, 0.0F));
    CHECK(near(rect_of(boxes[1]).y, 0.0F));
    CHECK(near(rect_of(boxes[2]).y, 20.0F));
}

TEST_CASE("ui layout: grow hands the free space out in proportion", "[ui][layout]")
{
    Fixture fixture;
    LayoutGroup group;
    group.direction = FlexDirection::Row;
    fixture.world.add(fixture.canvas, group);

    const Entity fixed = fixture.add(fixture.canvas, sized(0.0F, 0.0F, 40.0F, 20.0F));
    const Entity grow_two = fixture.add(fixture.canvas, sized(0.0F, 0.0F, 20.0F, 20.0F));
    const Entity grow_one = fixture.add(fixture.canvas, sized(0.0F, 0.0F, 20.0F, 20.0F));
    fixture.world.add(grow_two, LayoutItem{2.0F});
    fixture.world.add(grow_one, LayoutItem{1.0F});
    fixture.finish();

    REQUIRE(resonate::ui::layout(fixture.world, fixture.canvas, 300.0F, 100.0F));

    fixture.requireRect(fixed, 0.0F, 0.0F, 40.0F, 20.0F);
    /* 300 - 80 = 220 free; two thirds to the first grower. */
    fixture.requireRect(grow_two, 40.0F, 0.0F, 166.6667F, 20.0F);
    fixture.requireRect(grow_one, 206.6667F, 0.0F, 93.3333F, 20.0F);
}

TEST_CASE("ui layout: grow freezes at a max and the rest redistributes", "[ui][layout]")
{
    Fixture fixture;
    LayoutGroup group;
    group.direction = FlexDirection::Row;
    fixture.world.add(fixture.canvas, group);

    const Entity capped = fixture.add(fixture.canvas, sized(0.0F, 0.0F, 20.0F, 20.0F));
    fixture.world.add(capped, LayoutItem{1.0F, 1.0F, SelfAlign::Auto, 0, 0, 0, 0, 0, 0, 100.0F});
    const Entity floored = fixture.add(fixture.canvas, sized(0.0F, 0.0F, 10.0F, 20.0F));
    fixture.world.add(floored, LayoutItem{1.0F});
    fixture.finish();

    REQUIRE(resonate::ui::layout(fixture.world, fixture.canvas, 300.0F, 100.0F));

    /* An even share would put both at 155; the cap freezes at 100 and the
       remaining space goes to the other grower from its base. */
    fixture.requireRect(capped, 0.0F, 0.0F, 100.0F, 20.0F);
    fixture.requireRect(floored, 100.0F, 0.0F, 200.0F, 20.0F);
}

TEST_CASE("ui layout: min and max clamp the authored size before the flow", "[ui][layout]")
{
    Fixture fixture;
    LayoutGroup group;
    group.direction = FlexDirection::Row;
    group.gap = 10.0F;
    fixture.world.add(fixture.canvas, group);

    const Entity raised = fixture.add(fixture.canvas, sized(0.0F, 0.0F, 10.0F, 20.0F));
    fixture.world.add(raised, LayoutItem{0.0F, 1.0F, SelfAlign::Auto, 0, 0, 0, 0, 150.0F});
    const Entity lowered = fixture.add(fixture.canvas, sized(0.0F, 0.0F, 200.0F, 20.0F));
    fixture.world.add(lowered, LayoutItem{0.0F, 1.0F, SelfAlign::Auto, 0, 0, 0, 0, 0, 0, 60.0F});
    fixture.finish();

    REQUIRE(resonate::ui::layout(fixture.world, fixture.canvas, 300.0F, 100.0F));

    fixture.requireRect(raised, 0.0F, 0.0F, 150.0F, 20.0F);
    fixture.requireRect(lowered, 160.0F, 0.0F, 60.0F, 20.0F);
}

TEST_CASE("ui layout: margins sit between the padding and the item", "[ui][layout]")
{
    Fixture fixture;
    LayoutGroup group;
    group.direction = FlexDirection::Row;
    fixture.world.add(fixture.canvas, group);

    const Entity item = fixture.add(fixture.canvas, sized(0.0F, 0.0F, 50.0F, 20.0F));
    fixture.world.add(item, LayoutItem{0.0F, 1.0F, SelfAlign::Auto, 10.0F, 4.0F, 6.0F, 8.0F});
    fixture.finish();

    REQUIRE(resonate::ui::layout(fixture.world, fixture.canvas, 300.0F, 100.0F));

    fixture.requireRect(item, 10.0F, 4.0F, 50.0F, 20.0F);
}

TEST_CASE("ui layout: align_self overrides the container's cross alignment", "[ui][layout]")
{
    Fixture fixture;
    LayoutGroup group;
    group.direction = FlexDirection::Row;
    group.align = Align::Stretch;
    group.gap = 10.0F;
    fixture.world.add(fixture.canvas, group);

    const Entity stretched = fixture.add(fixture.canvas, sized(0.0F, 0.0F, 50.0F, 20.0F));
    const Entity centred = fixture.add(fixture.canvas, sized(0.0F, 0.0F, 50.0F, 20.0F));
    fixture.world.add(centred, LayoutItem{0.0F, 1.0F, SelfAlign::Center, 0, 0, 0, 0});
    const Entity floored = fixture.add(fixture.canvas, sized(0.0F, 0.0F, 50.0F, 20.0F));
    fixture.world.add(floored,
                      LayoutItem{0.0F, 1.0F, SelfAlign::End, 0, 0, 0, 0, 0, 0, 0, 40.0F});
    fixture.finish();

    REQUIRE(resonate::ui::layout(fixture.world, fixture.canvas, 300.0F, 100.0F));

    fixture.requireRect(stretched, 0.0F, 0.0F, 50.0F, 100.0F);
    fixture.requireRect(centred, 60.0F, 40.0F, 50.0F, 20.0F);
    /* End alignment: stuck to the line's bottom; a cap only clamps, it never
       grows the item. */
    fixture.requireRect(floored, 120.0F, 80.0F, 50.0F, 20.0F);
}
