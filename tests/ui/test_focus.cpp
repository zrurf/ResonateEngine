#include <catch2/catch_test_macros.hpp>

#include <cmath>
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

const char* const kDocument = R"xml(<document version="1" schemas="resonate.ui">
  <Screen key="screen">
    <StyleSheet key="styles">
      <StyleRule selector=".action"><Background color="0.2 0.2 0.2 1"/></StyleRule>
      <StyleRule selector=".action:focus"><Background color="0.7 0.7 0.7 1"/></StyleRule>
      <StyleRule selector=".action:focus-visible"><Background color="0.9 0.9 0.9 1"/></StyleRule>
    </StyleSheet>
    <Panel key="panel" x="0" y="0" width="200" height="200"
           direction="Column" align="Start">
      <Button key="first" class="action" width="100" height="40"/>
      <Button key="second" class="action" width="100" height="40"/>
      <Label key="volume" width="100" height="20">
        <Input/>
        <Slider min="0" max="100" value="50"/>
      </Label>
      <Panel key="dialog" x="50" y="50" width="100" height="100" position="Absolute"
             shown="false">
        <Modal/>
        <Button key="ok" class="action" width="80" height="30"/>
      </Panel>
    </Panel>
  </Screen>
</document>)xml";

struct Fixture
{
    World world{resonate::systemAllocator()};
    Entity root{};
    Entity first{};
    Entity second{};
    Entity volume{};
    Entity dialog{};
    Entity ok{};

    Fixture()
    {
        REQUIRE(resonate::hierarchy::registerComponents(world));
        REQUIRE(resonate::ui::registerComponents(world));
        std::vector<resonate::dom::Diagnostic> diagnostics;
        auto document = resonate::dom::Document::parse(kDocument,
                                                       &resonate::ui::documentSchema(),
                                                       diagnostics);
        REQUIRE(document != nullptr);
        REQUIRE(diagnostics.empty());
        root = resonate::ui::instantiate(world, *document);
        REQUIRE(root.valid());
        REQUIRE(resonate::ui::layout(world, root, 200.0F, 200.0F));
        first = resonate::ui::findByKey(world, root, "first");
        second = resonate::ui::findByKey(world, root, "second");
        volume = resonate::ui::findByKey(world, root, "volume");
        dialog = resonate::ui::findByKey(world, root, "dialog");
        ok = resonate::ui::findByKey(world, root, "ok");
        REQUIRE(first.valid());
        REQUIRE(second.valid());
        REQUIRE(volume.valid());
        REQUIRE(dialog.valid());
        REQUIRE(ok.valid());
    }

    void showDialog(bool shown)
    {
        auto* component = world.get<resonate::ui::Shown>(dialog);
        REQUIRE(component != nullptr);
        component->shown = shown;
        world.markChanged<resonate::ui::Shown>(dialog);
        REQUIRE(resonate::ui::layout(world, root, 200.0F, 200.0F));
    }

    [[nodiscard]] float red(Entity entity) const
    {
        const auto* background = world.get<resonate::ui::Background>(entity);
        REQUIRE(background != nullptr);
        return background->color.x;
    }
};

} // namespace

TEST_CASE("ui focus: keyboard focus wears :focus-visible, a click does not", "[ui][input]")
{
    Fixture fixture;

    /* Tab: the first button takes focus with the visible layer on. */
    REQUIRE(resonate::ui::focusNext(fixture.world, fixture.root, false));
    const auto* pointer = fixture.world.get<resonate::ui::PointerState>(fixture.root);
    REQUIRE(pointer != nullptr);
    CHECK(pointer->focused == fixture.first);
    CHECK(pointer->focus_visible);
    CHECK(fixture.world.get<resonate::ui::Input>(fixture.first)->state ==
          resonate::ui::WidgetState::FocusedVisible);
    CHECK(std::fabs(fixture.red(fixture.first) - 0.9F) < 0.001F);

    /* A press on the focused widget keeps the focus but drops the layer; the
       pressed state has no rule of its own, so the colour is the normal one
       while the button is held. */
    REQUIRE(resonate::ui::updateInput(fixture.world, fixture.root, 10.0F, 10.0F, true));
    pointer = fixture.world.get<resonate::ui::PointerState>(fixture.root);
    REQUIRE(pointer != nullptr);
    CHECK(pointer->focused == fixture.first);
    CHECK_FALSE(pointer->focus_visible);
    CHECK(fixture.world.get<resonate::ui::Input>(fixture.first)->state ==
          resonate::ui::WidgetState::Pressed);

    /* Released and left, the focus style is the plain one. */
    REQUIRE(resonate::ui::updateInput(fixture.world, fixture.root, 10.0F, 10.0F, false));
    REQUIRE(resonate::ui::updateInput(fixture.world, fixture.root, 180.0F, 180.0F, false));
    CHECK(fixture.world.get<resonate::ui::Input>(fixture.first)->state ==
          resonate::ui::WidgetState::Focused);
    CHECK(std::fabs(fixture.red(fixture.first) - 0.7F) < 0.001F);

    /* Tab again: the next widget takes keyboard focus. */
    REQUIRE(resonate::ui::focusNext(fixture.world, fixture.root, false));
    pointer = fixture.world.get<resonate::ui::PointerState>(fixture.root);
    CHECK(pointer->focused == fixture.second);
    CHECK(pointer->focus_visible);
    CHECK(std::fabs(fixture.red(fixture.second) - 0.9F) < 0.001F);
}

TEST_CASE("ui focus: arrow keys step a focused slider", "[ui][input]")
{
    Fixture fixture;
    REQUIRE(resonate::ui::updateInput(fixture.world, fixture.root, 50.0F, 90.0F, true));
    REQUIRE(resonate::ui::updateInput(fixture.world, fixture.root, 50.0F, 90.0F, false));
    const auto* pointer = fixture.world.get<resonate::ui::PointerState>(fixture.root);
    REQUIRE(pointer != nullptr);
    REQUIRE(pointer->focused == fixture.volume);

    auto* slider = fixture.world.get<resonate::ui::Slider>(fixture.volume);
    REQUIRE(slider != nullptr);
    CHECK(slider->value > 49.0F);
    const float start = slider->value;

    /* A twentieth of the range per press, clamped to the authored span. */
    CHECK(resonate::ui::stepSlider(fixture.world, fixture.root, true));
    slider = fixture.world.get<resonate::ui::Slider>(fixture.volume);
    CHECK(std::fabs(slider->value - (start + 5.0F)) < 0.01F);
    CHECK(resonate::ui::stepSlider(fixture.world, fixture.root, false));
    slider = fixture.world.get<resonate::ui::Slider>(fixture.volume);
    CHECK(std::fabs(slider->value - start) < 0.01F);

    slider->value = 98.0F;
    (void)resonate::ui::stepSlider(fixture.world, fixture.root, true);
    slider = fixture.world.get<resonate::ui::Slider>(fixture.volume);
    CHECK(std::fabs(slider->value - 100.0F) < 0.01F);

    /* A button is not a slider. */
    REQUIRE(resonate::ui::updateInput(fixture.world, fixture.root, 10.0F, 10.0F, true));
    CHECK_FALSE(resonate::ui::stepSlider(fixture.world, fixture.root, true));
}

TEST_CASE("ui focus: an active modal gates hits, presses and Tab", "[ui][input]")
{
    Fixture fixture;
    CHECK_FALSE(resonate::ui::activeModal(fixture.world, fixture.root).valid());

    /* Focus the first button before the dialog opens. */
    REQUIRE(resonate::ui::updateInput(fixture.world, fixture.root, 10.0F, 10.0F, true));
    REQUIRE(resonate::ui::updateInput(fixture.world, fixture.root, 10.0F, 10.0F, false));
    const auto* pointer = fixture.world.get<resonate::ui::PointerState>(fixture.root);
    REQUIRE(pointer != nullptr);
    REQUIRE(pointer->focused == fixture.first);

    fixture.showDialog(true);
    CHECK(resonate::ui::activeModal(fixture.world, fixture.root) == fixture.dialog);

    /* A press outside the dialog reaches nothing and leaves the focus
       alone. */
    REQUIRE(resonate::ui::updateInput(fixture.world, fixture.root, 10.0F, 10.0F, true));
    pointer = fixture.world.get<resonate::ui::PointerState>(fixture.root);
    REQUIRE(pointer != nullptr);
    CHECK_FALSE(pointer->hover.valid());
    CHECK(pointer->focused == fixture.first);
    CHECK_FALSE(fixture.world.get<resonate::ui::Input>(fixture.first)->activated);

    /* Tab hops into the dialog and stays there. */
    REQUIRE(resonate::ui::focusNext(fixture.world, fixture.root, false));
    pointer = fixture.world.get<resonate::ui::PointerState>(fixture.root);
    CHECK(pointer->focused == fixture.ok);
    REQUIRE(resonate::ui::focusNext(fixture.world, fixture.root, false));
    pointer = fixture.world.get<resonate::ui::PointerState>(fixture.root);
    CHECK(pointer->focused == fixture.ok);

    /* Inside the dialog the pointer works: hovering its button is a hit. */
    REQUIRE(resonate::ui::updateInput(fixture.world, fixture.root, 60.0F, 60.0F, false));
    pointer = fixture.world.get<resonate::ui::PointerState>(fixture.root);
    CHECK(pointer->hover == fixture.ok);

    /* Closing the dialog lifts the gate. */
    fixture.showDialog(false);
    CHECK_FALSE(resonate::ui::activeModal(fixture.world, fixture.root).valid());
    REQUIRE(resonate::ui::updateInput(fixture.world, fixture.root, 10.0F, 10.0F, false));
    pointer = fixture.world.get<resonate::ui::PointerState>(fixture.root);
    CHECK(pointer->hover == fixture.first);
}
