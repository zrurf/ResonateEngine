#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <vector>

#include <resonate/core/allocator.h>
#include <resonate/dom/document.h>
#include <resonate/ui/document.h>
#include <resonate/ui/document_schema.h>
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

TEST_CASE("ui display list: draws carry the clip they are confined to", "[ui][display-list]")
{
    Fixture fixture;
    const Entity viewport = fixture.add(fixture.root, 0.0F, 0.0F, 100.0F, 100.0F, 0,
                                        resonate::Vec4{0.2F, 0.2F, 0.2F, 1.0F});
    fixture.world.add(viewport, resonate::ui::ScrollView{});
    const Entity inner = fixture.add(viewport, 10.0F, 10.0F, 20.0F, 20.0F, 0,
                                     resonate::Vec4{1.0F, 0.0F, 0.0F, 1.0F});
    (void)inner;
    fixture.finish();

    resonate::ui::DisplayList list;
    REQUIRE(resonate::ui::buildDisplayList(fixture.world, fixture.root, list));
    REQUIRE(list.quads().size() >= 3U);
    /* The canvas paints unclipped; the viewport and its content carry the
       viewport's rect. */
    CHECK(list.quads()[0].clip.width == 0.0F);
    std::size_t clipped = 0U;
    for (const resonate::ui::Quad& quad : list.quads())
    {
        if (quad.clip.width == 100.0F)
        {
            ++clipped;
            CHECK(quad.clip.x == 0.0F);
            CHECK(quad.clip.y == 0.0F);
            CHECK(quad.clip.height == 100.0F);
        }
    }
    CHECK(clipped == 2U);
}

TEST_CASE("ui display list: a slider narrower than its knob paints safely", "[ui][display-list]")
{
    /* A zero or narrow track used to clamp to inverted bounds and trip the
       debug STL; the knob just parks at the track's left edge now. */
    Fixture fixture;
    const Entity track = fixture.add(fixture.root, 0.0F, 0.0F, 6.0F, 20.0F, 0,
                                     resonate::Vec4{0.2F, 0.2F, 0.2F, 1.0F});
    resonate::ui::Slider slider;
    slider.value = 0.5F;
    slider.min = 0.0F;
    slider.max = 1.0F;
    REQUIRE(fixture.world.add(track, slider) != nullptr);

    const Entity zero = fixture.add(fixture.root, 0.0F, 40.0F, 0.0F, 0.0F, 0,
                                    resonate::Vec4{0.2F, 0.2F, 0.2F, 1.0F});
    REQUIRE(fixture.world.add(zero, slider) != nullptr);
    fixture.finish();

    resonate::ui::DisplayList list;
    REQUIRE(resonate::ui::buildDisplayList(fixture.world, fixture.root, list));
    CHECK_FALSE(list.quads().empty());
}

TEST_CASE("ui display list: a focused empty field centres its caret", "[ui][display-list]")
{
    Fixture fixture;
    const Entity field = fixture.add(fixture.root, 10.0F, 20.0F, 120.0F, 26.0F, 0,
                                     resonate::Vec4{}, false);
    resonate::ui::Text text;
    text.font_size = 13.0F;
    text.v_align = resonate::ui::VAlign::Middle;
    fixture.world.add(field, text);
    fixture.world.add(field, resonate::ui::Input{});
    resonate::ui::PointerState pointer;
    pointer.focused = field;
    fixture.world.add(fixture.root, pointer);
    fixture.finish();

    DisplayList list;
    REQUIRE(resonate::ui::buildDisplayList(fixture.world, fixture.root, list));
    const auto quads = list.quads();
    /* Root's background, then the caret: an empty field has no glyph quads. */
    REQUIRE(quads.size() == 2U);
    const auto& caret = quads[1];
    CHECK(caret.width == 1.0F);
    /* The caret spans the fallback line box (0.8 + 0.2 of the font size) and
       sits centred in the box, not against its top. */
    CHECK(caret.height > 12.9F);
    CHECK(caret.height < 13.1F);
    CHECK(caret.y > 26.0F);
    CHECK(caret.y < 26.6F);
}

TEST_CASE("ui display list: a focused checkbox draws no caret", "[ui][display-list]")
{
    Fixture fixture;
    const Entity check = fixture.add(fixture.root, 10.0F, 20.0F, 120.0F, 24.0F, 0,
                                     resonate::Vec4{}, false);
    resonate::ui::Text text;
    text.font_size = 13.0F;
    fixture.world.add(check, text);
    fixture.world.add(check, resonate::ui::Input{});
    fixture.world.add(check, resonate::ui::Checkbox{});
    resonate::ui::PointerState pointer;
    pointer.focused = check;
    fixture.world.add(fixture.root, pointer);
    fixture.finish();

    DisplayList list;
    REQUIRE(resonate::ui::buildDisplayList(fixture.world, fixture.root, list));
    /* The root's background and the box's four outline bars; a caret would add
       a fifth column spanning the line box. */
    CHECK(list.quads().size() == 5U);
}

TEST_CASE("display list: pushEdge routes an orthogonal edge", "[ui][display]")
{
    resonate::ui::DisplayList list;
    const resonate::Vec4 color{1.0F, 0.5F, 0.0F, 1.0F};
    /* A left-to-right edge: out of the source at (100, 50), into the target
       at (300, 90). Three segments: horizontal, vertical, horizontal. */
    list.pushEdge(resonate::Vec2{100.0F, 50.0F}, resonate::Vec2{300.0F, 90.0F}, 4.0F,
                  color);
    const auto quads = list.quads();
    REQUIRE(quads.size() == 3U);
    /* Segment 1 spans the horizontal run at the source's y. */
    CHECK(quads[0].y + quads[0].height * 0.5F == 50.0F);
    CHECK(quads[0].x == 100.0F);
    CHECK(quads[0].width == 100.0F);
    /* Segment 2 is the vertical run at the midpoint. */
    CHECK(quads[1].x + quads[1].width * 0.5F == 200.0F);
    CHECK(quads[1].y == 50.0F);
    CHECK(quads[1].height == 40.0F);
    /* Segment 3 spans the horizontal run at the target's y. */
    CHECK(quads[2].y + quads[2].height * 0.5F == 90.0F);
    CHECK(quads[2].x == 200.0F);
    CHECK(quads[2].width == 100.0F);
    for (const auto& quad : quads)
    {
        CHECK((quad.height >= 4.0F || quad.width >= 4.0F));
        CHECK(quad.color.x == color.x);
        CHECK(quad.color.y == color.y);
        CHECK(quad.color.z == color.z);
        CHECK(quad.color.w == color.w);
    }

    /* A straight vertical drop needs only the vertical segment. */
    resonate::ui::DisplayList straight;
    straight.pushEdge(resonate::Vec2{50.0F, 0.0F}, resonate::Vec2{50.0F, 80.0F}, 2.0F,
                      color);
    CHECK(straight.quads().size() == 1U);
    CHECK(straight.quads()[0].height == 80.0F);
}

TEST_CASE("display list: decal quads paint over their subtree", "[ui][display]")
{
    const char* const text = R"xml(<document version="1" schemas="resonate.ui">
  <Screen key="hud">
    <Panel key="canvas" x="0" y="0" width="200" height="200" color="0.1 0.1 0.1 1">
      <Label key="mark" x="10" y="10" width="40" height="20" text="x" font_size="12"
             color="1 1 1 1"/>
    </Panel>
  </Screen>
</document>)xml";
    World world{resonate::systemAllocator()};
    REQUIRE(resonate::hierarchy::registerComponents(world));
    REQUIRE(resonate::ui::registerComponents(world));
    std::vector<resonate::dom::Diagnostic> diagnostics;
    auto document = resonate::dom::Document::parse(text, &resonate::ui::documentSchema(),
                                                   diagnostics);
    REQUIRE(document != nullptr);
    const Entity root = resonate::ui::instantiate(world, *document);
    REQUIRE(root.valid());
    REQUIRE(resonate::ui::layout(world, root, 400.0F, 400.0F));

    const Entity canvas = resonate::ui::findByKey(world, root, "canvas");
    REQUIRE(canvas.valid());
    resonate::ui::DisplayList plain;
    REQUIRE(resonate::ui::buildDisplayList(world, root, plain));
    const std::size_t without = plain.size();

    /* Two decal quads over the canvas: one covering it, one clipped away. The
       bytes are the raw Quad array the display list replays. */
    resonate::ui::Quad decals[2];
    decals[0].x = 0.0F;
    decals[0].y = 0.0F;
    decals[0].width = 200.0F;
    decals[0].height = 200.0F;
    decals[0].color = resonate::Vec4{0.0F, 1.0F, 0.0F, 1.0F};
    decals[1].x = 300.0F;
    decals[1].y = 300.0F;
    decals[1].width = 50.0F;
    decals[1].height = 50.0F;
    decals[1].color = resonate::Vec4{0.0F, 0.0F, 1.0F, 1.0F};
    const resonate::ecs::BlobHandle blob = world.createBlob(
        canvas, sizeof(decals), decals);
    REQUIRE(blob.valid());
    REQUIRE(world.add(canvas, resonate::ui::QuadDecals{blob}) != nullptr);

    resonate::ui::DisplayList decorated;
    REQUIRE(resonate::ui::buildDisplayList(world, root, decorated));
    CHECK(decorated.size() == without + 2U);
    /* Both decals replayed; the out-of-bounds one carries a clip that keeps it
       inside the canvas. */
    std::size_t green = 0U;
    std::size_t blue = 0U;
    for (const auto& quad : decorated.quads())
    {
        if (quad.color.y == 1.0F && quad.color.z == 0.0F && quad.width == 200.0F)
        {
            ++green;
        }
        if (quad.color.z == 1.0F && quad.color.y == 0.0F)
        {
            ++blue;
            CHECK(quad.clip.width <= 200.0F);
        }
    }
    CHECK(green == 1U);
    CHECK(blue == 1U);
}
