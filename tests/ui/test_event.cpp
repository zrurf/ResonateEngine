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
        resonate::ui::scrollInput(fixture.world, fixture.root, 10.0F, 50.0F, 1.0F);
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
    CHECK(list.size() == 6U);

    const Entity top = resonate::ui::hitTest(fixture.world, fixture.root, 10.0F, 5.0F);
    CHECK(top == fixture.rows[1]);
    const Entity bottom = resonate::ui::hitTest(fixture.world, fixture.root, 10.0F, 95.0F);
    CHECK(bottom == fixture.rows[5]);
    /* Below the viewport no row is reachable, even though row6 was laid out:
       the point belongs to the canvas above the list. */
    const Entity outside = resonate::ui::hitTest(fixture.world, fixture.root, 10.0F, 105.0F);
    CHECK(outside == fixture.root);
}
