#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <resonate/core/allocator.h>
#include <resonate/ecs/command_buffer.h>
#include <resonate/hierarchy/tree.h>
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
using resonate::ui::PositionMode;
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
