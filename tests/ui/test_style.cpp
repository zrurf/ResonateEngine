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
#include <resonate/ui/layout.h>
#include <resonate/ui/style.h>

#include "resonate.ui/components.gen.h"

namespace
{

using resonate::ecs::Entity;
using resonate::ecs::World;
using resonate::ui::Background;
using resonate::ui::ComputedRect;

const char* const kStyled = R"xml(<document version="1" schemas="resonate.ui">
  <Screen key="hud">
    <StyleSheet key="styles">
      <StyleRule selector="Label"><Background color="1 0 0 1"/></StyleRule>
      <StyleRule selector=".panel"><Background color="0.1 0.1 0.1 1"/></StyleRule>
      <StyleRule selector=".bar"><Background color="0 1 0 1"/></StyleRule>
      <StyleRule selector=".bar.ammo"><Background color="1 0.5 0 1"/></StyleRule>
      <StyleRule selector="#special"><Background color="0 0 1 1"/></StyleRule>
    </StyleSheet>
    <Panel key="panel" class="panel" x="0" y="0" width="300" height="100"
           direction="Column" gap="4">
      <Label key="health" class="bar" height="10"/>
      <Label key="ammo" class="bar ammo" height="10"/>
      <Label key="plain" height="10"/>
      <Label key="special" id="special" class="bar" height="10"/>
      <Label key="explicit" class="bar" height="10">
        <Background color="0.2 0.2 0.2 1"/>
      </Label>
    </Panel>
  </Screen>
</document>)xml";

struct Fixture
{
    World world{resonate::systemAllocator()};

    Fixture()
    {
        REQUIRE(resonate::hierarchy::registerComponents(world));
        REQUIRE(resonate::ui::registerComponents(world));
    }
};

} // namespace

TEST_CASE("ui style: type, class, id, specificity and explicit fields", "[ui][style]")
{
    std::vector<resonate::dom::Diagnostic> diagnostics;
    auto document =
        resonate::dom::Document::parse(kStyled, &resonate::ui::documentSchema(), diagnostics);
    REQUIRE(document != nullptr);
    CHECK(diagnostics.empty());

    resonate::ui::StyleTable styles;
    REQUIRE(resonate::ui::buildStyles(*document, styles, diagnostics));
    CHECK_FALSE(styles.empty());

    Fixture fixture;
    const Entity root = resonate::ui::instantiate(fixture.world, *document);
    REQUIRE(root.valid());

    /* The style sheet is non-visual: only the panel is a child of the screen. */
    const auto screen_children = resonate::hierarchy::children(fixture.world, root);
    REQUIRE(screen_children.size() == 1U);
    const Entity panel = screen_children[0];
    const auto labels = resonate::hierarchy::children(fixture.world, panel);
    REQUIRE(labels.size() == 5U);

    const auto color_of = [&](std::size_t index)
    {
        const Background* background = fixture.world.get<Background>(labels[index]);
        REQUIRE(background != nullptr);
        return background->color;
    };

    /* .bar (10) beats Label (1); .bar.ammo (20) beats .bar; #special (100)
       beats .bar; an explicit field beats every rule. */
    CHECK(color_of(0).y > 0.9F);
    CHECK(color_of(1).x > 0.9F);
    CHECK(color_of(1).y > 0.4F);
    CHECK(color_of(2).x > 0.9F);
    CHECK(color_of(3).z > 0.9F);
    CHECK(std::fabs(color_of(4).x - 0.2F) < 0.001F);

    const Background* panel_background = fixture.world.get<Background>(panel);
    REQUIRE(panel_background != nullptr);
    CHECK(std::fabs(panel_background->color.x - 0.1F) < 0.001F);
}

TEST_CASE("ui style: malformed selectors and derived declarations are refused", "[ui][style]")
{
    const char* const invalid[] = {
        R"xml(<document version="1"><Screen><StyleSheet><StyleRule selector=".bad selector"/></StyleSheet></Screen></document>)xml",
        R"xml(<document version="1"><Screen><StyleSheet><StyleRule selector=".x"><ComputedRect/></StyleRule></StyleSheet></Screen></document>)xml",
        R"xml(<document version="1"><Screen><StyleSheet><StyleRule selector=".x:checked"/></StyleSheet></Screen></document>)xml",
        R"xml(<document version="1"><Screen><StyleSheet><StyleRule selector=".x:hover.extra"/></StyleSheet></Screen></document>)xml",
    };

    for (const char* text : invalid)
    {
        std::vector<resonate::dom::Diagnostic> diagnostics;
        auto document =
            resonate::dom::Document::parse(text, &resonate::ui::documentSchema(), diagnostics);
        REQUIRE(document != nullptr);
        resonate::ui::StyleTable styles;
        diagnostics.clear();
        CHECK_FALSE(resonate::ui::buildStyles(*document, styles, diagnostics));
        CHECK_FALSE(diagnostics.empty());

        Fixture fixture;
        CHECK_FALSE(resonate::ui::instantiate(fixture.world, *document).valid());
    }
}

TEST_CASE("ui style: a state rule needs a normal declaration and may not add components",
          "[ui][style]")
{
    const char* const invalid[] = {
        /* The base value to restore on leaving the state is not declared. */
        R"xml(<document version="1"><Screen><StyleSheet><StyleRule selector=".action:hover"><Background color="0 1 0 1"/></StyleRule></StyleSheet><Button class="action" width="10" height="10"/></Screen></document>)xml",
        /* A state rule may restyle, not bring a component into existence. */
        R"xml(<document version="1"><Screen><StyleSheet><StyleRule selector=".action"><Background color="0 1 0 1"/></StyleRule><StyleRule selector=".action:hover"><LayoutGroup gap="2"/></StyleRule></StyleSheet><Button class="action" width="10" height="10"/></Screen></document>)xml",
    };

    for (const char* text : invalid)
    {
        std::vector<resonate::dom::Diagnostic> diagnostics;
        auto document =
            resonate::dom::Document::parse(text, &resonate::ui::documentSchema(), diagnostics);
        REQUIRE(document != nullptr);
        resonate::ui::StyleTable styles;
        diagnostics.clear();
        REQUIRE(resonate::ui::buildStyles(*document, styles, diagnostics));

        Fixture fixture;
        CHECK_FALSE(resonate::ui::instantiate(fixture.world, *document).valid());
    }
}
