#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <resonate/core/allocator.h>
#include <resonate/dom/document.h>
#include <resonate/ecs/world.h>
#include <resonate/hierarchy/tree.h>
#include <resonate/text/font.h>
#include <resonate/ui/display_list.h>
#include <resonate/ui/document.h>
#include <resonate/ui/document_schema.h>
#include <resonate/ui/input.h>
#include <resonate/ui/layout.h>
#include <resonate/ui/text_runtime.h>

#include "resonate.ui/components.gen.h"

namespace
{

using resonate::ecs::Entity;
using resonate::ecs::World;
using resonate::ui::ComputedRect;
using resonate::ui::Glyphs;
using resonate::ui::PointerState;
using resonate::ui::Text;
using resonate::ui::TextAtlasPage;

const char* const kLabel = R"xml(<document version="1" schemas="resonate.ui">
  <Screen key="hud">
    <Label key="greeting" text="Hi 你" font_size="24" color="0 0 0 0"
           width="160" height="32"/>
  </Screen>
</document>)xml";

std::shared_ptr<const resonate::text::Font> loadFont()
{
    const std::string path =
        std::string(RESONATE_SOURCE_DIR) + "/content/ui/fonts/ResonateSansSC-Subset.ttf";
    std::string error;
    auto font = resonate::text::Font::loadFile(path, error);
    REQUIRE(font != nullptr);
    return font;
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

TEST_CASE("ui text: a Label shapes into glyph quads and one canvas atlas", "[ui][text]")
{
    std::vector<resonate::dom::Diagnostic> diagnostics;
    auto document = resonate::dom::Document::parse(kLabel, &resonate::ui::documentSchema(),
                                                   diagnostics);
    REQUIRE(document != nullptr);
    CHECK(diagnostics.empty());

    Fixture fixture;
    const Entity root = resonate::ui::instantiate(fixture.world, *document, nullptr, {loadFont()});
    REQUIRE(root.valid());

    const TextAtlasPage* page = fixture.world.get<TextAtlasPage>(root);
    REQUIRE(page != nullptr);
    CHECK(page->width == 512U);
    CHECK(page->height == 512U);
    CHECK(page->revision == 1U);
    REQUIRE(fixture.world.alive(page->pixels));
    CHECK(fixture.world.blobSize(page->pixels) ==
          static_cast<std::size_t>(page->width) * page->height * 4U);
    const auto* atlas_pixels = static_cast<const std::uint8_t*>(fixture.world.blobData(page->pixels));
    REQUIRE(atlas_pixels != nullptr);
    std::uint32_t white_texels = 0;
    for (std::size_t index = 0; index < fixture.world.blobSize(page->pixels); index += 4U)
    {
        if (atlas_pixels[index + 3U] > 0U && atlas_pixels[index] == 255U &&
            atlas_pixels[index + 1U] == 255U && atlas_pixels[index + 2U] == 255U)
        {
            ++white_texels;
        }
    }
    CHECK(white_texels > 0U);

    const auto children = resonate::hierarchy::children(fixture.world, root);
    REQUIRE(children.size() == 1U);
    const Entity label = children[0];
    const Text* text = fixture.world.get<Text>(label);
    REQUIRE(text != nullptr);
    REQUIRE(fixture.world.alive(text->content));
    const Glyphs* glyphs = fixture.world.get<Glyphs>(label);
    REQUIRE(glyphs != nullptr);
    REQUIRE(fixture.world.alive(glyphs->records));
    CHECK(fixture.world.blobSize(glyphs->records) > 0U);

    REQUIRE(resonate::ui::layout(fixture.world, root, 320.0F, 200.0F));
    resonate::ui::DisplayList list;
    REQUIRE(resonate::ui::buildDisplayList(fixture.world, root, list));
    REQUIRE(list.size() > 1U);

    std::size_t text_quads = 0;
    for (const resonate::ui::Quad& quad : list.quads())
    {
        if (quad.texture != resonate::ui::kTextureFont)
        {
            continue;
        }
        ++text_quads;
        CHECK(quad.width > 0.0F);
        CHECK(quad.height > 0.0F);
        CHECK(quad.uv_min.x >= 0.0F);
        CHECK(quad.uv_min.y >= 0.0F);
        CHECK(quad.uv_max.x <= 1.0F);
        CHECK(quad.uv_max.y <= 1.0F);
    }
    CHECK(text_quads > 0U);
}


TEST_CASE("ui text: the demo's title keeps its text color", "[ui][text]")
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

    Fixture fixture;
    const Entity root = resonate::ui::instantiate(fixture.world, *document, nullptr, {loadFont()});
    REQUIRE(root.valid());
    const auto panel = resonate::hierarchy::children(fixture.world, root)[0];
    const auto labels = resonate::hierarchy::children(fixture.world, panel);
    REQUIRE(labels.size() >= 4U);
    const Text* title = fixture.world.get<Text>(labels[0]);
    REQUIRE(title != nullptr);
    CHECK(title->text_color.x == 1.0F);
    CHECK(title->text_color.y == 1.0F);
    CHECK(title->text_color.z == 1.0F);
    CHECK(title->text_color.w == 1.0F);

    REQUIRE(resonate::ui::layout(fixture.world, root, 1600.0F, 900.0F));
    resonate::ui::DisplayList list;
    REQUIRE(resonate::ui::buildDisplayList(fixture.world, root, list));
    std::size_t text_quads = 0;
    for (const resonate::ui::Quad& quad : list.quads())
    {
        if (quad.texture != resonate::ui::kTextureFont)
        {
            continue;
        }
        ++text_quads;
    }
    CHECK(text_quads > 0U);
}

TEST_CASE("ui text: without a font the Label keeps its box but has no glyph run",
          "[ui][text]")
{
    std::vector<resonate::dom::Diagnostic> diagnostics;
    auto document = resonate::dom::Document::parse(kLabel, &resonate::ui::documentSchema(),
                                                   diagnostics);
    REQUIRE(document != nullptr);

    Fixture fixture;
    const Entity root = resonate::ui::instantiate(fixture.world, *document);
    REQUIRE(root.valid());
    CHECK(fixture.world.get<TextAtlasPage>(root) == nullptr);

    const auto children = resonate::hierarchy::children(fixture.world, root);
    REQUIRE(children.size() == 1U);
    CHECK(fixture.world.get<Glyphs>(children[0]) == nullptr);
}

TEST_CASE("ui text: wrapping and alignment lay glyphs out in lines", "[ui][text]")
{
    const char* const text = R"xml(<document version="1" schemas="resonate.ui">
  <Screen key="hud">
    <Label key="wrapped" text="W W W" font_size="20" width="50" height="120"
           color="0 0 0 0" align="Center" wrap="true"/>
  </Screen>
</document>)xml";
    std::vector<resonate::dom::Diagnostic> diagnostics;
    auto document = resonate::dom::Document::parse(text, &resonate::ui::documentSchema(),
                                                   diagnostics);
    REQUIRE(document != nullptr);

    Fixture fixture;
    const Entity root = resonate::ui::instantiate(fixture.world, *document, nullptr, {loadFont()});
    REQUIRE(root.valid());
    REQUIRE(resonate::ui::layout(fixture.world, root, 200.0F, 200.0F));

    resonate::ui::DisplayList list;
    REQUIRE(resonate::ui::buildDisplayList(fixture.world, root, list));
    std::vector<float> ys;
    float minimum_x = 1e9F;
    for (const resonate::ui::Quad& quad : list.quads())
    {
        if (quad.texture != resonate::ui::kTextureFont)
        {
            continue;
        }
        ys.push_back(quad.y);
        minimum_x = std::min(minimum_x, quad.x);
    }
    REQUIRE(ys.size() == 3U);
    std::sort(ys.begin(), ys.end());
    CHECK(ys[2] > ys[0] + 10.0F);
    CHECK(minimum_x > -6.0F);
}

TEST_CASE("ui text: CJK wraps between glyphs", "[ui][text]")
{
    const char* const text = R"xml(<document version="1" schemas="resonate.ui">
  <Screen key="hud">
    <Label key="wrapped" text="生命值生命值" font_size="20" width="40" height="120"
           color="0 0 0 0"/>
  </Screen>
</document>)xml";
    std::vector<resonate::dom::Diagnostic> diagnostics;
    auto document = resonate::dom::Document::parse(text, &resonate::ui::documentSchema(),
                                                   diagnostics);
    REQUIRE(document != nullptr);

    Fixture fixture;
    const Entity root = resonate::ui::instantiate(fixture.world, *document, nullptr, {loadFont()});
    REQUIRE(root.valid());
    REQUIRE(resonate::ui::layout(fixture.world, root, 200.0F, 200.0F));

    resonate::ui::DisplayList list;
    REQUIRE(resonate::ui::buildDisplayList(fixture.world, root, list));
    std::vector<float> ys;
    std::size_t glyphs = 0;
    for (const resonate::ui::Quad& quad : list.quads())
    {
        if (quad.texture == resonate::ui::kTextureFont)
        {
            ys.push_back(quad.y);
            ++glyphs;
        }
    }
    CHECK(glyphs == 6U);
    std::sort(ys.begin(), ys.end());
    CHECK(ys.back() > ys.front() + 40.0F);
}

TEST_CASE("ui text: a zero-sized Label takes its measured text extent", "[ui][text]")
{
    const char* const text = R"xml(<document version="1" schemas="resonate.ui">
  <Screen key="hud">
    <Panel key="row" x="0" y="0" width="300" height="60"
           direction="Row" align="Start">
      <Label key="first" text="AB" font_size="20" color="0 0 0 0"/>
      <Label key="second" text="CD" font_size="20" color="0 0 0 0"/>
    </Panel>
  </Screen>
</document>)xml";
    std::vector<resonate::dom::Diagnostic> diagnostics;
    auto document = resonate::dom::Document::parse(text, &resonate::ui::documentSchema(),
                                                   diagnostics);
    REQUIRE(document != nullptr);

    Fixture fixture;
    const Entity root = resonate::ui::instantiate(fixture.world, *document, nullptr, {loadFont()});
    REQUIRE(root.valid());
    REQUIRE(resonate::ui::layout(fixture.world, root, 320.0F, 200.0F));

    const auto panel = resonate::hierarchy::children(fixture.world, root)[0];
    const auto labels = resonate::hierarchy::children(fixture.world, panel);
    REQUIRE(labels.size() == 2U);
    const auto* first = fixture.world.get<resonate::ui::ComputedRect>(labels[0]);
    const auto* second = fixture.world.get<resonate::ui::ComputedRect>(labels[1]);
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);
    CHECK(first->width > 10.0F);
    CHECK(first->height > 10.0F);
    CHECK(second->x >= first->x + first->width - 0.01F);
    CHECK(second->width > 10.0F);
}

TEST_CASE("ui text: a TextRuntime replaces a label's text after instantiation", "[ui][text]")
{
    const char* const text = R"xml(<document version="1" schemas="resonate.ui">
  <Screen key="hud">
    <Label key="label" text="A" font_size="24" width="120" height="40"
           color="0 0 0 0"/>
  </Screen>
</document>)xml";
    std::vector<resonate::dom::Diagnostic> diagnostics;
    auto document = resonate::dom::Document::parse(text, &resonate::ui::documentSchema(),
                                                   diagnostics);
    REQUIRE(document != nullptr);

    resonate::ui::TextRuntime runtime({loadFont()});
    Fixture fixture;
    const Entity root =
        resonate::ui::instantiate(fixture.world, *document, nullptr, runtime);
    REQUIRE(root.valid());
    const auto children = resonate::hierarchy::children(fixture.world, root);
    REQUIRE(children.size() == 1U);
    const Entity label = children[0];
    const TextAtlasPage* page = fixture.world.get<TextAtlasPage>(root);
    REQUIRE(page != nullptr);
    const std::uint32_t revision = page->revision;

    std::string error;
    REQUIRE(runtime.setText(fixture.world, root, label, "BC", error));
    CHECK(error.empty());

    const Text* replaced = fixture.world.get<Text>(label);
    REQUIRE(replaced != nullptr);
    REQUIRE(fixture.world.alive(replaced->content));
    CHECK(fixture.world.blobSize(replaced->content) == 2U);
    CHECK(std::string(static_cast<const char*>(fixture.world.blobData(replaced->content)), 2U) ==
          "BC");
    const Glyphs* glyphs = fixture.world.get<Glyphs>(label);
    REQUIRE(glyphs != nullptr);
    CHECK(fixture.world.blobSize(glyphs->records) > 0U);
    page = fixture.world.get<TextAtlasPage>(root);
    REQUIRE(page != nullptr);
    CHECK(page->revision == revision + 1U);
}

TEST_CASE("ui text: typing edits the focused field", "[ui][text]")
{
    const char* const text = R"xml(<document version="1" schemas="resonate.ui">
  <Screen key="hud">
    <Label key="field" text="A" font_size="20" width="120" height="32" color="0 0 0 0">
      <Input/>
    </Label>
  </Screen>
</document>)xml";
    std::vector<resonate::dom::Diagnostic> diagnostics;
    auto document = resonate::dom::Document::parse(text, &resonate::ui::documentSchema(),
                                                   diagnostics);
    REQUIRE(document != nullptr);

    resonate::ui::TextRuntime runtime({loadFont()});
    Fixture fixture;
    const Entity root = resonate::ui::instantiate(fixture.world, *document, nullptr, runtime);
    REQUIRE(root.valid());
    REQUIRE(resonate::ui::layout(fixture.world, root, 200.0F, 200.0F));
    const auto children = resonate::hierarchy::children(fixture.world, root);
    REQUIRE(children.size() == 1U);
    const Entity field = children[0];
    const ComputedRect* rect = fixture.world.get<ComputedRect>(field);
    REQUIRE(rect != nullptr);
    const float x = rect->x + rect->width * 0.5F;
    const float y = rect->y + rect->height * 0.5F;

    REQUIRE(resonate::ui::updateInput(fixture.world, root, x, y, false));
    REQUIRE(resonate::ui::updateInput(fixture.world, root, x, y, true));
    REQUIRE(resonate::ui::updateInput(fixture.world, root, x, y, false));
    const PointerState* pointer = fixture.world.get<PointerState>(root);
    REQUIRE(pointer != nullptr);
    CHECK(pointer->focused == field);

    const auto carets = [&]
    {
        resonate::ui::DisplayList list;
        REQUIRE(resonate::ui::buildDisplayList(fixture.world, root, list));
        std::size_t count = 0;
        for (const resonate::ui::Quad& quad : list.quads())
        {
            if (quad.texture == resonate::ui::kTextureWhite && quad.width == 1.0F)
            {
                ++count;
            }
        }
        return count;
    };
    CHECK(carets() == 1U);

    const auto content = [&]
    {
        const Text* component = fixture.world.get<Text>(field);
        REQUIRE(component != nullptr);
        REQUIRE(fixture.world.alive(component->content));
        return std::string(static_cast<const char*>(fixture.world.blobData(component->content)),
                           fixture.world.blobSize(component->content));
    };
    const TextAtlasPage* page = fixture.world.get<TextAtlasPage>(root);
    REQUIRE(page != nullptr);
    std::uint32_t revision = page->revision;

    std::string error;
    REQUIRE(resonate::ui::updateTextInput(fixture.world, root, runtime, "B", false, error));
    CHECK(error.empty());
    CHECK(content() == "AB");
    page = fixture.world.get<TextAtlasPage>(root);
    REQUIRE(page != nullptr);
    CHECK(page->revision == revision + 1U);
    revision = page->revision;

    /* Backspace erases a whole code point, not one byte. */
    REQUIRE(resonate::ui::updateTextInput(fixture.world, root, runtime, "\xE4\xBD\xA0", false,
                                          error));
    CHECK(content() == "AB\xE4\xBD\xA0");
    REQUIRE(resonate::ui::updateTextInput(fixture.world, root, runtime, "", true, error));
    CHECK(content() == "AB");
    REQUIRE(resonate::ui::updateTextInput(fixture.world, root, runtime, "", true, error));
    CHECK(content() == "A");

    /* A press outside the field drops focus and typing becomes a no-op. */
    REQUIRE(resonate::ui::updateInput(fixture.world, root, -100.0F, -100.0F, true));
    REQUIRE(resonate::ui::updateInput(fixture.world, root, -100.0F, -100.0F, false));
    pointer = fixture.world.get<PointerState>(root);
    REQUIRE(pointer != nullptr);
    CHECK_FALSE(pointer->focused.valid());
    CHECK(carets() == 0U);
    REQUIRE(resonate::ui::updateTextInput(fixture.world, root, runtime, "Z", false, error));
    CHECK(content() == "A");
}

TEST_CASE("ui text: an IME preedit previews and then commits", "[ui][text]")
{
    const char* const text = R"xml(<document version="1" schemas="resonate.ui">
  <Screen key="hud">
    <Label key="field" text="A" font_size="20" width="120" height="32" color="0 0 0 0">
      <Input/>
    </Label>
  </Screen>
</document>)xml";
    std::vector<resonate::dom::Diagnostic> diagnostics;
    auto document = resonate::dom::Document::parse(text, &resonate::ui::documentSchema(),
                                                   diagnostics);
    REQUIRE(document != nullptr);

    resonate::ui::TextRuntime runtime({loadFont()});
    Fixture fixture;
    const Entity root = resonate::ui::instantiate(fixture.world, *document, nullptr, runtime);
    REQUIRE(root.valid());
    REQUIRE(resonate::ui::layout(fixture.world, root, 200.0F, 200.0F));
    const auto children = resonate::hierarchy::children(fixture.world, root);
    REQUIRE(children.size() == 1U);
    const Entity field = children[0];
    const ComputedRect* rect = fixture.world.get<ComputedRect>(field);
    REQUIRE(rect != nullptr);
    REQUIRE(resonate::ui::updateInput(fixture.world, root, rect->x + 1.0F, rect->y + 1.0F,
                                      true));
    REQUIRE(resonate::ui::updateInput(fixture.world, root, rect->x + 1.0F, rect->y + 1.0F,
                                      false));
    REQUIRE(fixture.world.get<PointerState>(root)->focused == field);

    const auto content = [&]
    {
        const Text* component = fixture.world.get<Text>(field);
        REQUIRE(component != nullptr);
        REQUIRE(fixture.world.alive(component->content));
        return std::string(static_cast<const char*>(fixture.world.blobData(component->content)),
                           fixture.world.blobSize(component->content));
    };

    std::string error;
    REQUIRE(resonate::ui::updateTextComposition(fixture.world, root, runtime, "ni", error));
    CHECK(error.empty());
    CHECK(content() == "Ani");

    /* An unchanged preedit poll does not reshape or touch the atlas. */
    const TextAtlasPage* page = fixture.world.get<TextAtlasPage>(root);
    REQUIRE(page != nullptr);
    const std::uint32_t revision = page->revision;
    REQUIRE(resonate::ui::updateTextComposition(fixture.world, root, runtime, "ni", error));
    CHECK(fixture.world.get<TextAtlasPage>(root)->revision == revision);

    /* The commit replaces the preview rather than appending to it. */
    REQUIRE(resonate::ui::updateTextInput(fixture.world, root, runtime, "\xE4\xBD\xA0", false,
                                          error));
    CHECK(content() == "A\xE4\xBD\xA0");

    REQUIRE(resonate::ui::updateTextComposition(fixture.world, root, runtime, "hao", error));
    CHECK(content() == "A\xE4\xBD\xA0hao");
    REQUIRE(resonate::ui::updateTextComposition(fixture.world, root, runtime, "", error));
    CHECK(content() == "A\xE4\xBD\xA0");
}
