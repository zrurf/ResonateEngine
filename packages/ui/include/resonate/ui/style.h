#ifndef RESONATE_UI_STYLE_H
#define RESONATE_UI_STYLE_H

#include <memory>
#include <vector>

#include <resonate/dom/document.h>

namespace resonate::ui
{

/*
 * The UI style compiler's output: the rules a document's StyleSheet nodes
 * declare, already ordered by priority. Resolving a widget returns its
 * components with style fields filled in; fields the document authored itself
 * always win, so a style never silently overrides an explicit value.
 */
class StyleTable
{
  public:
    StyleTable();
    ~StyleTable();

    StyleTable(StyleTable&&) noexcept;
    StyleTable& operator=(StyleTable&&) noexcept;
    StyleTable(const StyleTable&) = delete;
    StyleTable& operator=(const StyleTable&) = delete;

    [[nodiscard]] bool empty() const noexcept;

    /* `state` is a WidgetState value: 0 normal, 1 hovered, 2 pressed,
       3 disabled. State rules apply on top of the base rules. */
    [[nodiscard]] std::vector<dom::Component> resolve(const dom::Document& document,
                                                      dom::NodeHandle node,
                                                      std::uint32_t state = 0U) const;

  private:
    struct Impl;
    friend bool buildStyles(dom::Document& document, StyleTable& out,
                            std::vector<dom::Diagnostic>& diagnostics);
    std::unique_ptr<Impl> impl_;
};

/* Collects every StyleSheet subtree in the document. False with diagnostics on
   a malformed selector or a derived component used as a declaration. Values
   may reference theme variables as var(--token) or var(--token, fallback), and
   <Variable token="..." value="..."/> nodes under any StyleSheet define them;
   the last definition in document order wins. */
bool buildStyles(dom::Document& document, StyleTable& out,
                 std::vector<dom::Diagnostic>& diagnostics);

/* Reads one stylesheet file for loadStylesheets. */
using StylesheetReader = bool (*)(void* context, std::string_view path, std::string& out_text);

/* Expands <StyleSheet src="..."> references: each file parses as a document
   whose StyleSheet subtrees are imported right after the reference, so rules
   the reference's document declares after it win the order tie-break. False
   with diagnostics when a file cannot be read or parsed. */
[[nodiscard]] bool loadStylesheets(dom::Document& document, void* context,
                                   StylesheetReader reader,
                                   std::vector<dom::Diagnostic>& diagnostics);

/* True for the non-visual StyleSheet/StyleRule subtree; instantiation skips it. */
[[nodiscard]] bool isStyleNode(const dom::Document& document, dom::NodeHandle node);

} // namespace resonate::ui

#endif /* RESONATE_UI_STYLE_H */
