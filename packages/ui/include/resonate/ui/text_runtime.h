#ifndef RESONATE_UI_TEXT_RUNTIME_H
#define RESONATE_UI_TEXT_RUNTIME_H

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

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
};

/* Applies typed UTF-8 to the canvas' focused Input that carries Text. Backspace
   removes one UTF-8 code point first, then `utf8` is appended. A canvas with no
   focused text field is a no-op. */
[[nodiscard]] bool updateTextInput(ecs::World& world, ecs::Entity canvas, TextRuntime& runtime,
                                   std::string_view utf8, bool backspace, std::string& error);

} // namespace resonate::ui

#endif /* RESONATE_UI_TEXT_RUNTIME_H */
