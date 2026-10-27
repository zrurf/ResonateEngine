#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include <resonate/core/allocator.h>
#include <resonate/dom/document.h>
#include <resonate/ecs/world.h>
#include <resonate/hierarchy/tree.h>
#include <resonate/ui/display_list.h>
#include <resonate/ui/document.h>
#include <resonate/ui/document_schema.h>
#include <resonate/ui/event.h>
#include <resonate/ui/hit_test.h>
#include <resonate/ui/input.h>
#include <resonate/ui/layout.h>

#include "resonate.ui/components.gen.h"

namespace
{

using resonate::ecs::Entity;
using resonate::ecs::World;
using resonate::ui::DisplayList;
using resonate::ui::Input;
using resonate::ui::Event;
using resonate::ui::EventKind;
using resonate::ui::EventPhase;
using resonate::ui::Listener;
using resonate::ui::ScrollView;

const char* const kList = R"xml(<document version="1" schemas="resonate.ui">
  <Screen key="hud">
    <Scroll key="list" x="0" y="0" width="100" height="100" step="20"
            direction="Column" align="Stretch">
      <Button key="row0" height="20"/>
      <Button key="row1" height="20"/>
      <Button key="row2" height="20"/>
      <Button key="row3" height="20"/>
      <Button key="row4" height="20"/>
      <Button key="row5" height="20"/>
      <Button key="row6" height="20"/>
      <Button key="row7" height="20"/>
    </Scroll>
  </Screen>
</document>)xml";

bool near(float actual, float expected)
{
    return std::fabs(actual - expected) < 0.001F;
}

/* Records every handler call as "<phase><kind>" so a test can assert the order
   the tree routing produced. */
struct Recorder
{
    std::vector<std::string> calls;
    bool consume = false;
};

bool record(void* context, World&, Entity, EventPhase phase, Event& event)
{
    auto* recorder = static_cast<Recorder*>(context);
    const char phases[] = {'c', 't', 'b'};
    const char kinds[] = {'d', 'u', 'm', 'k', 'w'};
    recorder->calls.emplace_back(std::string{phases[static_cast<int>(phase)]} +
                                kinds[static_cast<int>(event.kind)]);
    return recorder->consume;
}

struct Fixture
{
    World world{resonate::systemAllocator()};
    Entity root{};
    Entity list{};
    std::vector<Entity> rows;

    Fixture()
    {
        REQUIRE(resonate::hierarchy::registerComponents(world));
        REQUIRE(resonate::ui::registerComponents(world));

        std::vector<resonate::dom::Diagnostic> diagnostics;
        auto document = resonate::dom::Document::parse(
            kList, &resonate::ui::documentSchema(), diagnostics);
        REQUIRE(document != nullptr);
        CHECK(diagnostics.empty());
        root = resonate::ui::instantiate(world, *document);
        REQUIRE(root.valid());
        REQUIRE(resonate::ui::layout(world, root, 200.0F, 200.0F));

        list = resonate::hierarchy::children(world, root)[0];
        const auto children = resonate::hierarchy::children(world, list);
        rows.assign(children.begin(), children.end());
        REQUIRE(rows.size() == 8U);
    }

    void addListener(Entity entity, Recorder& recorder, bool consume, std::uint16_t kinds)
    {
        REQUIRE(world.add(entity, Listener{record, &recorder, kinds}) != nullptr);
    }
};

} // namespace

TEST_CASE("ui event: a click routes capture, target and bubble", "[ui][event]")
{
    Fixture fixture;
    Recorder canvas_recorder;
    Recorder row_recorder;
    fixture.addListener(fixture.root, canvas_recorder, false, 0xFFFFU);
    fixture.addListener(fixture.rows[0], row_recorder, false, 0xFFFFU);

    /* Press and release on the first row: the click lands on the row, and the
       canvas sees it in both phases. */
    REQUIRE(resonate::ui::updateInput(fixture.world, fixture.root, 10.0F, 10.0F, true));
    REQUIRE(resonate::ui::updateInput(fixture.world, fixture.root, 10.0F, 10.0F, false));

    /* Down, up and the click itself at the pointer; the canvas wraps each in
       capture and bubble. */
    /* Each pointer sample also routes a move, so the sequence is
       move, down, move, up, click. */
    const std::vector<std::string> expected_row{"cm", "tm", "cd", "td", "cm", "tm",
                                                "cu", "tu", "ck", "tk"};
    const std::vector<std::string> expected_canvas{"cm", "bm", "cd", "bd", "cm", "bm",
                                                   "cu", "bu", "ck", "bk"};
    CHECK(row_recorder.calls == expected_row);
    CHECK(canvas_recorder.calls == expected_canvas);
}

TEST_CASE("ui event: a consuming handler stops the routing", "[ui][event]")
{
    Fixture fixture;
    Recorder canvas_recorder;
    Recorder row_recorder;
    canvas_recorder.consume = true;
    fixture.addListener(fixture.root, canvas_recorder, true, 0xFFFFU);
    fixture.addListener(fixture.rows[0], row_recorder, false, 0xFFFFU);

    Event event{};
    event.kind = EventKind::Click;
    event.x = 10.0F;
    event.y = 10.0F;
    CHECK(resonate::ui::dispatchEvent(fixture.world, fixture.root, event));
    CHECK(event.consumed);
    /* The canvas consumes in the capture phase, so the row never sees it. */
    CHECK(canvas_recorder.calls == std::vector<std::string>{"ck"});
    CHECK(row_recorder.calls.empty());
}

TEST_CASE("ui event: the wheel scrolls a viewport by whole rows", "[ui][event]")
{
    Fixture fixture;
    const ScrollView* view = fixture.world.get<ScrollView>(fixture.list);
    REQUIRE(view != nullptr);
    CHECK(near(view->content_height, 160.0F));
    CHECK(near(view->scroll, 0.0F));

    CHECK(resonate::ui::scrollInput(fixture.world, fixture.root, 10.0F, 50.0F, 1.0F));
    view = fixture.world.get<ScrollView>(fixture.list);
    CHECK(near(view->scroll, 20.0F));

    /* Three more detents reach the end of the content and clamp there. */
    for (int step = 0; step < 5; ++step)
    {
        (void)resonate::ui::scrollInput(fixture.world, fixture.root, 10.0F, 50.0F, 1.0F);
    }
    view = fixture.world.get<ScrollView>(fixture.list);
    CHECK(near(view->scroll, 60.0F));

    CHECK(resonate::ui::scrollInput(fixture.world, fixture.root, 10.0F, 50.0F, -1.0F));
    view = fixture.world.get<ScrollView>(fixture.list);
    CHECK(near(view->scroll, 40.0F));
}

TEST_CASE("ui event: a handler can take the wheel from the viewport", "[ui][event]")
{
    Fixture fixture;
    Recorder recorder;
    recorder.consume = true;
    fixture.addListener(fixture.list, recorder, true,
                        resonate::ui::eventBit(EventKind::Wheel));

    CHECK(resonate::ui::scrollInput(fixture.world, fixture.root, 10.0F, 50.0F, 1.0F));
    const ScrollView* view = fixture.world.get<ScrollView>(fixture.list);
    CHECK(near(view->scroll, 0.0F));
    /* Capture reaches the viewport before the row it contains. */
    CHECK(recorder.calls == std::vector<std::string>{"cw"});
}

TEST_CASE("ui scroll: the viewport clips paint and hit testing", "[ui][event]")
{
    Fixture fixture;
    REQUIRE(resonate::ui::scrollInput(fixture.world, fixture.root, 10.0F, 50.0F, 1.0F));
    REQUIRE(resonate::ui::layout(fixture.world, fixture.root, 200.0F, 200.0F));

    /* row0 scrolled above the viewport, row5 is the last one that still fits. */
    DisplayList list;
    REQUIRE(resonate::ui::buildDisplayList(fixture.world, fixture.root, list));
    /* Six row backgrounds plus the viewport's scrollbar thumb. */
    CHECK(list.size() == 7U);

    const Entity top = resonate::ui::hitTest(fixture.world, fixture.root, 10.0F, 5.0F);
    CHECK(top == fixture.rows[1]);
    const Entity bottom = resonate::ui::hitTest(fixture.world, fixture.root, 10.0F, 95.0F);
    CHECK(bottom == fixture.rows[5]);
    /* Below the viewport no row is reachable, even though row6 was laid out:
       the point belongs to the canvas above the list. */
    const Entity outside = resonate::ui::hitTest(fixture.world, fixture.root, 10.0F, 105.0F);
    CHECK(outside == fixture.root);
}

TEST_CASE("ui event: a touch drag scrolls the viewport and cancels the click",
          "[ui][event]")
{
    Fixture fixture;
    Recorder row_recorder;
    fixture.addListener(fixture.rows[0], row_recorder, false, 0xFFFFU);

    /* One detent first so the viewport can follow a downward finger. */
    REQUIRE(resonate::ui::scrollInput(fixture.world, fixture.root, 10.0F, 50.0F, 1.0F));

    /* Press on the first row and drag down in small steps; the motion passes
       the slop on the third sample and the viewport follows the finger. */
    REQUIRE(resonate::ui::updateInput(fixture.world, fixture.root, 10.0F, 10.0F, true));
    REQUIRE(resonate::ui::updateInput(fixture.world, fixture.root, 10.0F, 16.0F, true));
    REQUIRE(resonate::ui::updateInput(fixture.world, fixture.root, 10.0F, 22.0F, true));
    REQUIRE(resonate::ui::updateInput(fixture.world, fixture.root, 10.0F, 28.0F, true));
    const ScrollView* view = fixture.world.get<ScrollView>(fixture.list);
    /* Two post-slop samples, six pixels each, against the row step. */
    CHECK(near(view->scroll, 20.0F - 12.0F));

    /* The release neither activates nor clicks the row the press began on. */
    REQUIRE(resonate::ui::updateInput(fixture.world, fixture.root, 10.0F, 28.0F, false));
    const Input* row = fixture.world.get<Input>(fixture.rows[0]);
    REQUIRE(row != nullptr);
    CHECK_FALSE(row->activated);
    /* The drag suppresses up and click; the press itself went through before
       the motion passed the slop. */
    for (const std::string& call : row_recorder.calls)
    {
        CHECK(call.back() != 'u');
        CHECK(call.back() != 'k');
    }
}

TEST_CASE("ui event: a tap shorter than the slop still clicks", "[ui][event]")
{
    Fixture fixture;
    REQUIRE(resonate::ui::updateInput(fixture.world, fixture.root, 10.0F, 10.0F, true));
    REQUIRE(resonate::ui::updateInput(fixture.world, fixture.root, 13.0F, 13.0F, false));
    const Input* row = fixture.world.get<Input>(fixture.rows[0]);
    REQUIRE(row != nullptr);
    CHECK(row->activated);
}

TEST_CASE("ui event: the wheel and a drag scroll over plain content", "[ui][event]")
{
    const char* const kPlain = R"xml(<document version="1" schemas="resonate.ui">
  <Screen key="hud">
    <Scroll key="list" x="0" y="0" width="100" height="100"
            direction="Column" align="Stretch">
      <Label key="row" width="100" height="160" text="plain"/>
    </Scroll>
  </Screen>
</document>)xml";
    World world{resonate::systemAllocator()};
    REQUIRE(resonate::hierarchy::registerComponents(world));
    REQUIRE(resonate::ui::registerComponents(world));
    std::vector<resonate::dom::Diagnostic> diagnostics;
    auto document = resonate::dom::Document::parse(kPlain, &resonate::ui::documentSchema(),
                                                   diagnostics);
    REQUIRE(document != nullptr);
    CHECK(diagnostics.empty());
    const Entity root = resonate::ui::instantiate(world, *document);
    REQUIRE(root.valid());
    REQUIRE(resonate::ui::layout(world, root, 200.0F, 200.0F));
    const Entity list = resonate::hierarchy::children(world, root)[0];

    /* The row carries no Input, so no event path exists; the viewport still
       scrolls, driven by the chain from the widget under the point. Half a
       detent keeps the range below the clamp for the drag that follows. */
    CHECK(resonate::ui::scrollInput(world, root, 50.0F, 50.0F, 0.5F));
    const ScrollView* view = world.get<ScrollView>(list);
    CHECK(near(view->scroll, 24.0F));

    /* A drag upward moves the content down with the finger. */
    REQUIRE(resonate::ui::updateInput(world, root, 50.0F, 90.0F, true));
    REQUIRE(resonate::ui::updateInput(world, root, 50.0F, 80.0F, true));
    REQUIRE(resonate::ui::updateInput(world, root, 50.0F, 70.0F, true));
    view = world.get<ScrollView>(list);
    CHECK(near(view->scroll, 44.0F));
}

TEST_CASE("ui event: a click toggles a checkbox and draws its box", "[ui][event]")
{
    const char * const text = R"xml(<document version="1" schemas="resonate.ui">
  <Screen key="hud">
    <Label key="check" class="check" x="0" y="0" width="160" height="24" text="可见"
           font_size="13" padding_left="22">
      <Input/>
      <Checkbox/>
    </Label>
  </Screen>
</document>)xml";
    World world{resonate::systemAllocator()};
    REQUIRE(resonate::hierarchy::registerComponents(world));
    REQUIRE(resonate::ui::registerComponents(world));
    std::vector<resonate::dom::Diagnostic> diagnostics;
    auto document = resonate::dom::Document::parse(text, &resonate::ui::documentSchema(),
                                                   diagnostics);
    REQUIRE(document != nullptr);
    CHECK(diagnostics.empty());
    const Entity root = resonate::ui::instantiate(world, *document);
    REQUIRE(root.valid());
    REQUIRE(resonate::ui::layout(world, root, 200.0F, 200.0F));
    const Entity check = resonate::hierarchy::children(world, root)[0];

    const resonate::ui::Checkbox* checkbox = world.get<resonate::ui::Checkbox>(check);
    REQUIRE(checkbox != nullptr);
    CHECK_FALSE(checkbox->checked);

    /* An unchecked box is four outline quads. */
    const auto box_quads = [&]
    {
        resonate::ui::DisplayList list;
        REQUIRE(resonate::ui::buildDisplayList(world, root, list));
        std::size_t count = 0;
        for (const resonate::ui::Quad& quad : list.quads())
        {
            const bool inside_box = quad.x >= 4.0F && quad.x + quad.width <= 18.0F &&
                                    quad.width <= 14.0F;
            if (inside_box)
            {
                ++count;
            }
        }
        return count;
    };
    CHECK(box_quads() == 4U);

    /* A click toggles the state and the box grows its inner square. */
    REQUIRE(resonate::ui::updateInput(world, root, 10.0F, 12.0F, true));
    REQUIRE(resonate::ui::updateInput(world, root, 10.0F, 12.0F, false));
    checkbox = world.get<resonate::ui::Checkbox>(check);
    REQUIRE(checkbox != nullptr);
    CHECK(checkbox->checked);
    CHECK(box_quads() == 5U);

    /* A second click toggles it back. */
    REQUIRE(resonate::ui::updateInput(world, root, 10.0F, 12.0F, true));
    REQUIRE(resonate::ui::updateInput(world, root, 10.0F, 12.0F, false));
    CHECK_FALSE(world.get<resonate::ui::Checkbox>(check)->checked);
}

TEST_CASE("ui scroll: the viewport draws a scrollbar thumb", "[ui][event]")
{
    Fixture fixture;
    const resonate::ui::ComputedRect* rect =
        fixture.world.get<resonate::ui::ComputedRect>(fixture.list);
    REQUIRE(rect != nullptr);
    const auto thumbs = [&]
    {
        resonate::ui::DisplayList list;
        REQUIRE(resonate::ui::buildDisplayList(fixture.world, fixture.root, list));
        std::size_t count = 0;
        for (const resonate::ui::Quad& quad : list.quads())
        {
            if (quad.x + quad.width <= rect->x + rect->width &&
                quad.x >= rect->x + rect->width - 9.0F && quad.width == 6.0F)
            {
                ++count;
            }
        }
        return count;
    };
    CHECK(thumbs() == 1U);

    /* Scrolling moves the thumb down without changing its length. */
    REQUIRE(resonate::ui::scrollInput(fixture.world, fixture.root, 10.0F, 50.0F, 1.0F));
    resonate::ui::DisplayList list;
    REQUIRE(resonate::ui::buildDisplayList(fixture.world, fixture.root, list));
    float thumb_y = -1.0F;
    for (const resonate::ui::Quad& quad : list.quads())
    {
        if (quad.width == 6.0F && quad.x >= rect->x + rect->width - 9.0F)
        {
            thumb_y = quad.y - rect->y;
        }
    }
    CHECK(thumb_y > 2.0F);
}
