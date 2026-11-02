#include <catch2/catch_all.hpp>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <resonate/cook/cook.h>
#include <resonate/dom/document.h>

namespace
{

using resonate::Span;
using resonate::cook::Summary;

struct Root
{
    std::filesystem::path path;

    explicit Root(const char* name)
        : path(std::filesystem::temp_directory_path() / ("resonate_cook_" + std::string(name)))
    {
        std::error_code code;
        std::filesystem::remove_all(path, code);
        std::filesystem::create_directories(path, code);
    }

    ~Root()
    {
        std::error_code code;
        std::filesystem::remove_all(path, code);
    }

    Root(const Root&) = delete;
    Root& operator=(const Root&) = delete;
};

void writeText(const std::filesystem::path& path, const std::string& text)
{
    std::error_code code;
    std::filesystem::create_directories(path.parent_path(), code);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

std::string readText(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

const char* const kScene = R"xml(<?xml version="1.0" encoding="utf-8"?>
<document version="1" schemas="resonate.scene">
  <node key="root" name="Root">
    <component type="resonate.hierarchy.LocalTransform" position="0 2.5 -7"/>
    <node key="child"/>
  </node>
</document>)xml";

struct Reports
{
    std::vector<std::string> lines;

    static void sink(void* user_data, const char* message)
    {
        static_cast<Reports*>(user_data)->lines.emplace_back(message != nullptr ? message : "");
    }
};

} // namespace

TEST_CASE("cook: documents become binary, editor sources stay behind", "[cook]")
{
    Root source("source");
    Root out("out");
    writeText(out.path / "stale.txt", "gone after a cook\n");
    writeText(source.path / "scene" / "demo.rscene", kScene);
    writeText(source.path / "scene" / "demo.rscene.meta", "guid = 1\n");
    writeText(source.path / "scene" / "plain.txt", "not a document\n");
    writeText(source.path / "animation" / "bob.rclip",
              "<clip duration=\"1\"><track field=\"position\">"
              "<key t=\"0\" value=\"0 0 0\"/></track></clip>");
    writeText(source.path / "ui" / "strings.rstrings",
              "<strings locale=\"en\"><entry key=\"k\">text</entry></strings>");
    writeText(source.path / "ui" / "theme.rcss", "Panel { }\n");

    Summary summary;
    REQUIRE(resonate::cook::run(source.path.string(), out.path.string(), summary, nullptr,
                                nullptr));
    CHECK(summary.documents == 1U);
    CHECK(summary.clips == 1U);
    CHECK(summary.strings == 1U);
    CHECK(summary.copies == 1U);
    CHECK(summary.skipped == 2U);
    CHECK(summary.failed == 0U);

    const std::string cooked = readText(out.path / "scene" / "demo.rscene");
    REQUIRE(cooked.size() > 8U);
    CHECK(resonate::dom::Document::isBinary(
        Span<const std::byte>(reinterpret_cast<const std::byte*>(cooked.data()),
                              cooked.size())));

    std::vector<resonate::dom::Diagnostic> diagnostics;
    const std::unique_ptr<resonate::dom::Document> document = resonate::dom::Document::parseBinary(
        Span<const std::byte>(reinterpret_cast<const std::byte*>(cooked.data()), cooked.size()),
        diagnostics);
    REQUIRE(document != nullptr);
    CHECK(document->findKey(document->intern("root")).valid());
    CHECK(document->childCount(document->root()) == 1U);

    const std::string strings = readText(out.path / "ui" / "strings.rstrings");
    REQUIRE(strings.size() > 4U);
    CHECK(strings.compare(0U, 4U, "RSTR") == 0);

    const std::string clip = readText(out.path / "animation" / "bob.rclip");
    REQUIRE(clip.size() > 4U);
    CHECK(clip.compare(0U, 4U, "RCLP") == 0);

    /* The output is replaced, not merged: a stale file must not reach a
       package. */
    CHECK_FALSE(std::filesystem::exists(out.path / "stale.txt"));

    /* Editor-only and non-runtime files stay behind. */
    CHECK_FALSE(std::filesystem::exists(out.path / "scene" / "demo.rscene.meta"));
    CHECK_FALSE(std::filesystem::exists(out.path / "scene" / "plain.txt"));
}

TEST_CASE("cook: extensions compare case-insensitively", "[cook]")
{
    Root source("case-source");
    Root out("case-out");
    writeText(source.path / "art" / "Hero.PNG", "not really a png, but a copy\n");
    writeText(source.path / "ui" / "Theme.RCSS", "Panel { }\n");

    Summary summary;
    REQUIRE(resonate::cook::run(source.path.string(), out.path.string(), summary, nullptr,
                                nullptr));
    CHECK(summary.copies == 2U);
    CHECK(summary.skipped == 0U);
    CHECK(std::filesystem::exists(out.path / "art" / "Hero.PNG"));
    CHECK(std::filesystem::exists(out.path / "ui" / "Theme.RCSS"));
}

TEST_CASE("cook: a broken document fails the run and names the file", "[cook]")
{
    Root source("broken-source");
    Root out("broken-out");
    writeText(source.path / "scene" / "broken.rscene", "<document><node></document>");

    Reports reports;
    Summary summary;
    CHECK_FALSE(resonate::cook::run(source.path.string(), out.path.string(), summary, &reports,
                                    &Reports::sink));
    CHECK(summary.failed == 1U);
    REQUIRE_FALSE(reports.lines.empty());
    CHECK(reports.lines.front().find("broken.rscene") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(out.path / "scene" / "broken.rscene"));
}

TEST_CASE("cook: the output tree cannot overlap the source", "[cook]")
{
    Root source("guard");
    Summary summary;
    CHECK_FALSE(resonate::cook::run(source.path.string(), source.path.string(), summary, nullptr,
                                    nullptr));
    CHECK_FALSE(resonate::cook::run(source.path.string(), (source.path / "cooked").string(),
                                    summary, nullptr, nullptr));
}
