#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include <resonate/core/allocator.h>
#include <resonate/dom/document.h>
#include <resonate/ecs/world.h>
#include <resonate/hierarchy/tree.h>
#include <resonate/ui/document.h>
#include <resonate/ui/document_schema.h>
#include <resonate/ui/input.h>
#include <resonate/ui/layout.h>

#include "resonate.ui/components.gen.h"

namespace
{

using resonate::ecs::Entity;
using resonate::ecs::World;
using resonate::ui::Background;
using resonate::ui::Input;
using resonate::ui::PointerState;
using resonate::ui::Slider;
using resonate::ui::StateStyles;
using resonate::ui::WidgetState;

const char* const kButtons = R"xml(<document version="1" schemas="resonate.ui">
  <Screen key="hud">
    <StyleSheet key="styles">
      <StyleRule selector=".action"><Background color="0.2 0.2 0.2 1"/></StyleRule>
      <StyleRule selector=".action:hover"><Background color="0.4 0.4 0.4 1"/></StyleRule>
      <StyleRule selector=".action:pressed"><Background color="0.6 0.6 0.6 1"/></StyleRule>
      <StyleRule selector=".action:disabled"><Background color="0.8 0.8 0.8 1"/></StyleRule>
      <StyleRule selector=".action:focus"><Background color="0.7 0.7 0.7 1"/></StyleRule>
    </StyleSheet>
    <Panel key="panel" x="0" y="0" width="200" height="200"
           direction="Column" align="Start">
      <Button key="plain" class="action" width="100" height="40"/>
      <Button key="nested" class="action" width="100" height="40">
        <Label key="inner" width="100" height="40"/>
      </Button>
      <Button key="disabled" class="action" width="100" height="40">
        <Input focusable="false"/>
      </Button>
      <Button key="fixed" class="action" width="100" height="40">
        <Background color="0.1 0.2 0.3 1"/>
      </Button>
    </Panel>
  </Screen>
</document>)xml";

bool near(float actual, float expected)
{
    return std::fabs(actual - expected) < 0.001F;
}

struct Fixture
{
    World world{resonate::systemAllocator()};
    Entity root{};
    std::vector<Entity> buttons;

    Fixture()
    {
        REQUIRE(resonate::hierarchy::registerComponents(world));
        REQUIRE(resonate::ui::registerComponents(world));

        std::vector<resonate::dom::Diagnostic> diagnostics;
        auto document = resonate::dom::Document::parse(kButtons,
                                                       &resonate::ui::documentSchema(),
                                                       diagnostics);
        REQUIRE(document != nullptr);
        CHECK(diagnostics.empty());
        root = resonate::ui::instantiate(world, *document);
        REQUIRE(root.valid());
        REQUIRE(resonate::ui::layout(world, root, 200.0F, 200.0F));

        const auto panel = resonate::hierarchy::children(world, root)[0];
        const auto children = resonate::hierarchy::children(world, panel);
        buttons.assign(children.begin(), children.end());
        REQUIRE(buttons.size() == 4U);
    }

    [[nodiscard]] float red(Entity entity) const
    {
        const Background* background = world.get<Background>(entity);
        REQUIRE(background != nullptr);
        return background->color.x;
    }
};

} // namespace

TEST_CASE("ui input: hover, press and release drive state styles", "[ui][input]")
{
    Fixture fixture;
    const Entity button = fixture.buttons[0];
    REQUIRE(fixture.world.get<StateStyles>(button) != nullptr);
    CHECK(near(fixture.red(button), 0.2F));

    REQUIRE(resonate::ui::updateInput(fixture.world, fixture.root, 10.0F, 10.0F, false));
    const Input* input = fixture.world.get<Input>(button);
    REQUIRE(input != nullptr);
    CHECK(input->state == WidgetState::Hovered);
    CHECK(near(fixture.red(button), 0.4F));
    const PointerState* pointer = fixture.world.get<PointerState>(fixture.root);
    REQUIRE(pointer != nullptr);
    CHECK(pointer->hover == button);

    REQUIRE(resonate::ui::updateInput(fixture.world, fixture.root, 10.0F, 10.0F, true));
    CHECK(input->state == WidgetState::Pressed);
    CHECK(near(fixture.red(button), 0.6F));
    CHECK(pointer->press == button);
    CHECK(pointer->down);

    REQUIRE(resonate::ui::updateInput(fixture.world, fixture.root, 10.0F, 10.0F, false));
    CHECK(input->state == WidgetState::Hovered);
    CHECK(near(fixture.red(button), 0.4F));
    CHECK(input->activated);
    CHECK(pointer->activated == button);
    CHECK_FALSE(pointer->down);
    CHECK_FALSE(pointer->press.valid());

    /* Activation is a one-update pulse. */
    REQUIRE(resonate::ui::updateInput(fixture.world, fixture.root, 10.0F, 10.0F, false));
    CHECK_FALSE(input->activated);
    CHECK_FALSE(pointer->activated.valid());
}

TEST_CASE("ui input: leaving while pressed cancels the release", "[ui][input]")
{
    Fixture fixture;
    const Entity button = fixture.buttons[0];

    REQUIRE(resonate::ui::updateInput(fixture.world, fixture.root, 10.0F, 10.0F, false));
    REQUIRE(resonate::ui::updateInput(fixture.world, fixture.root, 10.0F, 10.0F, true));
    /* The long travel is a drag, so the pending press is dropped and the
       release neither activates nor clicks; the widget keeps focus, not the
       pressed style. */
    REQUIRE(resonate::ui::updateInput(fixture.world, fixture.root, 180.0F, 180.0F, true));
    CHECK(fixture.world.get<Input>(button)->state == WidgetState::Focused);
    CHECK(near(fixture.red(button), 0.7F));
    const PointerState* pointer = fixture.world.get<PointerState>(fixture.root);
    REQUIRE(pointer != nullptr);
    CHECK_FALSE(pointer->press.valid());
    CHECK(pointer->down);

    REQUIRE(resonate::ui::updateInput(fixture.world, fixture.root, 180.0F, 180.0F, false));
    CHECK_FALSE(fixture.world.get<Input>(button)->activated);
    CHECK_FALSE(pointer->press.valid());
    CHECK_FALSE(pointer->down);
}

TEST_CASE("ui input: a child hit belongs to its Input ancestor", "[ui][input]")
{
    Fixture fixture;
    const Entity button = fixture.buttons[1];
    REQUIRE(resonate::ui::updateInput(fixture.world, fixture.root, 10.0F, 50.0F, false));
    const PointerState* pointer = fixture.world.get<PointerState>(fixture.root);
    REQUIRE(pointer != nullptr);
    CHECK(pointer->hover == button);
    CHECK(fixture.world.get<Input>(button)->state == WidgetState::Hovered);
    CHECK(near(fixture.red(button), 0.4F));
}

TEST_CASE("ui input: a disabled input blocks itself and its parent", "[ui][input]")
{
    Fixture fixture;
    const Entity button = fixture.buttons[2];
    const Input* input = fixture.world.get<Input>(button);
    REQUIRE(input != nullptr);
    CHECK_FALSE(input->focusable);
    CHECK(input->state == WidgetState::Disabled);
    CHECK(near(fixture.red(button), 0.8F));

    REQUIRE(resonate::ui::updateInput(fixture.world, fixture.root, 10.0F, 90.0F, true));
    const PointerState* pointer = fixture.world.get<PointerState>(fixture.root);
    REQUIRE(pointer != nullptr);
    CHECK_FALSE(pointer->hover.valid());
    CHECK_FALSE(pointer->press.valid());
    CHECK(input->state == WidgetState::Disabled);
    CHECK(near(fixture.red(button), 0.8F));
}

TEST_CASE("ui input: an explicit field wins over a state style", "[ui][input]")
{
    Fixture fixture;
    const Entity button = fixture.buttons[3];
    REQUIRE(resonate::ui::updateInput(fixture.world, fixture.root, 10.0F, 130.0F, false));
    CHECK(fixture.world.get<Input>(button)->state == WidgetState::Hovered);
    CHECK(near(fixture.red(button), 0.1F));
    CHECK(near(fixture.world.get<Background>(button)->color.y, 0.2F));
}

TEST_CASE("ui input: Tab moves focus in tree order and wraps", "[ui][input]")
{
    Fixture fixture;
    const PointerState* pointer = fixture.world.get<PointerState>(fixture.root);
    REQUIRE(pointer != nullptr);

    REQUIRE(resonate::ui::focusNext(fixture.world, fixture.root, false));
    CHECK(pointer->focused == fixture.buttons[0]);
    CHECK(fixture.world.get<Input>(fixture.buttons[0])->state == WidgetState::Focused);
    CHECK(near(fixture.red(fixture.buttons[0]), 0.7F));

    /* The disabled button's non-focusable Input is skipped. */
    REQUIRE(resonate::ui::focusNext(fixture.world, fixture.root, false));
    CHECK(pointer->focused == fixture.buttons[1]);
    REQUIRE(resonate::ui::focusNext(fixture.world, fixture.root, false));
    CHECK(pointer->focused == fixture.buttons[3]);
    CHECK(fixture.world.get<Input>(fixture.buttons[1])->state == WidgetState::Normal);

    REQUIRE(resonate::ui::focusNext(fixture.world, fixture.root, false));
    CHECK(pointer->focused == fixture.buttons[0]);
    REQUIRE(resonate::ui::focusNext(fixture.world, fixture.root, true));
    CHECK(pointer->focused == fixture.buttons[3]);
}

TEST_CASE("ui input: focus survives leaving the widget", "[ui][input]")
{
    Fixture fixture;
    const Entity first = fixture.buttons[0];
    const Entity second = fixture.buttons[1];
    const PointerState* pointer = fixture.world.get<PointerState>(fixture.root);
    REQUIRE(pointer != nullptr);

    REQUIRE(resonate::ui::updateInput(fixture.world, fixture.root, 10.0F, 10.0F, true));
    REQUIRE(resonate::ui::updateInput(fixture.world, fixture.root, 10.0F, 10.0F, false));
    CHECK(pointer->focused == first);
    CHECK(fixture.world.get<Input>(first)->state == WidgetState::Hovered);

    /* Leaving a focused widget drops it to its focus style, not to normal. */
    REQUIRE(resonate::ui::updateInput(fixture.world, fixture.root, 180.0F, 180.0F, false));
    CHECK(fixture.world.get<Input>(first)->state == WidgetState::Focused);
    CHECK(near(fixture.red(first), 0.7F));

    /* Pressing another widget moves focus; the previous one returns to
       normal even though it is no longer hovered. */
    REQUIRE(resonate::ui::updateInput(fixture.world, fixture.root, 10.0F, 50.0F, true));
    CHECK(pointer->focused == second);
    CHECK(fixture.world.get<Input>(first)->state == WidgetState::Normal);
    CHECK(near(fixture.red(first), 0.2F));
    REQUIRE(resonate::ui::updateInput(fixture.world, fixture.root, 10.0F, 50.0F, false));
    CHECK(fixture.world.get<Input>(second)->state == WidgetState::Hovered);

    REQUIRE(resonate::ui::updateInput(fixture.world, fixture.root, 180.0F, 180.0F, false));
    CHECK(fixture.world.get<Input>(second)->state == WidgetState::Focused);
    CHECK(near(fixture.red(second), 0.7F));
}

TEST_CASE("ui input: a slider jumps to a press and follows a drag", "[ui][input]")
{
    const char* const text = R"xml(<document version="1" schemas="resonate.ui">
  <Screen key="hud">
    <Label key="slider" x="0" y="0" width="200" height="20">
      <Input/>
      <Slider min="0" max="100" value="0"/>
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
    const Entity slider_entity = resonate::hierarchy::children(world, root)[0];

    const auto value = [&]
    {
        const resonate::ui::Slider* slider = world.get<resonate::ui::Slider>(slider_entity);
        REQUIRE(slider != nullptr);
        return slider->value;
    };

    /* A press at a quarter of the track jumps the value there. */
    REQUIRE(resonate::ui::updateInput(world, root, 50.0F, 10.0F, true));
    CHECK(near(value(), 25.0F));

    /* The gesture owns the drag: the value follows the pointer. */
    REQUIRE(resonate::ui::updateInput(world, root, 150.0F, 10.0F, true));
    CHECK(near(value(), 75.0F));
    REQUIRE(resonate::ui::updateInput(world, root, 200.0F, 10.0F, true));
    CHECK(near(value(), 100.0F));

    /* The release neither activates the widget nor clicks it. */
    REQUIRE(resonate::ui::updateInput(world, root, 200.0F, 10.0F, false));
    const resonate::ui::Input* input = world.get<resonate::ui::Input>(slider_entity);
    REQUIRE(input != nullptr);
    CHECK_FALSE(input->activated);
    CHECK(near(value(), 100.0F));
}

TEST_CASE("ui input: dragging a splitter resizes the panes beside it", "[ui][input]")
{
    const char* const text = R"xml(<document version="1" schemas="resonate.ui">
  <Screen key="hud">
    <Box key="row" x="0" y="0" width="1000" height="100">
      <Panel key="left" x="0" y="0" width="300" height="100" color="0.1 0.1 0.1 1"/>
      <Box key="handle" x="300" y="0" width="6" height="100" color="0.2 0.2 0.2 1">
        <Splitter axis="0"/>
      </Box>
      <Panel key="right" x="306" y="0" width="694" height="100" color="0.1 0.1 0.1 1"/>
    </Box>
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
    REQUIRE(resonate::ui::layout(world, root, 1000.0F, 200.0F));
    const auto children = resonate::hierarchy::children(world, resonate::hierarchy::children(world, root)[0]);
    REQUIRE(children.size() == 3U);
    const Entity left = children[0];
    const Entity handle = children[1];
    const Entity right = children[2];

    const auto width_of = [&](Entity entity)
    {
        const resonate::ui::RectTransform* rect =
            world.get<resonate::ui::RectTransform>(entity);
        REQUIRE(rect != nullptr);
        return rect->width;
    };

    /* A press on the divider owns the drag; moving right grows the left pane,
       moves the right one and the divider travels with the edge. */
    REQUIRE(resonate::ui::updateInput(world, root, 303.0F, 50.0F, true));
    REQUIRE(resonate::ui::updateInput(world, root, 343.0F, 50.0F, true));
    CHECK(near(width_of(left), 340.0F));
    CHECK(near(width_of(right), 654.0F));
    const resonate::ui::RectTransform* handle_rect = world.get<resonate::ui::RectTransform>(handle);
    REQUIRE(handle_rect != nullptr);
    CHECK(near(handle_rect->x, 340.0F));
    const resonate::ui::RectTransform* right_rect = world.get<resonate::ui::RectTransform>(right);
    REQUIRE(right_rect != nullptr);
    CHECK(near(right_rect->x, 346.0F));

    /* The drag follows each sample; the release ends it and holds the size. */
    REQUIRE(resonate::ui::updateInput(world, root, 323.0F, 50.0F, true));
    CHECK(near(width_of(left), 320.0F));
    REQUIRE(resonate::ui::updateInput(world, root, 323.0F, 50.0F, false));
    CHECK(near(width_of(left), 320.0F));
}

TEST_CASE("ui input: dragging a property label scrubs its value", "[ui][input]")
{
    const char* const text = R"xml(<document version="1" schemas="resonate.ui">
  <Screen key="hud">
    <Label key="scrub" x="0" y="0" width="160" height="22" text="不透明度"
           font_size="12">
      <Input/>
      <Scrubber step="0.5" value="10"/>
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
    const Entity scrub = resonate::hierarchy::children(world, root)[0];

    const auto value = [&]
    {
        const resonate::ui::Scrubber* scrubber = world.get<resonate::ui::Scrubber>(scrub);
        REQUIRE(scrubber != nullptr);
        return scrubber->value;
    };
    CHECK(near(value(), 10.0F));

    /* The press itself changes nothing; the drag moves by step per pixel. */
    REQUIRE(resonate::ui::updateInput(world, root, 40.0F, 10.0F, true));
    CHECK(near(value(), 10.0F));
    REQUIRE(resonate::ui::updateInput(world, root, 80.0F, 10.0F, true));
    CHECK(near(value(), 30.0F));
    REQUIRE(resonate::ui::updateInput(world, root, 60.0F, 10.0F, true));
    CHECK(near(value(), 20.0F));

    /* The release neither activates nor clicks the label. */
    REQUIRE(resonate::ui::updateInput(world, root, 60.0F, 10.0F, false));
    const resonate::ui::Input* input = world.get<resonate::ui::Input>(scrub);
    REQUIRE(input != nullptr);
    CHECK_FALSE(input->activated);
    CHECK(near(value(), 20.0F));
}

TEST_CASE("ui input: dragging the scrollbar thumb moves the scroll", "[ui][input]")
{
    const char* const text = R"xml(<document version="1" schemas="resonate.ui">
  <Screen key="hud">
    <Scroll key="list" x="0" y="0" width="100" height="100"
            direction="Column" align="Stretch">
      <Button key="row0" height="40" color="0.1 0.1 0.1 1"/>
      <Button key="row1" height="40" color="0.1 0.1 0.1 1"/>
      <Button key="row2" height="40" color="0.1 0.1 0.1 1"/>
      <Button key="row3" height="40" color="0.1 0.1 0.1 1"/>
    </Scroll>
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
    const Entity list = resonate::hierarchy::children(world, root)[0];
    const resonate::ui::ScrollView* view = world.get<resonate::ui::ScrollView>(list);
    REQUIRE(view != nullptr);
    CHECK(near(view->content_height, 160.0F));
    CHECK(near(view->scroll, 0.0F));

    /* The thumb runs the right edge; pressing it and dragging down scrolls by
       the thumb's own travel ratio: 20 px against 40 px of travel over a 60 px
       range moves 30. */
    REQUIRE(resonate::ui::updateInput(world, root, 95.0F, 10.0F, true));
    REQUIRE(resonate::ui::updateInput(world, root, 95.0F, 30.0F, true));
    view = world.get<resonate::ui::ScrollView>(list);
    /* 20 px of drag over 37.5 px of travel covers the 60 px range. */
    CHECK(near(view->scroll, 32.0F));

    /* The row under the press was never activated by the thumb drag. */
    REQUIRE(resonate::ui::updateInput(world, root, 95.0F, 30.0F, false));
    const auto rows = resonate::hierarchy::children(world, list);
    REQUIRE(rows.size() == 4U);
    const resonate::ui::Input* first_row = world.get<resonate::ui::Input>(rows[0]);
    REQUIRE(first_row != nullptr);
    CHECK_FALSE(first_row->activated);
}

TEST_CASE("ui input: the cursor follows the widget and the gesture", "[ui][input]")
{
    const char* const document_text = R"xml(<document version="1" schemas="resonate.ui">
  <Screen key="hud">
    <Panel key="panel" x="0" y="0" width="200" height="200"
           direction="Column" align="Start">
      <Button key="plain" width="100" height="30"/>
      <Button key="off" width="100" height="30">
        <Input focusable="false"/>
      </Button>
      <Label key="field" width="100" height="30" text="abc" font_size="12">
        <Input/>
      </Label>
      <Label key="slider" width="100" height="30">
        <Input/>
        <Slider min="0" max="10" value="5"/>
      </Label>
    </Panel>
  </Screen>
</document>)xml";
    World world{resonate::systemAllocator()};
    REQUIRE(resonate::hierarchy::registerComponents(world));
    REQUIRE(resonate::ui::registerComponents(world));
    std::vector<resonate::dom::Diagnostic> diagnostics;
    auto document = resonate::dom::Document::parse(document_text,
                                                   &resonate::ui::documentSchema(),
                                                   diagnostics);
    REQUIRE(document != nullptr);
    CHECK(diagnostics.empty());
    const Entity root = resonate::ui::instantiate(world, *document);
    REQUIRE(root.valid());
    REQUIRE(resonate::ui::layout(world, root, 200.0F, 200.0F));

    /* Nothing under the pointer: the arrow. */
    REQUIRE(resonate::ui::updateInput(world, root, 250.0F, 250.0F, false));
    CHECK(resonate::ui::cursorShape(world, root) == resonate::ui::CursorShape::Default);

    /* A plain button: the pointing hand. */
    REQUIRE(resonate::ui::updateInput(world, root, 50.0F, 15.0F, false));
    CHECK(resonate::ui::cursorShape(world, root) == resonate::ui::CursorShape::Pointer);

    /* A non-focusable widget shields the pointer without hovering, so the
       shape falls back to the arrow. */
    REQUIRE(resonate::ui::updateInput(world, root, 50.0F, 45.0F, false));
    CHECK(resonate::ui::cursorShape(world, root) == resonate::ui::CursorShape::Default);

    /* A text field: the I-beam. */
    REQUIRE(resonate::ui::updateInput(world, root, 50.0F, 75.0F, false));
    CHECK(resonate::ui::cursorShape(world, root) == resonate::ui::CursorShape::Text);

    /* A slider being dragged: the horizontal resize, even though the pointer
       now sits away from the track. */
    REQUIRE(resonate::ui::updateInput(world, root, 50.0F, 105.0F, true));
    REQUIRE(resonate::ui::updateInput(world, root, 55.0F, 105.0F, true));
    CHECK(resonate::ui::cursorShape(world, root) == resonate::ui::CursorShape::ResizeHorizontal);
    REQUIRE(resonate::ui::updateInput(world, root, 55.0F, 105.0F, false));
    CHECK(resonate::ui::cursorShape(world, root) == resonate::ui::CursorShape::Pointer);
}
