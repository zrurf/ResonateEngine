#ifndef RESONATE_UI_TEXT_RUNTIME_H
#define RESONATE_UI_TEXT_RUNTIME_H

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <resonate/ecs/component.h>
#include <resonate/ecs/entity.h>
#include <resonate/ecs/world.h>
#include <resonate/text/font.h>

namespace resonate::ui
{

namespace detail
{
class TextAtlasBuilder;
}

/* The live text state behind one canvas: the font chain and the atlas
   builder. Documents instantiated through a TextRuntime can change a Label's
   text after the fact; the runtime reshapes the run, grows/repacks the atlas
   when needed, refreshes every glyph run's UVs, and bumps the atlas revision
   so the renderer re-uploads it. A runtime belongs to one canvas. */
class TextRuntime
{
  public:
    explicit TextRuntime(std::vector<std::shared_ptr<const text::Font>> fonts);
    ~TextRuntime();
    TextRuntime(const TextRuntime&) = delete;
    TextRuntime& operator=(const TextRuntime&) = delete;

    /* Replaces a Label's UTF-8 content and rebuilds its glyphs. False with a
       named error when the entity has no Text or the atlas cannot grow. */
    [[nodiscard]] bool setText(ecs::World& world, ecs::Entity canvas, ecs::Entity entity,
                               std::string_view utf8, std::string& error);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    friend struct TextRuntimeAccess;
};

/* The document adapter's narrow view of the runtime; not part of the public
   surface a game uses. */
struct TextRuntimeAccess
{
    static detail::TextAtlasBuilder* atlas(TextRuntime& runtime);
    static void attach(TextRuntime& runtime, ecs::Entity canvas);
    static std::uint32_t revision(const TextRuntime& runtime);
    /* Restores the committed text unless the preedit belongs to `keep`. */
    static bool restoreComposition(ecs::World& world, ecs::Entity keep, TextRuntime& runtime,
                                   std::string& error);
    static bool updateComposition(ecs::World& world, ecs::Entity canvas, TextRuntime& runtime,
                                  std::string_view utf8, std::string& error);
};

/* Where a caret key takes the offset. */
enum class CaretMotion : std::uint8_t
{
    Left,
    Right,
    Home,
    End,
};

/* The canvas' text runtime, attached at instantiation so application code can
   reshape a label without holding the runtime itself. Runtime data, never
   authored. */
struct TextRuntimeRef
{
    TextRuntime* runtime = nullptr;
};

/* Replaces a widget's text through the canvas' runtime. False with a named
   error when the canvas carries no runtime or the entity has no Text. */
[[nodiscard]] bool setText(ecs::World& world, ecs::Entity canvas, ecs::Entity entity,
                           std::string_view utf8, std::string& error);

/* Applies typed UTF-8 to the canvas' focused Input that carries Text: the text
   lands at the caret, backspace removes the code point before it and
   delete_forward the one after (backspace wins when both are set). A field
   without a caret component edits at the end of its content. A canvas with no
   focused text field is a no-op. */
[[nodiscard]] bool updateTextInput(ecs::World& world, ecs::Entity canvas, TextRuntime& runtime,
                                   std::string_view utf8, bool backspace, bool delete_forward,
                                   std::string& error);

/* Points a text widget's caret at the widget-local `x`, before the glyph that
   starts at or past it: a click in the text places the caret. False when the
   widget carries no Text. */
[[nodiscard]] bool placeCaretAt(ecs::World& world, ecs::Entity entity, float local_x);

/* Starts a selection gesture: the caret lands at widget-local `x` and the
   selection anchor follows it, so the range is empty until a drag moves the
   caret. False when the widget carries no Text. */
[[nodiscard]] bool beginTextSelect(ecs::World& world, ecs::Entity entity, float local_x);

/* Moves the caret to widget-local `x` without moving the anchor, which is how
   a drag extends a selection. False when the widget carries no Text. */
[[nodiscard]] bool extendCaretTo(ecs::World& world, ecs::Entity entity, float local_x);

/* Selects the word under widget-local `x`: a run of letters, digits,
   underscore or non-ASCII code points; a separator selects itself. False when
   the widget carries no Text. */
[[nodiscard]] bool selectWordAt(ecs::World& world, ecs::Entity entity, float local_x);

/* The selected byte range of a text field, empty (begin == end) when none.
   False when the entity carries no Text. */
[[nodiscard]] bool selectionRange(ecs::World& world, ecs::Entity entity,
                                  std::uint32_t& out_begin, std::uint32_t& out_end);

/* Moves the focused field's caret one code point or to an edge; false when no
   text field has focus. */
[[nodiscard]] bool moveCaret(ecs::World& world, ecs::Entity canvas, CaretMotion motion);

/* Shows an IME preedit in the focused field without committing it: the field
   renders its committed text followed by `utf8`. An empty preedit restores the
   committed text; changing focus first restores the field that held it. */
[[nodiscard]] bool updateTextComposition(ecs::World& world, ecs::Entity canvas,
                                         TextRuntime& runtime, std::string_view utf8,
                                         std::string& error);

/* Replaces the focused field's whole text. A platform that owns its own text
   editor (Android's IME connection) reports that editor's buffer instead of
   keystrokes, so the field mirrors the buffer rather than replaying edits.
   Answers true without touching anything when no text widget has focus. */
[[nodiscard]] bool setFocusedText(ecs::World& world, ecs::Entity canvas, TextRuntime& runtime,
                                  std::string_view utf8, std::string& error);

/* True when the widget's Text is an editable field rather than a caption: it
   carries Input and none of the components that own their own interaction
   (checkbox, slider, dropdown, scrubber, tab, selectable, splitter). Those
   widgets draw their text but take no caret and no typing. */
[[nodiscard]] bool isTextField(ecs::World& world, ecs::Entity entity);

/* True when the canvas' focused widget is a text field, which is when a
   platform should show its soft keyboard and the caret belongs on screen. */
[[nodiscard]] bool focusedTextEditable(ecs::World& world, ecs::Entity canvas);

/* Makes every dropdown's header read its selected row's caption, so a choice
   shows without every caller relaying it. A row's caption is its own Text or
   the first Text under it. Called after input and before layout; a header that
   already reads the caption is left alone, so this costs one subtree walk. */
[[nodiscard]] bool syncDropdowns(ecs::World& world, ecs::Entity canvas, std::string& error);

/* The canvas' focused widget, or an invalid entity when none has focus. */
[[nodiscard]] ecs::Entity focusedEntity(ecs::World& world, ecs::Entity canvas);

/* The focused text field's content, or empty when none has focus. A platform
   editor owns the buffer while a field is focused, so the engine pushes this
   into it first and mirrors what the editor then reports. */
[[nodiscard]] std::string focusedText(ecs::World& world, ecs::Entity canvas);

} // namespace resonate::ui

namespace resonate::ecs
{

/* The runtime reference is the host's own handle, never authored. */
RESONATE_COMPONENT(resonate::ui::TextRuntimeRef, "resonate.ui.TextRuntimeRef");

} // namespace resonate::ecs

#endif /* RESONATE_UI_TEXT_RUNTIME_H */
