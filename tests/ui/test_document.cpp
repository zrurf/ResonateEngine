#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <resonate/core/allocator.h>
#include <resonate/dom/document.h>
#include <resonate/ecs/world.h>
#include <resonate/hierarchy/tree.h>
#include <resonate/ui/display_list.h>
#include <resonate/ui/document.h>
#include <resonate/ui/hit_test.h>
#include <resonate/ui/binding.h>
#include <resonate/ui/image_registry.h>

#include "resonate.ui/components.gen.h"
#include <resonate/ui/document_schema.h>
#include <resonate/ui/style.h>
#include <resonate/ui/layout.h>

#include "resonate.ui/components.gen.h"

namespace
{

using resonate::ecs::Entity;
using resonate::ecs::World;
using resonate::ui::Background;
using resonate::ui::Canvas;
using resonate::ui::ComputedRect;
using resonate::ui::LayoutGroup;
using resonate::ui::RectTransform;

const char* const kUiDocument = R"xml(<document version="1" schemas="resonate.ui">
  <Screen key="hud" name="HUD">
    <Panel key="panel" id="hud-panel" x="40" y="40" width="360" height="180"
           align="Stretch">
      <Background color="0.06 0.07 0.10 0.92"/>
      <LayoutGroup direction="Column" gap="8"
                   padding_left="12" padding_top="12"
                   padding_right="12" padding_bottom="12"/>
      <Label key="title" height="28">
        <Background color="0.20 0.42 0.90 1"/>
      </Label>
      <Label key="bar" height="16">
        <Background color="0.35 0.84 0.45 1"/>
      </Label>
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

    Fixture()
    {
        REQUIRE(resonate::hierarchy::registerComponents(world));
        REQUIRE(resonate::ui::registerComponents(world));
    }
};

} // namespace

TEST_CASE("ui document: implicit node types instantiate into widget entities", "[ui][document]")
{
    std::vector<resonate::dom::Diagnostic> diagnostics;
    auto document = resonate::dom::Document::parse(kUiDocument, &resonate::ui::documentSchema(),
                                                   diagnostics);
    REQUIRE(document != nullptr);
    CHECK(diagnostics.empty());

    Fixture fixture;
    const Entity root = resonate::ui::instantiate(fixture.world, *document);
    REQUIRE(root.valid());
    CHECK(fixture.world.get<Canvas>(root) != nullptr);
    CHECK(fixture.world.get<ComputedRect>(root) != nullptr);

    const auto children = resonate::hierarchy::children(fixture.world, root);
    REQUIRE(children.size() == 1U);
    const Entity panel = children[0];
    const RectTransform* panel_rect = fixture.world.get<RectTransform>(panel);
    REQUIRE(panel_rect != nullptr);
    CHECK(near(panel_rect->x, 40.0F));
    CHECK(near(panel_rect->width, 360.0F));
    const LayoutGroup* layout = fixture.world.get<LayoutGroup>(panel);
    REQUIRE(layout != nullptr);
    CHECK(layout->direction == resonate::ui::FlexDirection::Column);
    CHECK(layout->align == resonate::ui::Align::Stretch);
    CHECK(near(layout->gap, 8.0F));
    CHECK(near(layout->padding_left, 12.0F));
    const Background* background = fixture.world.get<Background>(panel);
    REQUIRE(background != nullptr);
    CHECK(near(background->color.w, 0.92F));

    REQUIRE(resonate::ui::layout(fixture.world, root, 320.0F, 240.0F));
    const ComputedRect* panel_rect_out = fixture.world.get<ComputedRect>(panel);
    REQUIRE(panel_rect_out != nullptr);
    CHECK(near(panel_rect_out->x, 40.0F));
    CHECK(near(panel_rect_out->y, 40.0F));
    CHECK(near(panel_rect_out->width, 360.0F));
    CHECK(near(panel_rect_out->height, 180.0F));

    const auto labels = resonate::hierarchy::children(fixture.world, panel);
    REQUIRE(labels.size() == 2U);
    const ComputedRect* title_rect = fixture.world.get<ComputedRect>(labels[0]);
    REQUIRE(title_rect != nullptr);
    CHECK(near(title_rect->x, 52.0F));
    CHECK(near(title_rect->y, 52.0F));
    CHECK(near(title_rect->width, 336.0F));
    CHECK(near(title_rect->height, 28.0F));
    const ComputedRect* bar_rect = fixture.world.get<ComputedRect>(labels[1]);
    REQUIRE(bar_rect != nullptr);
    CHECK(near(bar_rect->y, 88.0F));
    CHECK(near(bar_rect->height, 16.0F));

    resonate::ui::DisplayList list;
    REQUIRE(resonate::ui::buildDisplayList(fixture.world, root, list));
    CHECK(list.size() == 3U);
    CHECK(near(list.quads()[0].width, 360.0F));
    CHECK(near(list.quads()[1].y, 52.0F));
}

TEST_CASE("ui document: a Picture emits a textured quad", "[ui][document]")
{
    const char* const text = R"xml(<document version="1">
  <Screen key="hud">
    <Picture key="icon" texture="1" x="10" y="20" width="32" height="24"
             uv_min="0.25 0.5" uv_max="0.75 0.75" tint="1 0.5 0.25 1"/>
  </Screen>
</document>)xml";
    std::vector<resonate::dom::Diagnostic> diagnostics;
    auto document =
        resonate::dom::Document::parse(text, &resonate::ui::documentSchema(), diagnostics);
    REQUIRE(document != nullptr);
    CHECK(diagnostics.empty());

    Fixture fixture;
    const Entity root = resonate::ui::instantiate(fixture.world, *document);
    REQUIRE(root.valid());
    REQUIRE(resonate::ui::layout(fixture.world, root, 200.0F, 200.0F));

    resonate::ui::DisplayList list;
    REQUIRE(resonate::ui::buildDisplayList(fixture.world, root, list));
    REQUIRE(list.size() == 1U);
    const resonate::ui::Quad& quad = list.quads()[0];
    CHECK(quad.texture == resonate::ui::kTextureChecker);
    CHECK(near(quad.x, 10.0F));
    CHECK(near(quad.y, 20.0F));
    CHECK(near(quad.width, 32.0F));
    CHECK(near(quad.height, 24.0F));
    CHECK(near(quad.uv_min.x, 0.25F));
    CHECK(near(quad.uv_min.y, 0.5F));
    CHECK(near(quad.uv_max.x, 0.75F));
    CHECK(near(quad.uv_max.y, 0.75F));
    CHECK(near(quad.color.y, 0.5F));
    CHECK(near(quad.color.z, 0.25F));
}

TEST_CASE("ui document: a Picture path resolves through the image registry", "[ui][document]")
{
    const char* const text = R"xml(<document version="1">
  <Screen key="hud">
    <Picture key="known" path="hero.png" x="10" y="20" width="32" height="24"/>
    <Picture key="unknown" path="missing.png" x="50" y="20" width="8" height="8"/>
  </Screen>
</document>)xml";
    std::vector<resonate::dom::Diagnostic> diagnostics;
    auto document =
        resonate::dom::Document::parse(text, &resonate::ui::documentSchema(), diagnostics);
    REQUIRE(document != nullptr);
    CHECK(diagnostics.empty());

    resonate::ui::ImageRegistry images;
    const std::uint32_t hero = images.registerImage("hero.png");
    CHECK(hero == resonate::ui::ImageRegistry::kFirstId);

    Fixture fixture;
    const Entity root = resonate::ui::instantiate(fixture.world, *document, nullptr, {}, nullptr,
                                                  &images);
    REQUIRE(root.valid());
    REQUIRE(resonate::ui::layout(fixture.world, root, 200.0F, 200.0F));

    const auto children = resonate::hierarchy::children(fixture.world, root);
    REQUIRE(children.size() == 2U);
    const resonate::ui::Image* known = fixture.world.get<resonate::ui::Image>(children[0]);
    REQUIRE(known != nullptr);
    CHECK(known->texture == hero);
    const resonate::ui::Image* unknown = fixture.world.get<resonate::ui::Image>(children[1]);
    REQUIRE(unknown != nullptr);
    /* An unregistered path keeps the authored texture and reports a warning. */
    CHECK(unknown->texture == resonate::ui::kTextureWhite);
}

TEST_CASE("ui document: an authored derived component is refused", "[ui][document]")
{
    const char* const text = R"xml(<document version="1">
  <Screen key="hud"><ComputedRect/></Screen>
</document>)xml";
    std::vector<resonate::dom::Diagnostic> diagnostics;
    auto document = resonate::dom::Document::parse(text, &resonate::ui::documentSchema(),
                                                   diagnostics);
    REQUIRE(document != nullptr);

    Fixture fixture;
    CHECK_FALSE(resonate::ui::instantiate(fixture.world, *document).valid());
}

TEST_CASE("ui document: an unknown component is a parse error", "[ui][document]")
{
    const char* const text = R"xml(<document version="1">
  <Screen key="hud"><Camera/></Screen>
</document>)xml";
    std::vector<resonate::dom::Diagnostic> diagnostics;
    CHECK(resonate::dom::Document::parse(text, &resonate::ui::documentSchema(), diagnostics) ==
          nullptr);
    CHECK_FALSE(diagnostics.empty());
}

namespace
{

/* Loads a shipped document's external stylesheets the way startup does: paths
   resolve from the source tree. */
bool loadShippedStylesheets(resonate::dom::Document& document)
{
    std::vector<resonate::dom::Diagnostic> diagnostics;
    return resonate::ui::loadStylesheets(
        document, nullptr,
        [](void*, std::string_view source_path, std::string& out_text)
        {
            std::ifstream file(std::string(RESONATE_SOURCE_DIR) + "/" + std::string(source_path),
                               std::ios::binary);
            if (!file.is_open())
            {
                return false;
            }
            out_text.assign(std::istreambuf_iterator<char>(file),
                            std::istreambuf_iterator<char>());
            return true;
        },
        diagnostics);
}

} // namespace

TEST_CASE("ui document: the shipped demo document instantiates", "[ui][document]")
{
    const std::string path = std::string(RESONATE_SOURCE_DIR) + "/content/ui/demo.rui";
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.is_open());
    const std::string text((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());

    std::vector<resonate::dom::Diagnostic> diagnostics;
    auto document = resonate::dom::Document::parse(text, &resonate::ui::documentSchema(),
                                                   diagnostics);
    REQUIRE(document != nullptr);
    CHECK(diagnostics.empty());
    REQUIRE(loadShippedStylesheets(*document));

    Fixture fixture;
    resonate::ui::ImageRegistry images;
    const std::uint32_t logo = images.registerImage("logo.png");
    const Entity root =
        resonate::ui::instantiate(fixture.world, *document, nullptr, {}, nullptr, &images);
    REQUIRE(root.valid());
    resonate::ui::updateBindings(fixture.world, root);
    REQUIRE(resonate::ui::layout(fixture.world, root, 1600.0F, 900.0F));

    resonate::ui::DisplayList list;
    REQUIRE(resonate::ui::buildDisplayList(fixture.world, root, list));
    /* Seven fills: the panel, title, two bars, field, button and the icon (a
       transparent label contributes no quad). */
    CHECK(list.size() == 7U);
    CHECK(near(list.quads()[0].width, 360.0F));
    CHECK(near(list.quads()[0].height, 180.0F));
    CHECK(near(list.quads()[2].width, 240.0F));
    CHECK(near(list.quads()[3].width, 90.0F));
    CHECK(near(list.quads()[4].width, 200.0F));
    CHECK(near(list.quads()[4].height, 24.0F));
    CHECK(near(list.quads()[5].width, 140.0F));
    CHECK(near(list.quads()[5].height, 36.0F));
    CHECK(list.quads()[6].texture == logo);
    CHECK(near(list.quads()[6].width, 48.0F));
}

TEST_CASE("ui document: an absolute Picture inside a panel emits a textured quad",
          "[ui][document]")
{
    /* The demo HUD's icon: a Picture anchored absolutely inside a column
       panel with padding. The panel's flow must leave it out of the layout
       and still place it, or the icon never reaches the display list. */
    const char* const text = R"xml(<document version="1">
  <Screen key="hud">
    <Panel key="panel" x="40" y="40" width="360" height="180" color="0.1 0.1 0.1 1"
           direction="Column" padding_left="12" padding_top="12">
      <Label key="title" width="336" height="28" text="Title"/>
      <Picture key="icon" path="logo.png" x="276" y="96" width="48" height="48"
               position="Absolute"/>
    </Panel>
  </Screen>
</document>)xml";
    std::vector<resonate::dom::Diagnostic> diagnostics;
    auto document =
        resonate::dom::Document::parse(text, &resonate::ui::documentSchema(), diagnostics);
    REQUIRE(document != nullptr);
    CHECK(diagnostics.empty());

    resonate::ui::ImageRegistry images;
    const std::uint32_t logo = images.registerImage("logo.png");

    Fixture fixture;
    const Entity root = resonate::ui::instantiate(fixture.world, *document, nullptr, {}, nullptr,
                                                  &images);
    REQUIRE(root.valid());
    REQUIRE(resonate::ui::layout(fixture.world, root, 320.0F, 240.0F));

    const Entity panel = resonate::hierarchy::children(fixture.world, root)[0];
    const auto children = resonate::hierarchy::children(fixture.world, panel);
    REQUIRE(children.size() == 2U);
    const ComputedRect* icon_rect = fixture.world.get<ComputedRect>(children[1]);
    REQUIRE(icon_rect != nullptr);
    /* An absolute child is anchored inside the panel's content box: the
       panel's origin plus its padding, then the authored offset. */
    CHECK(near(icon_rect->x, 40.0F + 12.0F + 276.0F));
    CHECK(near(icon_rect->y, 40.0F + 12.0F + 96.0F));
    CHECK(near(icon_rect->width, 48.0F));
    CHECK(near(icon_rect->height, 48.0F));

    resonate::ui::DisplayList list;
    REQUIRE(resonate::ui::buildDisplayList(fixture.world, root, list));
    const resonate::ui::Quad* quad = nullptr;
    for (const resonate::ui::Quad& candidate : list.quads())
    {
        if (candidate.texture == logo)
        {
            quad = &candidate;
        }
    }
    REQUIRE(quad != nullptr);
    CHECK(near(quad->x, 40.0F + 12.0F + 276.0F));
    CHECK(near(quad->y, 40.0F + 12.0F + 96.0F));
    CHECK(near(quad->width, 48.0F));
}

TEST_CASE("ui document: a nine-sliced Picture emits the nine regions", "[ui][document]")
{
    const char* const text = R"xml(<document version="1">
  <Screen key="hud">
    <Picture key="frame" texture="1" x="10" y="20" width="100" height="80"
             uv_min="0 0" uv_max="1 1" slice="0.25 0.25 0.25 0.25"/>
  </Screen>
</document>)xml";
    std::vector<resonate::dom::Diagnostic> diagnostics;
    auto document =
        resonate::dom::Document::parse(text, &resonate::ui::documentSchema(), diagnostics);
    REQUIRE(document != nullptr);
    CHECK(diagnostics.empty());

    Fixture fixture;
    const Entity root = resonate::ui::instantiate(fixture.world, *document);
    REQUIRE(root.valid());
    REQUIRE(resonate::ui::layout(fixture.world, root, 200.0F, 200.0F));

    resonate::ui::DisplayList list;
    REQUIRE(resonate::ui::buildDisplayList(fixture.world, root, list));
    REQUIRE(list.size() == 9U);
    const auto quads = list.quads();
    CHECK(near(quads[0].x, 10.0F));
    CHECK(near(quads[0].y, 20.0F));
    CHECK(near(quads[0].width, 25.0F));
    CHECK(near(quads[0].height, 20.0F));
    CHECK(near(quads[0].uv_min.x, 0.0F));
    CHECK(near(quads[0].uv_max.x, 0.25F));
    CHECK(near(quads[4].x, 35.0F));
    CHECK(near(quads[4].y, 40.0F));
    CHECK(near(quads[4].width, 50.0F));
    CHECK(near(quads[4].height, 40.0F));
    CHECK(near(quads[4].uv_min.x, 0.25F));
    CHECK(near(quads[4].uv_max.x, 0.75F));
    CHECK(near(quads[8].x, 85.0F));
    CHECK(near(quads[8].y, 80.0F));
    CHECK(near(quads[8].width, 25.0F));
    CHECK(near(quads[8].height, 20.0F));
}

TEST_CASE("ui document: a hidden subtree paints nothing and takes no hits", "[ui][document]")
{
    const char* const text = R"xml(<document version="1" schemas="resonate.ui">
  <Screen key="hud">
    <Panel key="left" x="0" y="0" width="100" height="100">
      <Background color="0.2 0.3 0.4 1"/>
      <Label key="caption" x="0" y="0" width="100" height="20" text="left"/>
    </Panel>
    <Panel key="right" x="120" y="0" width="100" height="100">
      <Background color="0.4 0.3 0.2 1"/>
    </Panel>
  </Screen>
</document>)xml";
    std::vector<resonate::dom::Diagnostic> diagnostics;
    auto document =
        resonate::dom::Document::parse(text, &resonate::ui::documentSchema(), diagnostics);
    REQUIRE(document != nullptr);
    CHECK(diagnostics.empty());

    Fixture fixture;
    const Entity root = resonate::ui::instantiate(fixture.world, *document);
    REQUIRE(root.valid());
    REQUIRE(resonate::ui::layout(fixture.world, root, 400.0F, 200.0F));
    const Entity left = resonate::ui::findByKey(fixture.world, root, "left");
    REQUIRE(left.valid());

    const auto quads = [&]()
    {
        resonate::ui::DisplayList list;
        REQUIRE(resonate::ui::buildDisplayList(fixture.world, root, list));
        return list.quads().size();
    };
    CHECK(quads() == 2U);
    CHECK(resonate::ui::hitTest(fixture.world, root, 50.0F, 50.0F) == left);

    /* Hiding the pane keeps its widgets and their geometry; the paint and the
       hit go away. */
    CHECK(resonate::ui::isShown(fixture.world, left));
    resonate::ui::setShown(fixture.world, left, false);
    CHECK_FALSE(resonate::ui::isShown(fixture.world, left));
    CHECK(quads() == 1U);
    CHECK(resonate::ui::hitTest(fixture.world, root, 50.0F, 50.0F) != left);

    resonate::ui::setShown(fixture.world, left, true);
    CHECK(quads() == 2U);
    CHECK(resonate::ui::hitTest(fixture.world, root, 50.0F, 50.0F) == left);
}
