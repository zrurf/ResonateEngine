#include <resonate/cook/cook.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <resonate/animation/clip.h>
#include <resonate/dom/document.h>
#include <resonate/dom/schema.h>
#include <resonate/ui/string_table.h>

#include "resonate.animation/nodes.gen.h"
#include "resonate.gameplay/nodes.gen.h"
#include "resonate.hierarchy/nodes.gen.h"
#include "resonate.render/nodes.gen.h"
#include "resonate.scene/nodes.gen.h"
#include "resonate.ui/nodes.gen.h"

namespace resonate::cook
{
namespace
{

/* The same compositions the runtime parses and instantiates through; a document
   cooked against a different schema vocabulary would not load. */
const dom::SchemaRegistry& contentSchema()
{
    static const dom::TableSchemaRegistry ui_table(
        Span<const dom::NodeTypeDesc>(ui::generated::kNodeTypes, ui::generated::kNodeTypeCount),
        Span<const char* const>(ui::generated::kComponents, ui::generated::kComponentCount));
    static const dom::TableSchemaRegistry hierarchy_table(
        Span<const dom::NodeTypeDesc>(hierarchy::generated::kNodeTypes,
                                      hierarchy::generated::kNodeTypeCount),
        Span<const char* const>(hierarchy::generated::kComponents,
                                hierarchy::generated::kComponentCount));
    static const dom::TableSchemaRegistry render_table(
        Span<const dom::NodeTypeDesc>(render::generated::kNodeTypes,
                                      render::generated::kNodeTypeCount),
        Span<const char* const>(render::generated::kComponents,
                                render::generated::kComponentCount));
    static const dom::TableSchemaRegistry gameplay_table(
        Span<const dom::NodeTypeDesc>(gameplay::generated::kNodeTypes,
                                      gameplay::generated::kNodeTypeCount),
        Span<const char* const>(gameplay::generated::kComponents,
                                gameplay::generated::kComponentCount));
    static const dom::TableSchemaRegistry animation_table(
        Span<const dom::NodeTypeDesc>(animation::generated::kNodeTypes,
                                      animation::generated::kNodeTypeCount),
        Span<const char* const>(animation::generated::kComponents,
                                animation::generated::kComponentCount));
    static const dom::TableSchemaRegistry scene_table(
        Span<const dom::NodeTypeDesc>(scene::generated::kNodeTypes,
                                      scene::generated::kNodeTypeCount),
        Span<const char* const>(scene::generated::kComponents,
                                scene::generated::kComponentCount));
    static const std::array<const dom::SchemaRegistry*, 6> tables{
        &ui_table, &hierarchy_table, &render_table,
        &gameplay_table, &animation_table, &scene_table};
    static const dom::CompositeSchemaRegistry registry(
        Span<const dom::SchemaRegistry* const>(tables.data(), tables.size()));
    return registry;
}

void say(void* user_data, ReportFn report, const std::string& message)
{
    if (report != nullptr)
    {
        report(user_data, message.c_str());
        return;
    }
    std::fprintf(stderr, "cook: %s\n", message.c_str());
}

[[nodiscard]] bool readFile(const std::filesystem::path& path, std::string& out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open())
    {
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

[[nodiscard]] bool writeFile(const std::filesystem::path& path, const void* data, std::size_t size)
{
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error)
    {
        return false;
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open())
    {
        return false;
    }
    if (size != 0U)
    {
        out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    }
    return out.good();
}

[[nodiscard]] bool isDocument(const std::filesystem::path& path)
{
    const std::string extension = path.extension().string();
    return extension == ".rscene" || extension == ".rui";
}

[[nodiscard]] bool isClip(const std::filesystem::path& path)
{
    return path.extension() == ".rclip";
}

[[nodiscard]] bool isStrings(const std::filesystem::path& path)
{
    return path.extension() == ".rstrings";
}

/* Editor-only sources: identity is an editor concept and declarations are the
   compiler's input, neither is read by a run. */
[[nodiscard]] bool isEditorOnly(const std::filesystem::path& path)
{
    const std::string extension = path.extension().string();
    return extension == ".meta" || extension == ".rschema" ||
           path.filename().string() == "schemas.lock.json";
}

/* What a run may read beside the cooked documents: fonts, images, the icon map
   and the stylesheet. Sources that only tools read (SVG icons, PPM textures,
   C# sources) are not shipped; a new runtime asset type has to be added here,
   which is the point -- the shipped tree is a list, not a sweep. */
[[nodiscard]] bool isRuntimeAsset(const std::filesystem::path& path)
{
    static const char* const kExtensions[] = {".ttf", ".otf", ".png", ".jpg",
                                              ".jpeg", ".ktx2", ".icons", ".rcss"};
    const std::string extension = path.extension().string();
    return std::any_of(std::begin(kExtensions), std::end(kExtensions),
                       [&extension](const char* candidate) { return extension == candidate; });
}

/* Parses the source, writes the cooked form and loads it back. The reload is
   what makes "the bytes this run ships" a checked statement instead of a hope:
   a codec change that loses a node fails here, not on a player's load. */
[[nodiscard]] bool cookDocument(const std::filesystem::path& source,
                                const std::filesystem::path& target, void* user_data,
                                ReportFn report)
{
    std::string text;
    if (!readFile(source, text))
    {
        say(user_data, report, source.string() + ": cannot read the document");
        return false;
    }
    std::vector<dom::Diagnostic> diagnostics;
    std::unique_ptr<dom::Document> document =
        dom::Document::parse(text, &contentSchema(), diagnostics);
    if (document == nullptr)
    {
        for (const dom::Diagnostic& diagnostic : diagnostics)
        {
            say(user_data, report,
                source.string() + ":" + std::to_string(diagnostic.line) + ":" +
                    std::to_string(diagnostic.column) + ": " + diagnostic.message);
        }
        return false;
    }

    const std::vector<std::byte> cooked = document->toBinary();
    std::vector<dom::Diagnostic> reload_diagnostics;
    const std::unique_ptr<dom::Document> reloaded =
        dom::Document::parseBinary(Span<const std::byte>(cooked.data(), cooked.size()),
                                   reload_diagnostics);
    if (reloaded == nullptr || reloaded->nodes().size() != document->nodes().size() ||
        reloaded->toXml() != document->toXml())
    {
        say(user_data, report,
            source.string() + ": the cooked form does not round trip the authored document");
        return false;
    }
    if (!writeFile(target, cooked.data(), cooked.size()))
    {
        say(user_data, report, target.string() + ": cannot write the cooked document");
        return false;
    }
    return true;
}

[[nodiscard]] bool copyFile(const std::filesystem::path& source,
                            const std::filesystem::path& target, void* user_data, ReportFn report)
{
    std::error_code error;
    std::filesystem::create_directories(target.parent_path(), error);
    if (error)
    {
        say(user_data, report, target.string() + ": cannot create its directory");
        return false;
    }
    std::filesystem::copy_file(source, target, std::filesystem::copy_options::overwrite_existing,
                               error);
    if (error)
    {
        say(user_data, report, source.string() + ": cannot copy it (" + error.message() + ")");
        return false;
    }
    return true;
}

/* Clips are plain keyframe data once parsed, so the cooked form is their track
   table. The reload check compares the sampled data, not the bytes: the writer
   is what produces both forms, and what has to match is the animation. */
[[nodiscard]] bool cookClip(const std::filesystem::path& source,
                            const std::filesystem::path& target, void* user_data, ReportFn report)
{
    std::string text;
    if (!readFile(source, text))
    {
        say(user_data, report, source.string() + ": cannot read the clip");
        return false;
    }
    animation::Clip clip;
    std::string error;
    if (!animation::Clip::parse(text, clip, error))
    {
        say(user_data, report, source.string() + ": " + error);
        return false;
    }
    const std::vector<std::byte> cooked = clip.toBinary();
    animation::Clip reloaded;
    if (!animation::Clip::parseBinary(Span<const std::byte>(cooked.data(), cooked.size()),
                                      reloaded, error) ||
        reloaded.duration != clip.duration ||
        reloaded.position_root_motion != clip.position_root_motion ||
        reloaded.position.keys.size() != clip.position.keys.size() ||
        reloaded.rotation.keys.size() != clip.rotation.keys.size() ||
        reloaded.scale.keys.size() != clip.scale.keys.size())
    {
        say(user_data, report, source.string() + ": the cooked clip does not round trip");
        return false;
    }
    if (!writeFile(target, cooked.data(), cooked.size()))
    {
        say(user_data, report, target.string() + ": cannot write the cooked clip");
        return false;
    }
    return true;
}

/* The string table is its own XML profile, so it has its own cooked codec; the
   reload checks the locale and every key rather than trusting the writer. */
[[nodiscard]] bool cookStrings(const std::filesystem::path& source,
                               const std::filesystem::path& target, void* user_data,
                               ReportFn report)
{
    std::string text;
    if (!readFile(source, text))
    {
        say(user_data, report, source.string() + ": cannot read the string table");
        return false;
    }
    std::string error;
    const std::unique_ptr<ui::StringTable> table = ui::StringTable::parse(text, error);
    if (table == nullptr)
    {
        say(user_data, report, source.string() + ": " + error);
        return false;
    }
    const std::vector<std::byte> cooked = table->toBinary();
    const std::unique_ptr<ui::StringTable> reloaded =
        ui::StringTable::parseBytes(Span<const std::byte>(cooked.data(), cooked.size()), error);
    if (reloaded == nullptr || reloaded->locale() != table->locale())
    {
        say(user_data, report, source.string() + ": the cooked string table does not round trip");
        return false;
    }
    if (!writeFile(target, cooked.data(), cooked.size()))
    {
        say(user_data, report, target.string() + ": cannot write the cooked string table");
        return false;
    }
    return true;
}

} // namespace

bool run(const std::string& source, const std::string& out, Summary& summary, void* user_data,
         ReportFn report)
{
    std::error_code error;
    const std::filesystem::path source_root(source);
    if (!std::filesystem::is_directory(source_root, error) || error)
    {
        say(user_data, report, source + ": the content directory does not exist");
        return false;
    }
    const std::filesystem::path out_root(out);
    const std::filesystem::path source_abs = std::filesystem::weakly_canonical(source_root, error);
    error.clear();
    const std::filesystem::path out_abs = std::filesystem::weakly_canonical(out_root, error);
    error.clear();
    const auto inside = [](const std::filesystem::path& path, const std::filesystem::path& prefix)
    {
        auto part = path.begin();
        for (auto wanted = prefix.begin(); wanted != prefix.end(); ++wanted, ++part)
        {
            if (part == path.end() || *part != *wanted)
            {
                return false;
            }
        }
        return true;
    };
    if (out.empty() || out_abs == source_abs || inside(source_abs, out_abs) ||
        inside(out_abs, source_abs))
    {
        say(user_data, report, out + ": the output tree must be its own directory");
        return false;
    }
    /* The output is a build artifact and is replaced wholesale: a file that is
       no longer an asset must not survive into a package because it sat in the
       previous tree. */
    std::filesystem::remove_all(out_root, error);
    error.clear();
    std::filesystem::create_directories(out_root, error);
    if (error)
    {
        say(user_data, report, out + ": cannot create the output directory");
        return false;
    }

    bool ok = true;
    auto iterator = std::filesystem::recursive_directory_iterator(
        source_root, std::filesystem::directory_options::skip_permission_denied, error);
    if (error)
    {
        say(user_data, report, source + ": cannot walk the content directory");
        return false;
    }
    for (const std::filesystem::directory_entry& entry : iterator)
    {
        if (!entry.is_regular_file(error) || error)
        {
            continue;
        }
        const std::filesystem::path& path = entry.path();
        if (isEditorOnly(path))
        {
            ++summary.skipped;
            continue;
        }
        std::error_code relative_error;
        const std::filesystem::path relative =
            std::filesystem::relative(path, source_root, relative_error);
        if (relative_error)
        {
            say(user_data, report, path.string() + ": cannot map it into the output tree");
            ok = false;
            ++summary.failed;
            continue;
        }
        const std::filesystem::path target = out_root / relative;
        if (isDocument(path))
        {
            if (cookDocument(path, target, user_data, report))
            {
                ++summary.documents;
            }
            else
            {
                ok = false;
                ++summary.failed;
            }
            continue;
        }
        if (isClip(path))
        {
            if (cookClip(path, target, user_data, report))
            {
                ++summary.clips;
            }
            else
            {
                ok = false;
                ++summary.failed;
            }
            continue;
        }
        if (isStrings(path))
        {
            if (cookStrings(path, target, user_data, report))
            {
                ++summary.strings;
            }
            else
            {
                ok = false;
                ++summary.failed;
            }
            continue;
        }
        if (!isRuntimeAsset(path))
        {
            ++summary.skipped;
            continue;
        }
        if (copyFile(path, target, user_data, report))
        {
            ++summary.copies;
        }
        else
        {
            ok = false;
            ++summary.failed;
        }
    }
    return ok;
}

} // namespace resonate::cook
