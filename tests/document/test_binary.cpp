#include <catch2/catch_all.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <resonate/dom/document.h>

namespace
{

using resonate::Span;
using resonate::dom::Document;
using resonate::dom::Diagnostic;
using resonate::dom::Field;
using resonate::dom::NodeHandle;

const char* const kDocument = R"xml(<?xml version="1.0" encoding="utf-8"?>
<document version="3" schemas="resonate.scene, resonate.render">
  <!-- A tree with every kind of record the model holds. -->
  <resources>
    <asset id="checker" guid="0192a0c4-0000-7000-8000-000000000000"/>
    <asset id="bob" guid="0192a0c4-0000-7000-8000-000000000001"/>
  </resources>
  <node key="root" name="Root &amp; World" class="scene root">
    <node key="camera" name="Camera">
      <LocalTransform position="0 2.5 -7"/>
      <component type="resonate.render.Camera" fov_y_degrees="60"/>
    </node>
    <node key="panel" id="hud-panel" name="Panel" type="ui.Panel" class="panel primary">
      <Background color="0.06 0.07 0.10 0.92"/>
      <node key="title" type="ui.Label">
        <RectTransform height="28"/>
      </node>
    </node>
  </node>
</document>)xml";


std::unique_ptr<Document> parseSource()
{
    std::vector<Diagnostic> diagnostics;
    std::unique_ptr<Document> document = Document::parse(kDocument, diagnostics);
    REQUIRE(document != nullptr);
    CHECK(diagnostics.empty());
    return document;
}

} // namespace

TEST_CASE("document binary: the cooked form round trips the model", "[document][binary]")
{
    std::unique_ptr<Document> source = parseSource();
    const NodeHandle root = source->findKey(source->intern("root"));
    REQUIRE(root.valid());
    const std::uint32_t selector = source->intern("selector");
    REQUIRE(source->setProperty(root, Field{selector, "panel > title"}));

    /* A detached node is part of the document too; the codec must carry it. */
    const NodeHandle detached =
        source->create(source->intern("resonate.scene.Detached"));
    REQUIRE(detached.valid());

    const std::vector<std::byte> cooked = source->toBinary();
    REQUIRE(cooked.size() > 16U);
    REQUIRE(Document::isBinary(Span<const std::byte>(cooked.data(), cooked.size())));

    std::vector<Diagnostic> diagnostics;
    std::unique_ptr<Document> restored =
        Document::parseBinary(Span<const std::byte>(cooked.data(), cooked.size()), diagnostics);
    REQUIRE(restored != nullptr);
    CHECK(diagnostics.empty());

    CHECK(restored->formatVersion() == 3U);
    CHECK(restored->toXml() == source->toXml());
    CHECK(restored->nodes().size() == source->nodes().size());
    CHECK(restored->root() == restored->findKey(restored->intern("root")));

    const Field* property = restored->property(restored->root(), restored->intern("selector"));
    REQUIRE(property != nullptr);
    CHECK(property->value == "panel > title");

    /* The same content has the same bytes, so a cook result is cacheable. */
    const std::vector<std::byte> again = restored->toBinary();
    CHECK(again == cooked);
}

TEST_CASE("document binary: damage is refused, not guessed at", "[document][binary]")
{
    std::unique_ptr<Document> source = parseSource();
    const std::vector<std::byte> cooked = source->toBinary();
    std::vector<Diagnostic> diagnostics;

    SECTION("the magic identifies the codec")
    {
        CHECK_FALSE(Document::isBinary(Span<const std::byte>(cooked.data(), 3U)));
        std::vector<std::byte> not_cooked = cooked;
        not_cooked[0] = std::byte{'R'};
        not_cooked[1] = std::byte{'D'};
        not_cooked[2] = std::byte{'X'};
        CHECK_FALSE(Document::isBinary(
            Span<const std::byte>(not_cooked.data(), not_cooked.size())));
        CHECK(Document::parseBinary(
                  Span<const std::byte>(not_cooked.data(), not_cooked.size()), diagnostics) ==
              nullptr);
        CHECK_FALSE(diagnostics.empty());
    }
    SECTION("a truncated file is refused")
    {
        for (const std::size_t size : {std::size_t{0}, std::size_t{4}, std::size_t{20},
                                       cooked.size() / 2U, cooked.size() - 1U})
        {
            diagnostics.clear();
            CHECK(Document::parseBinary(Span<const std::byte>(cooked.data(), size),
                                        diagnostics) == nullptr);
            CHECK_FALSE(diagnostics.empty());
        }
    }
    SECTION("trailing bytes are refused")
    {
        std::vector<std::byte> extended = cooked;
        extended.push_back(std::byte{0});
        CHECK(Document::parseBinary(
                  Span<const std::byte>(extended.data(), extended.size()), diagnostics) ==
              nullptr);
    }
    SECTION("a newer codec is refused")
    {
        std::vector<std::byte> future = cooked;
        future[4] = std::byte{99};
        CHECK(Document::parseBinary(Span<const std::byte>(future.data(), future.size()),
                                    diagnostics) == nullptr);
        CHECK(diagnostics.back().message.find("codec") != std::string::npos);
    }
}
