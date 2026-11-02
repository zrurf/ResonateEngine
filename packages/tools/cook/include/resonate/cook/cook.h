#ifndef RESONATE_COOK_COOK_H
#define RESONATE_COOK_COOK_H

/*
 * The cook step: turns a source content tree into the tree a packaged build
 * ships. Documents are parsed with the same schema compositions the runtime
 * instantiates them through, written in the binary codec and verified by
 * loading the bytes back before they are written; clips are parsed and written
 * in the cooked clip codec the same way. Runtime assets the codecs do not own
 * (fonts, images, the icon map, stylesheets) are copied unchanged; textures are
 * cooked by their own tool. Editor-only and tool-only sources (.meta sidecars,
 * .rschema declarations, icon SVG sources, C# sources) are not shipped.
 *
 * The output mirrors the source's relative layout, so a package that copies it
 * to `<executable>/content` is found by the runtime's usual content walk.
 * Product manifests stage separately: they describe where the cooked files
 * live, not what they contain. The output tree is replaced wholesale on every
 * run, so a file that is no longer an asset cannot reach a package.
 */

#include <cstdint>
#include <string>

namespace resonate::cook
{

struct Summary
{
    std::uint32_t documents = 0U; /* parsed and written in the binary codec */
    std::uint32_t clips = 0U;     /* parsed and written in the cooked clip codec */
    std::uint32_t strings = 0U;   /* parsed and written in the cooked table codec */
    std::uint32_t copies = 0U;    /* carried over unchanged */
    std::uint32_t skipped = 0U;   /* editor-only or not a file the runtime reads */
    std::uint32_t failed = 0U;    /* documents that did not parse or verify */
};

using ReportFn = void (*)(void* user_data, const char* message);

/* Cooks every file under `source` into `out`. Returns false when any document
   failed; `report` receives one line per failure (and nothing on success). A
   missing or unreadable output location also fails the run. */
[[nodiscard]] bool run(const std::string& source, const std::string& out, Summary& summary,
                       void* user_data, ReportFn report);

} // namespace resonate::cook

#endif /* RESONATE_COOK_COOK_H */
