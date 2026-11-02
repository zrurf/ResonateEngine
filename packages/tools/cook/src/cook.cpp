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
#include <unordered_map>
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
    out.close();
    return out.good();
}

/* Extensions compare case-insensitively: the host filesystem does not
   distinguish Hero.PNG from hero.png, and a miss here silently drops the
   asset from the shipped tree. */
[[nodiscard]] std::string loweredExtension(const std::filesystem::path& path)
{
    std::string extension = path.extension().string();
    for (char& character : extension)
    {
        if (character >= 'A' && character <= 'Z')
        {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return extension;
}

[[nodiscard]] bool isDocument(const std::filesystem::path& path)
{
    const std::string extension = loweredExtension(path);
    return extension == ".rscene" || extension == ".rui";
}

[[nodiscard]] bool isClip(const std::filesystem::path& path)
{
    return loweredExtension(path) == ".rclip";
}

[[nodiscard]] bool isStrings(const std::filesystem::path& path)
{
    return loweredExtension(path) == ".rstrings";
}

/* Editor-only sources: identity is an editor concept and declarations are the
   compiler's input, neither is read by a run. */
[[nodiscard]] bool isEditorOnly(const std::filesystem::path& path)
{
    const std::string extension = loweredExtension(path);
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
    const std::string extension = loweredExtension(path);
    return std::any_of(std::begin(kExtensions), std::end(kExtensions),
                       [&extension](const char* candidate) { return extension == candidate; });
}

/* Everything the document codec carries, per node in slot order: identity,
   classes, components, properties and the child list by slot. toXml covers
   the root subtree; this also covers detached nodes, whose content toXml
   never reaches. */
[[nodiscard]] std::string documentSignature(const dom::Document& document)
{
    const std::vector<dom::NodeHandle> nodes = document.nodes();
    std::unordered_map<std::uint32_t, std::size_t> slots;
    slots.reserve(nodes.size());
    for (std::size_t position = 0; position < nodes.size(); ++position)
    {
        slots.emplace(nodes[position].index, position);
    }
    std::string signature;
    const auto append = [&signature](std::string_view text)
    {
        signature += std::to_string(text.size());
        signature += ':';
        signature.append(text);
    };
    for (const dom::NodeHandle node : nodes)
    {
        append(document.text(document.type(node)));
        append(document.text(document.key(node)));
        append(document.text(document.id(node)));
        append(document.text(document.name(node)));
        for (const dom::StringId class_id : document.classes(node))
        {
            append(document.text(class_id));
        }
        for (const dom::Component& component : document.components(node))
        {
            append(document.text(component.type));
            for (const dom::Field& field : component.fields)
            {
                append(document.text(field.name));
                append(field.value);
            }
        }
        for (const dom::Field& property : document.properties(node))
        {
            append(document.text(property.name));
            append(property.value);
        }
        append(std::to_string(document.childCount(node)));
        for (dom::NodeHandle child = document.firstChild(node); child.valid();
             child = document.nextSibling(child))
        {
            append(std::to_string(slots.at(child.index)));
        }
    }
    return signature;
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
        reloaded->toXml() != document->toXml() ||
        documentSignature(*reloaded) != documentSignature(*document))
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
    const auto sameVec3 = [](const animation::Vec3Track& authored,
                             const animation::Vec3Track& read)
    {
        if (authored.keys.size() != read.keys.size())
        {
            return false;
        }
        for (std::size_t index = 0; index < authored.keys.size(); ++index)
        {
            const animation::Vec3Track::Key& written = authored.keys[index];
            const animation::Vec3Track::Key& back = read.keys[index];
            if (written.time != back.time || written.value.x != back.value.x ||
                written.value.y != back.value.y || written.value.z != back.value.z)
            {
                return false;
            }
        }
        return true;
    };
    const auto sameQuat = [](const animation::QuatTrack& authored,
                             const animation::QuatTrack& read)
    {
        if (authored.keys.size() != read.keys.size())
        {
            return false;
        }
        for (std::size_t index = 0; index < authored.keys.size(); ++index)
        {
            const animation::QuatTrack::Key& written = authored.keys[index];
            const animation::QuatTrack::Key& back = read.keys[index];
            if (written.time != back.time || written.value.x != back.value.x ||
                written.value.y != back.value.y || written.value.z != back.value.z ||
                written.value.w != back.value.w)
            {
                return false;
            }
        }
        return true;
    };
    if (!animation::Clip::parseBinary(Span<const std::byte>(cooked.data(), cooked.size()),
                                      reloaded, error) ||
        reloaded.duration != clip.duration ||
        reloaded.position_root_motion != clip.position_root_motion ||
        !sameVec3(clip.position, reloaded.position) ||
        !sameQuat(clip.rotation, reloaded.rotation) || !sameVec3(clip.scale, reloaded.scale))
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
    if (reloaded == nullptr || reloaded->locale() != table->locale() ||
        reloaded->entries() != table->entries())
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
    /* The replacement is recursive: an output around the working directory (a
       build variable left unexpanded, a stray root) must not pass. */
    std::error_code cwd_error;
    const std::filesystem::path cwd = std::filesystem::weakly_canonical(
        std::filesystem::current_path(cwd_error), cwd_error);
    if (cwd_error || inside(cwd, out_abs))
    {
        say(user_data, report, out + ": the output tree must not contain the working directory");
        return false;
    }
    /* The output is a build artifact and is replaced wholesale: a file that is
       no longer an asset must not survive into a package because it sat in the
       previous tree. A removal that could not finish (a file a running game
       still locks) has to fail the cook, or the stale file ships. */
    std::filesystem::remove_all(out_root, error);
    if (error)
    {
        say(user_data, report, out + ": cannot replace the output tree (" + error.message() + ")");
        return false;
    }
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
