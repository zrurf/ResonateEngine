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
    const PointerState* pointer = fixture.world.get<PointerState>(fixture.root);
    REQUIRE(pointer != nullptr);

    REQUIRE(resonate::ui::updateInput(fixture.world, fixture.root, 10.0F, 10.0F, false));
    REQUIRE(resonate::ui::updateInput(fixture.world, fixture.root, 10.0F, 10.0F, true));
    REQUIRE(resonate::ui::updateInput(fixture.world, fixture.root, 180.0F, 180.0F, true));
    CHECK(fixture.world.get<Input>(button)->state == WidgetState::Focused);
    CHECK(near(fixture.red(button), 0.7F));
    CHECK(pointer->press == button);
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
