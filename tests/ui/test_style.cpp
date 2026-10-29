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
#include <resonate/ui/icon_registry.h>
#include <resonate/ui/image_registry.h>
#include <resonate/ui/layout.h>
#include <resonate/ui/input.h>
#include <resonate/ui/style.h>
#include <resonate/ui/transition.h>

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

TEST_CASE("ui style: theme variables resolve in declaration values", "[ui][style]")
{
    const char* const themed = R"xml(<document version="1" schemas="resonate.ui">
  <Screen key="hud">
    <StyleSheet key="theme">
      <Variable token="--panel" value="0.5 0.25 0.125 1"/>
      <Variable token="--panel" value="0.5 0.25 0.25 1"/>
      <StyleRule selector=".panel"><Background color="var(--panel)"/></StyleRule>
      <StyleRule selector=".fallback"><Background color="var(--missing, 0.125 0.5 0.25 1)"/></StyleRule>
    </StyleSheet>
    <Panel key="panel" class="panel" x="0" y="0" width="100" height="100"/>
    <Box key="fallback" class="fallback" x="0" y="0" width="100" height="100"/>
  </Screen>
</document>)xml";
    std::vector<resonate::dom::Diagnostic> diagnostics;
    auto document =
        resonate::dom::Document::parse(themed, &resonate::ui::documentSchema(), diagnostics);
    REQUIRE(document != nullptr);
    resonate::ui::StyleTable styles;
    REQUIRE(resonate::ui::buildStyles(*document, styles, diagnostics));

    Fixture fixture;
    const Entity root = resonate::ui::instantiate(fixture.world, *document);
    REQUIRE(root.valid());
    const auto children = resonate::hierarchy::children(fixture.world, root);
    REQUIRE(children.size() == 2U);
    /* The last Variable definition wins. */
    const Background* panel = fixture.world.get<Background>(children[0]);
    REQUIRE(panel != nullptr);
    CHECK(std::fabs(panel->color.y - 0.25F) < 0.001F);
    CHECK(std::fabs(panel->color.z - 0.25F) < 0.001F);
    /* An unknown token takes its fallback. */
    const Background* fallback = fixture.world.get<Background>(children[1]);
    REQUIRE(fallback != nullptr);
    CHECK(std::fabs(fallback->color.y - 0.5F) < 0.001F);
}

TEST_CASE("ui style: a background's radius reaches the display list and its border rings",
          "[ui][style]")
{
    const char* const rounded = R"xml(<document version="1" schemas="resonate.ui">
  <Screen key="hud">
    <StyleSheet key="theme">
      <Variable token="--radius" value="8"/>
      <StyleRule selector=".round">
        <Background color="0.2 0.3 0.4 1" border_color="1 1 1 1" border_width="2"
                    radius="var(--radius)"/>
      </StyleRule>
    </StyleSheet>
    <Panel key="round" class="round" x="0" y="0" width="100" height="50"/>
    <Panel key="clamped" x="0" y="60" width="100" height="50">
      <Background color="1 1 1 1" border_color="1 1 1 1" border_width="2" radius="60"/>
    </Panel>
  </Screen>
</document>)xml";
    std::vector<resonate::dom::Diagnostic> diagnostics;
    auto document =
        resonate::dom::Document::parse(rounded, &resonate::ui::documentSchema(), diagnostics);
    REQUIRE(document != nullptr);
    CHECK(diagnostics.empty());

    Fixture fixture;
    const Entity root = resonate::ui::instantiate(fixture.world, *document);
    REQUIRE(root.valid());
    REQUIRE(resonate::ui::layout(fixture.world, root, 200.0F, 160.0F));

    resonate::ui::DisplayList list;
    REQUIRE(resonate::ui::buildDisplayList(fixture.world, root, list));
    const auto quads = list.quads();
    /* Each panel is one fill and one border ring; nothing else paints. */
    REQUIRE(quads.size() == 4U);
    CHECK(std::fabs(quads[0].radius - 8.0F) < 0.001F);
    CHECK(quads[0].border == 0.0F);
    CHECK(std::fabs(quads[1].radius - 8.0F) < 0.001F);
    CHECK(std::fabs(quads[1].border - 2.0F) < 0.001F);
    /* A radius wider than half the shorter side is clamped to that half. */
    CHECK(std::fabs(quads[2].radius - 25.0F) < 0.001F);
    CHECK(std::fabs(quads[3].radius - 25.0F) < 0.001F);
    CHECK(std::fabs(quads[3].border - 2.0F) < 0.001F);
}

TEST_CASE("ui style: an icon node resolves its atlas rect and takes the class tint", "[ui][style]")
{
    const char* const icon_document = R"xml(<document version="1" schemas="resonate.ui">
  <Screen key="hud">
    <StyleSheet key="theme">
      <Variable token="--ink" value="0.25 0.5 0.75 1"/>
      <StyleRule selector=".icon"><Image tint="var(--ink)"/></StyleRule>
    </StyleSheet>
    <Icon key="probe" class="icon" x="10" y="10" width="16" height="16" icon="camera"/>
  </Screen>
</document>)xml";
    resonate::ui::IconRegistry icons;
    std::string icon_error;
    REQUIRE(icons.load("# atlas\ncamera 0.25 0.5 0.5 0.75\n", icon_error));
    icons.setTexture(7U);

    std::vector<resonate::dom::Diagnostic> diagnostics;
    auto document = resonate::dom::Document::parse(icon_document,
                                                   &resonate::ui::documentSchema(), diagnostics);
    REQUIRE(document != nullptr);
    CHECK(diagnostics.empty());

    Fixture fixture;
    const Entity root =
        resonate::ui::instantiate(fixture.world, *document, nullptr, {}, nullptr, nullptr, &icons);
    REQUIRE(root.valid());
    REQUIRE(resonate::ui::layout(fixture.world, root, 100.0F, 100.0F));

    const auto children = resonate::hierarchy::children(fixture.world, root);
    REQUIRE(children.size() == 1U);
    const resonate::ui::Image* image = fixture.world.get<resonate::ui::Image>(children[0]);
    REQUIRE(image != nullptr);
    CHECK(image->texture == 7U);
    CHECK(std::fabs(image->uv_min.x - 0.25F) < 0.0001F);
    CHECK(std::fabs(image->uv_min.y - 0.5F) < 0.0001F);
    CHECK(std::fabs(image->uv_max.x - 0.5F) < 0.0001F);
    CHECK(std::fabs(image->uv_max.y - 0.75F) < 0.0001F);
    CHECK(std::fabs(image->tint.z - 0.75F) < 0.0001F);

    resonate::ui::DisplayList list;
    REQUIRE(resonate::ui::buildDisplayList(fixture.world, root, list));
    REQUIRE(list.quads().size() == 1U);
    CHECK(list.quads()[0].texture == 7U);
    CHECK(std::fabs(list.quads()[0].uv_min.x - 0.25F) < 0.0001F);
    CHECK(std::fabs(list.quads()[0].uv_max.y - 0.75F) < 0.0001F);
}

TEST_CASE("ui style: an unknown theme variable is refused", "[ui][style]")
{
    const char* const broken = R"xml(<document version="1" schemas="resonate.ui">
  <Screen key="hud">
    <StyleSheet key="theme">
      <StyleRule selector=".panel"><Background color="var(--missing)"/></StyleRule>
    </StyleSheet>
  </Screen>
</document>)xml";
    std::vector<resonate::dom::Diagnostic> diagnostics;
    auto document =
        resonate::dom::Document::parse(broken, &resonate::ui::documentSchema(), diagnostics);
    REQUIRE(document != nullptr);
    resonate::ui::StyleTable styles;
    CHECK_FALSE(resonate::ui::buildStyles(*document, styles, diagnostics));
    REQUIRE_FALSE(diagnostics.empty());
}

namespace
{

bool readMemoryStylesheet(void* context, std::string_view path, std::string& out_text)
{
    auto* sheets = static_cast<const std::vector<std::pair<std::string, std::string>>*>(context);
    for (const auto& sheet : *sheets)
    {
        if (sheet.first == path)
        {
            out_text = sheet.second;
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE("ui style: an external stylesheet imports into the document", "[ui][style]")
{
    const char* const sheet = R"xml(<document version="1" schemas="resonate.ui">
  <StyleSheet key="theme">
    <Variable token="--accent" value="0.75 0.375 0.0 1"/>
    <StyleRule selector=".panel"><Background color="var(--accent)"/></StyleRule>
  </StyleSheet>
</document>)xml";
    const char* const document_text = R"xml(<document version="1" schemas="resonate.ui">
  <Screen key="hud">
    <StyleSheet key="theme" src="theme.rcss"/>
    <StyleSheet key="styles">
      <StyleRule selector=".panel"><Background color="0.125 0.125 0.125 1"/></StyleRule>
    </StyleSheet>
    <Panel key="panel" class="panel" x="0" y="0" width="100" height="100"/>
  </Screen>
</document>)xml";

    std::vector<resonate::dom::Diagnostic> diagnostics;
    auto document = resonate::dom::Document::parse(document_text, &resonate::ui::documentSchema(),
                                                   diagnostics);
    REQUIRE(document != nullptr);
    std::vector<std::pair<std::string, std::string>> sheets{{"theme.rcss", sheet}};
    REQUIRE(resonate::ui::loadStylesheets(*document, &sheets, &readMemoryStylesheet,
                                          diagnostics));
    CHECK(diagnostics.empty());

    resonate::ui::StyleTable styles;
    REQUIRE(resonate::ui::buildStyles(*document, styles, diagnostics));
    Fixture fixture;
    const Entity root = resonate::ui::instantiate(fixture.world, *document);
    REQUIRE(root.valid());
    const Entity panel = resonate::hierarchy::children(fixture.world, root)[0];
    const Background* background = fixture.world.get<Background>(panel);
    REQUIRE(background != nullptr);
    /* The document's own rule comes after the reference and wins the order
       tie-break at equal specificity. */
    CHECK(std::fabs(background->color.x - 0.125F) < 0.001F);

    /* A sheet the reader cannot open fails the load. */
    const char* const absent_text = R"xml(<document version="1" schemas="resonate.ui">
  <Screen key="hud">
    <StyleSheet key="theme" src="absent.rcss"/>
  </Screen>
</document>)xml";
    std::vector<resonate::dom::Diagnostic> missing;
    auto broken = resonate::dom::Document::parse(absent_text, &resonate::ui::documentSchema(),
                                                missing);
    REQUIRE(broken != nullptr);
    missing.clear();
    CHECK_FALSE(resonate::ui::loadStylesheets(*broken, &sheets, &readMemoryStylesheet, missing));
    CHECK_FALSE(missing.empty());
}

TEST_CASE("ui style: a transition tweens the hover colour instead of jumping",
          "[ui][style]")
{
    const char* const text = R"xml(<document version="1" schemas="resonate.ui">
  <Screen key="hud">
    <StyleSheet key="styles">
      <StyleRule selector=".action">
        <Background color="0.25 0.25 0.25 1"/>
        <Transition duration_ms="100"/>
      </StyleRule>
      <StyleRule selector=".action:hover">
        <Background color="0.75 0.75 0.75 1"/>
      </StyleRule>
    </StyleSheet>
    <Button key="action" class="action" x="0" y="0" width="100" height="40"/>
  </Screen>
</document>)xml";
    std::vector<resonate::dom::Diagnostic> diagnostics;
    auto document = resonate::dom::Document::parse(text, &resonate::ui::documentSchema(),
                                                   diagnostics);
    REQUIRE(document != nullptr);
    CHECK(diagnostics.empty());

    Fixture fixture;
    const Entity root = resonate::ui::instantiate(fixture.world, *document);
    REQUIRE(root.valid());
    REQUIRE(resonate::ui::layout(fixture.world, root, 200.0F, 200.0F));
    const Entity action = resonate::hierarchy::children(fixture.world, root)[0];

    resonate::ui::ColorTransitionSystem transitions;
    REQUIRE(transitions.bind(fixture.world));

    /* Hovering does not jump: the tween starts and the colour walks. */
    REQUIRE(resonate::ui::updateInput(fixture.world, root, 50.0F, 20.0F, false));
    const Background* background = fixture.world.get<Background>(action);
    REQUIRE(background != nullptr);
    CHECK(std::fabs(background->color.x - 0.25F) < 0.01F);
    REQUIRE(fixture.world.get<resonate::ui::ColorTween>(action) != nullptr);

    transitions.advance(fixture.world, 0.05F);
    background = fixture.world.get<Background>(action);
    CHECK(std::fabs(background->color.x - 0.50F) < 0.01F);

    transitions.advance(fixture.world, 0.05F);
    background = fixture.world.get<Background>(action);
    CHECK(std::fabs(background->color.x - 0.75F) < 0.01F);
    CHECK(fixture.world.get<resonate::ui::ColorTween>(action) == nullptr);
}
