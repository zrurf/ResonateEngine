#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <vector>

#include <resonate/core/allocator.h>
#include <resonate/dom/document.h>
#include <resonate/ecs/command_buffer.h>
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

using resonate::ecs::CommandBuffer;
using resonate::ecs::Entity;
using resonate::ecs::World;
using resonate::ui::ComputedRect;
using resonate::ui::Text;

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
    CommandBuffer commands{world};
    Entity canvas{};

    Fixture()
    {
        REQUIRE(resonate::hierarchy::registerComponents(world));
        REQUIRE(resonate::ui::registerComponents(world));
        canvas = world.create();
        REQUIRE(canvas.valid());
        world.add(canvas, resonate::ui::Canvas{});
        world.add(canvas, ComputedRect{});
    }
};

/* One focused field with shaped text, driven through the pointer: the shared
   setup of every selection case below. */
struct SelectionFixture
{
    World& world;
    resonate::ui::TextRuntime runtime;
    Entity root{};
    Entity field{};
    ComputedRect rect{};

    explicit SelectionFixture(World& target)
        : world(target), runtime({loadFont()})
    {
        const char* const text = R"xml(<document version="1" schemas="resonate.ui">
  <Screen key="hud">
    <Label key="field" text="hello world" font_size="20" width="220" height="32"
           color="0 0 0 0">
      <Input/>
    </Label>
  </Screen>
</document>)xml";
        std::vector<resonate::dom::Diagnostic> diagnostics;
        auto document = resonate::dom::Document::parse(text, &resonate::ui::documentSchema(),
                                                       diagnostics);
        REQUIRE(document != nullptr);
        CHECK(diagnostics.empty());
        root = resonate::ui::instantiate(world, *document, nullptr, runtime);
        REQUIRE(root.valid());
        REQUIRE(resonate::ui::layout(world, root, 300.0F, 200.0F));
        field = resonate::hierarchy::children(world, root)[0];
        const ComputedRect* computed = world.get<ComputedRect>(field);
        REQUIRE(computed != nullptr);
        rect = *computed;
    }

    /* Press at the run's start, drag to its end, release. */
    void selectAll()
    {
        const float mid_y = rect.y + rect.height * 0.5F;
        REQUIRE(resonate::ui::updateInput(world, root, rect.x + 1.0F, mid_y, true));
        REQUIRE(resonate::ui::updateInput(world, root, rect.x + rect.width - 1.0F, mid_y, true));
        REQUIRE(resonate::ui::updateInput(world, root, rect.x + rect.width - 1.0F, mid_y, false));
    }

    [[nodiscard]] std::string content() const
    {
        const Text* component = world.get<Text>(field);
        REQUIRE(component != nullptr);
        if (!world.alive(component->content))
        {
            return {};
        }
        return std::string(static_cast<const char*>(world.blobData(component->content)),
                           world.blobSize(component->content));
    }
};

} // namespace

TEST_CASE("ui selection: a drag selects and typing replaces the range", "[ui][text][selection]")
{
    Fixture fixture;
    SelectionFixture selection(fixture.world);

    selection.selectAll();
    std::uint32_t select_begin = 0U;
    std::uint32_t select_end = 0U;
    REQUIRE(resonate::ui::selectionRange(selection.world, selection.field, select_begin,
                                         select_end));
    CHECK(select_begin == 0U);
    CHECK(select_end == selection.content().size());

    std::string error;
    REQUIRE(resonate::ui::updateTextInput(selection.world, selection.root, selection.runtime, "X",
                                          false, false, error));
    CHECK(selection.content() == "X");
    REQUIRE(resonate::ui::selectionRange(selection.world, selection.field, select_begin,
                                         select_end));
    CHECK(select_begin == select_end);
}

TEST_CASE("ui selection: backspace deletes the range and arrows collapse it",
          "[ui][text][selection]")
{
    Fixture fixture;
    SelectionFixture selection(fixture.world);

    selection.selectAll();
    std::string error;
    REQUIRE(resonate::ui::updateTextInput(selection.world, selection.root, selection.runtime, "",
                                          true, false, error));
    CHECK(selection.content().empty());

    /* Refill and select again; the left arrow collapses at the selection's
       start instead of stepping or deleting. */
    REQUIRE(resonate::ui::setText(selection.world, selection.root, selection.field, "hello",
                                  error));
    selection.selectAll();
    std::uint32_t select_begin = 0U;
    std::uint32_t select_end = 0U;
    REQUIRE(resonate::ui::selectionRange(selection.world, selection.field, select_begin,
                                         select_end));
    CHECK(select_begin < select_end);
    REQUIRE(resonate::ui::moveCaret(selection.world, selection.root,
                                    resonate::ui::CaretMotion::Left));
    REQUIRE(resonate::ui::selectionRange(selection.world, selection.field, select_begin,
                                         select_end));
    CHECK(select_begin == select_end);
    const resonate::ui::TextCaret* caret = selection.world.get<resonate::ui::TextCaret>(selection.field);
    REQUIRE(caret != nullptr);
    CHECK(caret->offset == 0U);
}

TEST_CASE("ui selection: the highlight draws behind the glyphs", "[ui][text][selection]")
{
    Fixture fixture;
    SelectionFixture selection(fixture.world);

    resonate::ui::DisplayList plain;
    REQUIRE(resonate::ui::buildDisplayList(selection.world, selection.root, plain));
    std::size_t highlights = 0U;
    for (const resonate::ui::Quad& quad : plain.quads())
    {
        if (quad.color.w > 0.0F && quad.color.w < 0.9F)
        {
            ++highlights;
        }
    }
    CHECK(highlights == 0U);

    selection.selectAll();
    resonate::ui::DisplayList selected;
    REQUIRE(resonate::ui::buildDisplayList(selection.world, selection.root, selected));
    highlights = 0U;
    float highlighted_width = 0.0F;
    for (const resonate::ui::Quad& quad : selected.quads())
    {
        if (quad.color.w > 0.0F && quad.color.w < 0.9F)
        {
            ++highlights;
            highlighted_width += quad.width;
        }
    }
    /* One column per selected glyph's cell, covering the whole shaped run. */
    const resonate::ui::TextMetrics* metrics =
        selection.world.get<resonate::ui::TextMetrics>(selection.field);
    REQUIRE(metrics != nullptr);
    CHECK(highlights > 1U);
    CHECK(highlighted_width > metrics->advance * 0.9F);
}

TEST_CASE("ui selection: shift arrows grow and shrink the range", "[ui][text][selection]")
{
    Fixture fixture;
    SelectionFixture selection(fixture.world);
    const float mid_y = selection.rect.y + selection.rect.height * 0.5F;
    REQUIRE(resonate::ui::updateInput(selection.world, selection.root, selection.rect.x + 1.0F,
                                      mid_y, true));
    REQUIRE(resonate::ui::updateInput(selection.world, selection.root, selection.rect.x + 1.0F,
                                      mid_y, false));

    std::uint32_t select_begin = 0U;
    std::uint32_t select_end = 0U;
    REQUIRE(resonate::ui::moveCaret(selection.world, selection.root,
                                    resonate::ui::CaretMotion::Right, true));
    REQUIRE(resonate::ui::moveCaret(selection.world, selection.root,
                                    resonate::ui::CaretMotion::Right, true));
    REQUIRE(resonate::ui::selectionRange(selection.world, selection.field, select_begin,
                                         select_end));
    CHECK(select_begin == 0U);
    CHECK(select_end == 2U);

    /* Shift+Left shrinks the same range from the moving edge. */
    REQUIRE(resonate::ui::moveCaret(selection.world, selection.root,
                                    resonate::ui::CaretMotion::Left, true));
    REQUIRE(resonate::ui::selectionRange(selection.world, selection.field, select_begin,
                                         select_end));
    CHECK(select_begin == 0U);
    CHECK(select_end == 1U);

    /* Shift+End keeps the anchor and takes the caret to the end. */
    REQUIRE(resonate::ui::moveCaret(selection.world, selection.root,
                                    resonate::ui::CaretMotion::End, true));
    REQUIRE(resonate::ui::selectionRange(selection.world, selection.field, select_begin,
                                         select_end));
    CHECK(select_begin == 0U);
    CHECK(select_end == selection.content().size());

    std::string error;
    REQUIRE(resonate::ui::updateTextInput(selection.world, selection.root, selection.runtime, "X",
                                          false, false, error));
    CHECK(selection.content() == "X");
}

TEST_CASE("ui selection: word motions step over separator runs", "[ui][text][selection]")
{
    Fixture fixture;
    SelectionFixture selection(fixture.world);
    const float mid_y = selection.rect.y + selection.rect.height * 0.5F;
    REQUIRE(resonate::ui::updateInput(selection.world, selection.root, selection.rect.x + 1.0F,
                                      mid_y, true));
    REQUIRE(resonate::ui::updateInput(selection.world, selection.root, selection.rect.x + 1.0F,
                                      mid_y, false));

    /* "hello world": caret at 0, the word motions land at the ends of the
       words and at their starts on the way back. */
    const auto caret = [&]()
    {
        const resonate::ui::TextCaret* component =
            selection.world.get<resonate::ui::TextCaret>(selection.field);
        REQUIRE(component != nullptr);
        return component->offset;
    };
    REQUIRE(resonate::ui::moveCaret(selection.world, selection.root,
                                    resonate::ui::CaretMotion::WordRight));
    CHECK(caret() == 5U);
    REQUIRE(resonate::ui::moveCaret(selection.world, selection.root,
                                    resonate::ui::CaretMotion::WordRight));
    CHECK(caret() == 11U);
    REQUIRE(resonate::ui::moveCaret(selection.world, selection.root,
                                    resonate::ui::CaretMotion::WordLeft));
    CHECK(caret() == 6U);
    REQUIRE(resonate::ui::moveCaret(selection.world, selection.root,
                                    resonate::ui::CaretMotion::WordLeft));
    CHECK(caret() == 0U);

    /* A word motion with the anchor held selects the whole word. */
    REQUIRE(resonate::ui::moveCaret(selection.world, selection.root,
                                    resonate::ui::CaretMotion::WordRight, true));
    std::uint32_t select_begin = 0U;
    std::uint32_t select_end = 0U;
    REQUIRE(resonate::ui::selectionRange(selection.world, selection.field, select_begin,
                                         select_end));
    CHECK(selection.content().substr(select_begin, select_end - select_begin) == "hello");
}

TEST_CASE("ui selection: select all covers the field without a drag", "[ui][text][selection]")
{
    Fixture fixture;
    SelectionFixture selection(fixture.world);
    const float mid_y = selection.rect.y + selection.rect.height * 0.5F;
    REQUIRE(resonate::ui::updateInput(selection.world, selection.root, selection.rect.x + 1.0F,
                                      mid_y, true));
    REQUIRE(resonate::ui::updateInput(selection.world, selection.root, selection.rect.x + 1.0F,
                                      mid_y, false));

    REQUIRE(resonate::ui::selectAll(selection.world, selection.root));
    std::uint32_t select_begin = 0U;
    std::uint32_t select_end = 0U;
    REQUIRE(resonate::ui::selectionRange(selection.world, selection.field, select_begin,
                                         select_end));
    CHECK(select_begin == 0U);
    CHECK(select_end == selection.content().size());

    std::string error;
    REQUIRE(resonate::ui::updateTextInput(selection.world, selection.root, selection.runtime, "",
                                          true, false, error));
    CHECK(selection.content().empty());
}

TEST_CASE("ui selection: a double click selects the word under the pointer",
          "[ui][text][selection]")
{
    Fixture fixture;
    SelectionFixture selection(fixture.world);
    const float mid_y = selection.rect.y + selection.rect.height * 0.5F;

    /* "hello world": click into the second word twice inside the window. The
       first press lands in "hello" or before "world"; the double click must
       select "world" whole either way, so aim where the words split: press
       once at the far right (end of the run), then again at the middle of
       "world". Simpler and stable: press once at the end, then again at the
       same spot after 0.2 s. */
    const auto press = [&](float x, double at)
    {
        REQUIRE(resonate::ui::updateInput(selection.world, selection.root, x, mid_y, true, at));
        REQUIRE(resonate::ui::updateInput(selection.world, selection.root, x, mid_y, false, at));
    };
    press(selection.rect.x + 1.0F, 0.0);
    press(selection.rect.x + selection.rect.width - 1.0F, 0.25);

    std::uint32_t select_begin = 0U;
    std::uint32_t select_end = 0U;
    REQUIRE(resonate::ui::selectionRange(selection.world, selection.field, select_begin,
                                         select_end));
    CHECK(selection.content().substr(select_begin, select_end - select_begin) == "world");

    /* Directly: the word under a click in "hello" is "hello"; past the run it
       is the last word. */
    const resonate::ui::TextMetrics* metrics =
        selection.world.get<resonate::ui::TextMetrics>(selection.field);
    REQUIRE(metrics != nullptr);
    REQUIRE(resonate::ui::selectWordAt(selection.world, selection.field, metrics->advance * 0.25F));
    REQUIRE(resonate::ui::selectionRange(selection.world, selection.field, select_begin,
                                         select_end));
    CHECK(selection.content().substr(select_begin, select_end - select_begin) == "hello");
    REQUIRE(resonate::ui::selectWordAt(selection.world, selection.field, metrics->advance + 5.0F));
    REQUIRE(resonate::ui::selectionRange(selection.world, selection.field, select_begin,
                                         select_end));
    CHECK(selection.content().substr(select_begin, select_end - select_begin) == "world");
}
