#include <catch2/catch_test_macros.hpp>

#include <cstdint>

#include <resonate/core/allocator.h>
#include <resonate/ecs/command_buffer.h>
#include <resonate/hierarchy/tree.h>
#include <resonate/ui/hit_test.h>
#include <resonate/ui/layout.h>

#include "resonate.ui/components.gen.h"

namespace
{

using resonate::ecs::CommandBuffer;
using resonate::ecs::Entity;
using resonate::ecs::World;
using resonate::ui::ComputedRect;
using resonate::ui::RectTransform;

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
        world.add(canvas, resonate::ui::Canvas{});
        world.add(canvas, ComputedRect{0.0F, 0.0F, 100.0F, 100.0F});
    }

    Entity add(Entity parent, float x, float y, float width, float height, int z = 0,
               bool with_rect = true)
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
        if (with_rect)
        {
            world.add(entity, ComputedRect{x, y, width, height});
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

TEST_CASE("ui hit test: the topmost widget at the point wins", "[ui][hit-test]")
{
    Fixture fixture;
    const Entity panel = fixture.add(fixture.canvas, 0.0F, 0.0F, 100.0F, 100.0F);
    const Entity first = fixture.add(panel, 10.0F, 10.0F, 30.0F, 30.0F, 0);
    const Entity second = fixture.add(panel, 20.0F, 20.0F, 30.0F, 30.0F, 1);
    fixture.finish();

    CHECK(resonate::ui::hitTest(fixture.world, fixture.canvas, 25.0F, 25.0F) == second);
    CHECK(resonate::ui::hitTest(fixture.world, fixture.canvas, 12.0F, 12.0F) == first);
    CHECK(resonate::ui::hitTest(fixture.world, fixture.canvas, 60.0F, 60.0F) == panel);
    CHECK_FALSE(resonate::ui::hitTest(fixture.world, fixture.canvas, 200.0F, 200.0F).valid());
}

TEST_CASE("ui hit test: a later sibling subtree covers an earlier one", "[ui][hit-test]")
{
    Fixture fixture;
    const Entity panel = fixture.add(fixture.canvas, 0.0F, 0.0F, 100.0F, 100.0F);
    const Entity first = fixture.add(panel, 0.0F, 0.0F, 50.0F, 50.0F, 0);
    const Entity nested = fixture.add(first, 20.0F, 20.0F, 10.0F, 10.0F, 0);
    const Entity second = fixture.add(panel, 25.0F, 25.0F, 25.0F, 25.0F, 1);
    fixture.finish();

    /* The second sibling is painted after the whole first subtree, so it is
       tested first; the nested widget still wins where the sibling is absent. */
    CHECK(resonate::ui::hitTest(fixture.world, fixture.canvas, 27.0F, 27.0F) == second);
    CHECK(resonate::ui::hitTest(fixture.world, fixture.canvas, 22.0F, 22.0F) == nested);
}

TEST_CASE("ui hit test: equal z keeps reversed hierarchy order", "[ui][hit-test]")
{
    Fixture fixture;
    const Entity panel = fixture.add(fixture.canvas, 0.0F, 0.0F, 100.0F, 100.0F);
    const Entity first = fixture.add(panel, 0.0F, 0.0F, 50.0F, 50.0F);
    const Entity second = fixture.add(panel, 0.0F, 0.0F, 50.0F, 50.0F);
    fixture.finish();

    CHECK(resonate::ui::hitTest(fixture.world, fixture.canvas, 10.0F, 10.0F) == second);
    (void)first;
}

TEST_CASE("ui hit test: a widget without a computed rect is transparent", "[ui][hit-test]")
{
    Fixture fixture;
    const Entity panel = fixture.add(fixture.canvas, 0.0F, 0.0F, 100.0F, 100.0F);
    fixture.add(panel, 10.0F, 10.0F, 30.0F, 30.0F, 0, false);
    fixture.finish();

    CHECK(resonate::ui::hitTest(fixture.world, fixture.canvas, 20.0F, 20.0F) == panel);
}

TEST_CASE("ui hit test: a root without a rect answers nothing", "[ui][hit-test]")
{
    Fixture fixture;
    const Entity root = fixture.world.create();
    REQUIRE(root.valid());
    fixture.world.add(root, RectTransform{});

    CHECK_FALSE(resonate::ui::hitTest(fixture.world, root, 10.0F, 10.0F).valid());
}
