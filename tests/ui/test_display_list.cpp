#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <vector>

#include <resonate/core/allocator.h>
#include <resonate/ecs/command_buffer.h>
#include <resonate/hierarchy/tree.h>
#include <resonate/ui/display_list.h>
#include <resonate/ui/layout.h>

#include "resonate.ui/components.gen.h"

namespace
{

using resonate::ecs::CommandBuffer;
using resonate::ecs::Entity;
using resonate::ecs::World;
using resonate::ui::Background;
using resonate::ui::ComputedRect;
using resonate::ui::DisplayList;
using resonate::ui::RectTransform;

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

struct Fixture
{
    World world{resonate::systemAllocator()};
    CommandBuffer commands{world};
    Entity root{};

    Fixture()
    {
        REQUIRE(resonate::hierarchy::registerComponents(world));
        REQUIRE(resonate::ui::registerComponents(world));
        root = world.create();
        REQUIRE(root.valid());
        world.add(root, resonate::ui::Canvas{});
        world.add(root, ComputedRect{});
        world.add(root, Background{resonate::Vec4{0.0F, 0.0F, 0.0F, 1.0F}});
    }

    Entity add(Entity parent, float x, float y, float width, float height, int z,
               resonate::Vec4 color, bool background = true)
    {
        const Entity entity = world.create();
        REQUIRE(entity.valid());
        RectTransform transform;
        transform.x = x;
        transform.y = y;
        transform.width = width;
        transform.height = height;
        transform.z = z;
        world.add(entity, transform);
        world.add(entity, ComputedRect{x, y, width, height});
        if (background)
        {
            world.add(entity, Background{color});
        }
        resonate::hierarchy::setParent(commands, entity, parent);
        return entity;
    }

    void finish()
    {
        world.play(commands);
    }
};

} // namespace

TEST_CASE("ui display list: parents paint before children and siblings by z", "[ui][display-list]")
{
    Fixture fixture;
    const Entity high = fixture.add(fixture.root, 0.0F, 0.0F, 10.0F, 10.0F, 5,
                                    resonate::Vec4{1.0F, 0.0F, 0.0F, 1.0F});
    const Entity low = fixture.add(fixture.root, 0.0F, 0.0F, 10.0F, 10.0F, 1,
                                   resonate::Vec4{0.0F, 1.0F, 0.0F, 1.0F});
    fixture.add(low, 0.0F, 0.0F, 5.0F, 5.0F, 0, resonate::Vec4{0.0F, 0.0F, 1.0F, 1.0F});
    fixture.finish();

    DisplayList list;
    REQUIRE(resonate::ui::buildDisplayList(fixture.world, fixture.root, list));

    /* Root (z is irrelevant at the top), then the z=1 sibling and its child,
       then the z=5 sibling. */
    const auto quads = list.quads();
    REQUIRE(quads.size() == 4U);
    CHECK(quads[0].color.x == 0.0F); /* root black */
    CHECK(quads[1].color.y == 1.0F); /* low green */
    CHECK(quads[2].color.z == 1.0F); /* low's blue child */
    CHECK(quads[3].color.x == 1.0F); /* high red */
    (void)high;
}

TEST_CASE("ui display list: equal z keeps hierarchy order", "[ui][display-list]")
{
    Fixture fixture;
    fixture.add(fixture.root, 0.0F, 0.0F, 10.0F, 10.0F, 0, resonate::Vec4{1.0F, 0.0F, 0.0F, 1.0F});
    fixture.add(fixture.root, 0.0F, 0.0F, 10.0F, 10.0F, 0, resonate::Vec4{0.0F, 1.0F, 0.0F, 1.0F});
    fixture.finish();

    DisplayList list;
    REQUIRE(resonate::ui::buildDisplayList(fixture.world, fixture.root, list));
    const auto quads = list.quads();
    REQUIRE(quads.size() == 3U);
    CHECK(quads[1].color.x == 1.0F);
    CHECK(quads[2].color.y == 1.0F);
}

TEST_CASE("ui display list: a widget without Background still paints its children",
          "[ui][display-list]")
{
    Fixture fixture;
    const Entity group = fixture.add(fixture.root, 0.0F, 0.0F, 10.0F, 10.0F, 0,
                                     resonate::Vec4{}, false);
    fixture.add(group, 0.0F, 0.0F, 5.0F, 5.0F, 0, resonate::Vec4{0.0F, 0.0F, 1.0F, 1.0F});
    fixture.finish();

    DisplayList list;
    REQUIRE(resonate::ui::buildDisplayList(fixture.world, fixture.root, list));
    const auto quads = list.quads();
    REQUIRE(quads.size() == 2U);
    CHECK(quads[1].color.z == 1.0F);
}

TEST_CASE("ui display list: a background widget without a rect is refused", "[ui][display-list]")
{
    Reports reports;
    Fixture fixture;
    fixture.world.setReportSink(&reports, &Reports::sink);

    const Entity broken = fixture.world.create();
    REQUIRE(broken.valid());
    fixture.world.add(broken, Background{resonate::Vec4{1.0F, 1.0F, 1.0F, 1.0F}});
    resonate::hierarchy::setParent(fixture.commands, broken, fixture.root);
    fixture.finish();

    DisplayList list;
    CHECK_FALSE(resonate::ui::buildDisplayList(fixture.world, fixture.root, list));
    CHECK(reports.contains("no ComputedRect"));
}

TEST_CASE("ui display list: a repeated build reuses the same order", "[ui][display-list]")
{
    Fixture fixture;
    fixture.add(fixture.root, 0.0F, 0.0F, 10.0F, 10.0F, 2, resonate::Vec4{1.0F, 0.0F, 0.0F, 1.0F});
    fixture.add(fixture.root, 0.0F, 0.0F, 10.0F, 10.0F, 0, resonate::Vec4{0.0F, 1.0F, 0.0F, 1.0F});
    fixture.finish();

    DisplayList first;
    DisplayList second;
    REQUIRE(resonate::ui::buildDisplayList(fixture.world, fixture.root, first));
    REQUIRE(resonate::ui::buildDisplayList(fixture.world, fixture.root, second));
    REQUIRE(first.size() == second.size());
    for (std::size_t index = 0; index < first.size(); ++index)
    {
        CHECK(first.quads()[index].x == second.quads()[index].x);
        CHECK(first.quads()[index].color.y == second.quads()[index].color.y);
    }
}
