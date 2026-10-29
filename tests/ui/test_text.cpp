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
#include <resonate/ui/style.h>
#include <resonate/ui/input.h>
#include <resonate/ui/layout.h>
#include <resonate/ui/text_runtime.h>

#include "../../packages/ui/src/glyph_run.h"
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
using resonate::ui::TextCaret;
using resonate::ui::TextMetrics;

const char* const kLabel = R"xml(<document version="1" schemas="resonate.ui">
  <Screen key="hud">
    <Label key="greeting" text="Hi 你" font_size="24" color="0 0 0 0"
           width="160" height="32"/>
  </Screen>
</document>)xml";

bool near(float actual, float expected)
{
    const float difference = actual - expected;
    return difference > -0.001F && difference < 0.001F;
}

std::shared_ptr<const resonate::text::Font> loadFont()
{
    const std::string path =
        std::string(RESONATE_SOURCE_DIR) + "/content/ui/fonts/NotoSansSC-Regular.ttf";
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
    REQUIRE(list.texts().size() == 1U);
    /* H, i and 你 carry ink; the space only advances. */
    REQUIRE(list.texts()[0].glyphs.size() == 3U);
    for (const resonate::ui::TextGlyph& glyph : list.texts()[0].glyphs)
    {
        CHECK(glyph.glyph_id != 0U);
        CHECK(glyph.font_index == 0U);
        CHECK(glyph.pen_y > 0.0F);
    }

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
    REQUIRE(loadShippedStylesheets(*document));

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
        /* Empty text carries no blob at all. */
        if (!fixture.world.alive(component->content))
        {
            return std::string{};
        }
        return std::string(static_cast<const char*>(fixture.world.blobData(component->content)),
                           fixture.world.blobSize(component->content));
    };
    const TextAtlasPage* page = fixture.world.get<TextAtlasPage>(root);
    REQUIRE(page != nullptr);
    std::uint32_t revision = page->revision;

    std::string error;
    REQUIRE(resonate::ui::updateTextInput(fixture.world, root, runtime, "B", false, false, error));
    CHECK(error.empty());
    CHECK(content() == "AB");
    page = fixture.world.get<TextAtlasPage>(root);
    REQUIRE(page != nullptr);
    CHECK(page->revision == revision + 1U);
    revision = page->revision;

    /* The caret sits where the shaped run ends: the advance, not the last
       glyph's ink edge, which atlas padding pushes right. */
    const TextMetrics* metrics = fixture.world.get<TextMetrics>(field);
    REQUIRE(metrics != nullptr);
    CHECK(metrics->advance > 0.0F);
    resonate::ui::DisplayList caret_list;
    REQUIRE(resonate::ui::buildDisplayList(fixture.world, root, caret_list));
    bool caret_found = false;
    for (const resonate::ui::Quad& quad : caret_list.quads())
    {
        if (quad.texture == resonate::ui::kTextureWhite && quad.width == 1.0F)
        {
            CHECK(near(quad.x, rect->x + metrics->advance));
            caret_found = true;
        }
    }
    CHECK(caret_found);

    /* A control byte from a platform's key translation is not text. */
    REQUIRE(resonate::ui::updateTextInput(fixture.world, root, runtime, "\b", false, false, error));
    CHECK(content() == "AB");

    /* Backspace erases a whole code point, not one byte. */
    REQUIRE(resonate::ui::updateTextInput(fixture.world, root, runtime, "\xE4\xBD\xA0", false, false,
                                          error));
    CHECK(content() == "AB\xE4\xBD\xA0");
    REQUIRE(resonate::ui::updateTextInput(fixture.world, root, runtime, "", true, false, error));
    CHECK(content() == "AB");
    REQUIRE(resonate::ui::updateTextInput(fixture.world, root, runtime, "", true, false, error));
    CHECK(content() == "A");
    REQUIRE(resonate::ui::updateTextInput(fixture.world, root, runtime, "", true, false, error));
    CHECK(content().empty());
    REQUIRE(resonate::ui::updateTextInput(fixture.world, root, runtime, "", true, false, error));
    CHECK(content().empty());

    /* A press outside the field drops focus and typing becomes a no-op. */
    REQUIRE(resonate::ui::updateInput(fixture.world, root, -100.0F, -100.0F, true));
    REQUIRE(resonate::ui::updateInput(fixture.world, root, -100.0F, -100.0F, false));
    pointer = fixture.world.get<PointerState>(root);
    REQUIRE(pointer != nullptr);
    CHECK_FALSE(pointer->focused.valid());
    CHECK(carets() == 0U);
    REQUIRE(resonate::ui::updateTextInput(fixture.world, root, runtime, "Z", false, false, error));
    CHECK(content().empty());
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
        /* Empty text carries no blob at all. */
        if (!fixture.world.alive(component->content))
        {
            return std::string{};
        }
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
    REQUIRE(resonate::ui::updateTextInput(fixture.world, root, runtime, "\xE4\xBD\xA0", false, false,
                                          error));
    CHECK(content() == "A\xE4\xBD\xA0");

    REQUIRE(resonate::ui::updateTextComposition(fixture.world, root, runtime, "hao", error));
    CHECK(content() == "A\xE4\xBD\xA0hao");
    REQUIRE(resonate::ui::updateTextComposition(fixture.world, root, runtime, "", error));
    CHECK(content() == "A\xE4\xBD\xA0");
}

TEST_CASE("ui text: the caret edits in the middle of the field", "[ui][text]")
{
    const char* const text = R"xml(<document version="1" schemas="resonate.ui">
  <Screen key="hud">
    <Label key="field" text="AB" font_size="20" width="120" height="32" color="0 0 0 0">
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
    const Entity field = resonate::hierarchy::children(fixture.world, root)[0];
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
        if (!fixture.world.alive(component->content))
        {
            return std::string{};
        }
        return std::string(static_cast<const char*>(fixture.world.blobData(component->content)),
                           fixture.world.blobSize(component->content));
    };
    const auto caret = [&]
    {
        const TextCaret* component = fixture.world.get<TextCaret>(field);
        return component != nullptr ? component->offset : static_cast<std::uint32_t>(-1);
    };
    std::string error;

    /* Home puts the caret before the text; typing lands there. */
    CHECK(resonate::ui::moveCaret(fixture.world, root, resonate::ui::CaretMotion::Home));
    CHECK(caret() == 0U);
    REQUIRE(resonate::ui::updateTextInput(fixture.world, root, runtime, "X", false, false, error));
    CHECK(content() == "XAB");
    CHECK(caret() == 1U);

    /* Right steps over one code point; backspace removes the one before it. */
    CHECK(resonate::ui::moveCaret(fixture.world, root, resonate::ui::CaretMotion::Right));
    CHECK(caret() == 2U);
    REQUIRE(resonate::ui::updateTextInput(fixture.world, root, runtime, "", true, false, error));
    CHECK(content() == "XB");
    CHECK(caret() == 1U);

    /* Typing lands at the caret, not at the end, and a code point outside
       ASCII is one step. */
    REQUIRE(resonate::ui::updateTextInput(fixture.world, root, runtime, "\xE4\xBD\xA0", false, false,
                                          error));
    CHECK(content() == "X\xE4\xBD\xA0" "B");
    CHECK(resonate::ui::moveCaret(fixture.world, root, resonate::ui::CaretMotion::End));
    CHECK(caret() == 5U);
    CHECK(resonate::ui::moveCaret(fixture.world, root, resonate::ui::CaretMotion::Left));
    CHECK(caret() == 4U);
    CHECK(resonate::ui::moveCaret(fixture.world, root, resonate::ui::CaretMotion::Left));
    CHECK(caret() == 1U);

    /* The drawn caret follows the offset: mid-string it precedes the end. */
    resonate::ui::DisplayList list;
    REQUIRE(resonate::ui::buildDisplayList(fixture.world, root, list));
    float mid_caret = -1.0F;
    for (const resonate::ui::Quad& quad : list.quads())
    {
        if (quad.texture == resonate::ui::kTextureWhite && quad.width == 1.0F)
        {
            mid_caret = quad.x;
        }
    }
    REQUIRE(mid_caret >= 0.0F);
    const TextMetrics* metrics = fixture.world.get<TextMetrics>(field);
    REQUIRE(metrics != nullptr);
    CHECK(mid_caret < rect->x + metrics->advance);

    /* A press in the text drops the caret before the glyph whose cell it lands
       in, split at the cell's middle. */
    const Glyphs* glyphs = fixture.world.get<Glyphs>(field);
    REQUIRE(glyphs != nullptr);
    const auto* records = static_cast<const resonate::ui::detail::GlyphQuad*>(
        fixture.world.blobData(glyphs->records));
    REQUIRE(fixture.world.blobSize(glyphs->records) >= 3U * sizeof(*records));
    const float pen = records[1].pen_x;
    const float width = records[2].pen_x - pen;
    REQUIRE(resonate::ui::updateInput(fixture.world, root, rect->x + pen + width * 0.25F,
                                      rect->y + 1.0F, true));
    REQUIRE(resonate::ui::updateInput(fixture.world, root, rect->x + pen + width * 0.25F,
                                      rect->y + 1.0F, false));
    CHECK(caret() == 1U);
    REQUIRE(resonate::ui::updateInput(fixture.world, root, rect->x + pen + width * 0.75F,
                                      rect->y + 1.0F, true));
    REQUIRE(resonate::ui::updateInput(fixture.world, root, rect->x + pen + width * 0.75F,
                                      rect->y + 1.0F, false));
    CHECK(caret() == 4U);

    /* Delete removes the code point after the caret and leaves it in place. */
    CHECK(resonate::ui::moveCaret(fixture.world, root, resonate::ui::CaretMotion::Left));
    CHECK(caret() == 1U);
    REQUIRE(resonate::ui::updateTextInput(fixture.world, root, runtime, "", false, true, error));
    CHECK(content() == "XB");
    CHECK(caret() == 1U);
}

TEST_CASE("ui editor: the editor document instantiates", "[ui][text]")
{
    std::ifstream file(std::string(RESONATE_SOURCE_DIR) + "/content/ui/editor.rui");
    REQUIRE(file.good());
    const std::string source((std::istreambuf_iterator<char>(file)),
                             std::istreambuf_iterator<char>());
    std::vector<resonate::dom::Diagnostic> diagnostics;
    auto document = resonate::dom::Document::parse(source, &resonate::ui::documentSchema(),
                                                   diagnostics);
    REQUIRE(document != nullptr);
    CHECK(diagnostics.empty());

    resonate::ui::TextRuntime runtime({loadFont()});
    Fixture fixture;
    const Entity root = resonate::ui::instantiate(fixture.world, *document, nullptr, runtime);
    REQUIRE(root.valid());
    REQUIRE(resonate::ui::layout(fixture.world, root, 1600.0F, 900.0F));

    /* The panes, toolbar, tree rows and inspector all paint. */
    resonate::ui::DisplayList list;
    REQUIRE(resonate::ui::buildDisplayList(fixture.world, root, list));
    CHECK(list.size() > 10U);

    /* The panes follow the target: re-laying out at another size keeps the
       inspector on the right edge and the status line at the bottom. */
    REQUIRE(resonate::ui::layout(fixture.world, root, 1920.0F, 1200.0F));
    const auto check_edge = [&](const char* key, bool along_x, float expected)
    {
        const Entity pane = resonate::ui::findByKey(fixture.world, root, key);
        REQUIRE(pane.valid());
        const resonate::ui::ComputedRect* rect = fixture.world.get<resonate::ui::ComputedRect>(pane);
        REQUIRE(rect != nullptr);
        const float edge = along_x ? rect->x + rect->width : rect->y + rect->height;
        CHECK(std::fabs(edge - expected) < 0.5F);
    };
    check_edge("inspector", true, 1920.0F);
    check_edge("status-bar", false, 1200.0F);
}

TEST_CASE("ui gallery: the widget gallery instantiates", "[ui][text]")
{
    std::ifstream file(std::string(RESONATE_SOURCE_DIR) + "/content/ui/gallery.rui");
    REQUIRE(file.good());
    const std::string source((std::istreambuf_iterator<char>(file)),
                             std::istreambuf_iterator<char>());
    std::vector<resonate::dom::Diagnostic> diagnostics;
    auto document = resonate::dom::Document::parse(source, &resonate::ui::documentSchema(),
                                                   diagnostics);
    REQUIRE(document != nullptr);
    CHECK(diagnostics.empty());
    REQUIRE(loadShippedStylesheets(*document));

    resonate::ui::TextRuntime runtime({loadFont()});
    Fixture fixture;
    const Entity root = resonate::ui::instantiate(fixture.world, *document, nullptr, runtime);
    REQUIRE(root.valid());
    REQUIRE(resonate::ui::layout(fixture.world, root, 1000.0F, 720.0F));

    /* The toggle glyphs and slider tracks paint on top of the panels. */
    resonate::ui::DisplayList list;
    REQUIRE(resonate::ui::buildDisplayList(fixture.world, root, list));
    CHECK(list.size() > 20U);
}

TEST_CASE("ui document: widgets are reachable by their document key", "[ui][document]")
{
    const std::string path = std::string(RESONATE_SOURCE_DIR) + "/content/ui/gallery.rui";
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.is_open());
    const std::string text((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    std::vector<resonate::dom::Diagnostic> diagnostics;
    auto document = resonate::dom::Document::parse(text, &resonate::ui::documentSchema(),
                                                   diagnostics);
    REQUIRE(document != nullptr);
    REQUIRE(loadShippedStylesheets(*document));

    resonate::ui::TextRuntime runtime({loadFont()});
    Fixture fixture;
    const Entity root = resonate::ui::instantiate(fixture.world, *document, nullptr, runtime);
    REQUIRE(root.valid());

    const Entity dropdown = resonate::ui::findByKey(fixture.world, root, "dropdown");
    REQUIRE(dropdown.valid());
    CHECK(fixture.world.get<resonate::ui::Dropdown>(dropdown) != nullptr);
    const Entity slider_b = resonate::ui::findByKey(fixture.world, root, "slider-b");
    REQUIRE(slider_b.valid());
    CHECK(fixture.world.get<resonate::ui::Slider>(slider_b) != nullptr);
    CHECK_FALSE(resonate::ui::findByKey(fixture.world, root, "no-such-widget").valid());
}

TEST_CASE("ui document: the selected row and widget text answer by key", "[ui][document]")
{
    std::ifstream file(std::string(RESONATE_SOURCE_DIR) + "/content/ui/gallery.rui");
    REQUIRE(file.good());
    const std::string source((std::istreambuf_iterator<char>(file)),
                             std::istreambuf_iterator<char>());
    std::vector<resonate::dom::Diagnostic> diagnostics;
    auto document = resonate::dom::Document::parse(source, &resonate::ui::documentSchema(),
                                                   diagnostics);
    REQUIRE(document != nullptr);
    REQUIRE(loadShippedStylesheets(*document));

    resonate::ui::TextRuntime runtime({loadFont()});
    Fixture fixture;
    const Entity root = resonate::ui::instantiate(fixture.world, *document, nullptr, runtime);
    REQUIRE(root.valid());
    REQUIRE(resonate::ui::layout(fixture.world, root, 1000.0F, 1080.0F));

    const Entity list = resonate::ui::findByKey(fixture.world, root, "list");
    REQUIRE(list.valid());
    const Entity row = resonate::ui::findByKey(fixture.world, root, "list-row-1");
    REQUIRE(row.valid());
    /* A row is a button wrapping its label; the text comes from the label. */
    CHECK(resonate::ui::widgetText(fixture.world, row) == "平行光");

    resonate::ui::Selectable* selectable = fixture.world.get<resonate::ui::Selectable>(row);
    REQUIRE(selectable != nullptr);
    selectable->selected = true;
    CHECK(resonate::ui::selectedChild(fixture.world, list) == row);

    /* Application code reshapes a label through the canvas' runtime. */
    const Entity title = resonate::ui::findByKey(fixture.world, root, "text-title");
    REQUIRE(title.valid());
    std::string error;
    REQUIRE(resonate::ui::setText(fixture.world, root, title, "已更新", error));
    CHECK(resonate::ui::widgetText(fixture.world, title) == "已更新");
}

TEST_CASE("ui text: the caret hides while the blink phase is off", "[ui][text]")
{
    const char* const text = R"xml(<document version="1" schemas="resonate.ui">
  <Screen key="hud">
    <Label key="field" text="AB" font_size="20" width="120" height="32" color="0 0 0 0">
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
    const Entity field = resonate::hierarchy::children(fixture.world, root)[0];
    REQUIRE(resonate::ui::updateInput(fixture.world, root, 10.0F, 10.0F, true));
    REQUIRE(resonate::ui::updateInput(fixture.world, root, 10.0F, 10.0F, false));

    const auto carets = [&]
    {
        resonate::ui::DisplayList list;
        REQUIRE(resonate::ui::buildDisplayList(fixture.world, root, list));
        std::size_t count = 0U;
        for (const resonate::ui::Quad& quad : list.quads())
        {
            count += quad.texture == resonate::ui::kTextureWhite && quad.width == 1.0F ? 1U : 0U;
        }
        return count;
    };
    CHECK(carets() == 1U);

    /* The blink phase hides it; the field itself stays focused. */
    REQUIRE(fixture.world.add(root, resonate::ui::CaretBlink{false}) != nullptr);
    CHECK(carets() == 0U);
    resonate::ui::CaretBlink* blink = fixture.world.get<resonate::ui::CaretBlink>(root);
    REQUIRE(blink != nullptr);
    blink->visible = true;
    CHECK(carets() == 1U);
    (void)field;
}

TEST_CASE("ui text: a long field scrolls its own text so the caret stays visible", "[ui][text]")
{
    const char* const text = R"xml(<document version="1" schemas="resonate.ui">
  <Screen key="hud">
    <Label key="field" text="ABCDEFGHIJKLMNOP" font_size="16" width="60" height="24"
           wrap="false" color="0 0 0 0">
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
    const Entity field = resonate::hierarchy::children(fixture.world, root)[0];
    const resonate::ui::ComputedRect* rect = fixture.world.get<resonate::ui::ComputedRect>(field);
    REQUIRE(rect != nullptr);
    REQUIRE(resonate::ui::updateInput(fixture.world, root, rect->x + 5.0F, rect->y + 5.0F, true));
    REQUIRE(resonate::ui::updateInput(fixture.world, root, rect->x + 5.0F, rect->y + 5.0F, false));
    /* The press placed the caret where it landed; the end is where the text
       has to scroll under it. */
    REQUIRE(resonate::ui::moveCaret(fixture.world, root, resonate::ui::CaretMotion::End));

    resonate::ui::DisplayList list;
    REQUIRE(resonate::ui::buildDisplayList(fixture.world, root, list));
    REQUIRE_FALSE(list.texts().empty());
    /* The run starts left of the box: the leading glyphs scrolled out. */
    CHECK(list.texts()[0].glyphs.front().pen_x < rect->x);

    float caret_x = -1.0F;
    for (const resonate::ui::Quad& quad : list.quads())
    {
        if (quad.texture == resonate::ui::kTextureWhite && quad.width == 1.0F)
        {
            caret_x = quad.x;
        }
    }
    REQUIRE(caret_x >= 0.0F);
    /* The caret sits inside the field, not past its right edge. */
    CHECK(caret_x >= rect->x);
    CHECK(caret_x <= rect->x + rect->width);
}

/* A wholesale replacement clears the selection before shaping. The Text row the
   call cached first is gone by then: the field must still shape with its own
   font, and the entity that backfilled the row must keep its content. */
TEST_CASE("ui text: a replacement after a selection keeps the field's own font",
          "[ui][text]")
{
    const char* const text = R"xml(<document version="1" schemas="resonate.ui">
  <Screen key="hud">
    <Label key="big" text="A" font_size="40" width="200" height="60"
           color="0 0 0 0"/>
    <Label key="small" text="B" font_size="12" width="200" height="60"
           color="0 0 0 0"/>
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
    const Entity big = resonate::ui::findByKey(fixture.world, root, "big");
    const Entity small = resonate::ui::findByKey(fixture.world, root, "small");
    REQUIRE(big.valid());
    REQUIRE(small.valid());

    /* Both selections put the labels in one archetype. Clearing big's moves it
       out and swaps small into the row the cached Text pointer still names. */
    fixture.world.add(big, resonate::ui::TextSelection{0U});
    fixture.world.add(small, resonate::ui::TextSelection{0U});
    REQUIRE(fixture.world.get<resonate::ui::TextSelection>(big) != nullptr);

    std::string error;
    REQUIRE(runtime.setText(fixture.world, root, big, "AB", error));
    CHECK(error.empty());

    const TextMetrics* metrics = fixture.world.get<TextMetrics>(big);
    REQUIRE(metrics != nullptr);
    /* 40 px against 12 px: a stale read would shape the field at 12. */
    CHECK(metrics->ascent > 30.0F);

    /* The backfilled entity keeps its own bytes and glyphs. */
    const Text* small_text = fixture.world.get<Text>(small);
    REQUIRE(small_text != nullptr);
    REQUIRE(fixture.world.alive(small_text->content));
    const Glyphs* small_glyphs = fixture.world.get<Glyphs>(small);
    REQUIRE(small_glyphs != nullptr);
    REQUIRE(fixture.world.alive(small_glyphs->records));
}

TEST_CASE("ui text: an empty field shapes its placeholder in the hint color", "[ui][text]")
{
    const char* const text = R"xml(<document version="1" schemas="resonate.ui">
  <Screen key="hud">
    <Label key="field" text="" placeholder="输入命令" font_size="20" width="240" height="32"
           color="0 0 0 0" placeholder_color="0.2 0.4 0.6 0.5"/>
  </Screen>
</document>)xml";
    std::vector<resonate::dom::Diagnostic> diagnostics;
    auto document =
        resonate::dom::Document::parse(text, &resonate::ui::documentSchema(), diagnostics);
    REQUIRE(document != nullptr);
    CHECK(diagnostics.empty());

    resonate::ui::TextRuntime runtime({loadFont()});
    Fixture fixture;
    const Entity root = resonate::ui::instantiate(fixture.world, *document, nullptr, runtime);
    REQUIRE(root.valid());
    const auto children = resonate::hierarchy::children(fixture.world, root);
    REQUIRE(children.size() == 1U);
    const Entity field = children[0];
    REQUIRE(resonate::ui::layout(fixture.world, root, 320.0F, 64.0F));

    const auto run_color = [&]()
    {
        resonate::ui::DisplayList list;
        REQUIRE(resonate::ui::buildDisplayList(fixture.world, root, list));
        REQUIRE(list.texts().size() == 1U);
        REQUIRE(!list.texts()[0].glyphs.empty());
        return list.texts()[0].color;
    };

    /* No content: the placeholder's bytes are the run, painted dim. */
    const resonate::ui::Text* component = fixture.world.get<resonate::ui::Text>(field);
    REQUIRE(component != nullptr);
    CHECK_FALSE(fixture.world.alive(component->content));
    resonate::Vec4 color = run_color();
    CHECK(std::fabs(color.z - 0.6F) < 0.001F);
    CHECK(std::fabs(color.w - 0.5F) < 0.001F);

    /* Typed text replaces the hint and takes the field's own color. */
    std::string error;
    REQUIRE(runtime.setText(fixture.world, root, field, "set a 1", error));
    REQUIRE(resonate::ui::layout(fixture.world, root, 320.0F, 64.0F));
    color = run_color();
    CHECK(color.z > 0.9F);
    CHECK(std::fabs(color.w - 1.0F) < 0.001F);

    /* Clearing the field brings the hint back. */
    REQUIRE(runtime.setText(fixture.world, root, field, "", error));
    REQUIRE(resonate::ui::layout(fixture.world, root, 320.0F, 64.0F));
    color = run_color();
    CHECK(std::fabs(color.z - 0.6F) < 0.001F);
}
