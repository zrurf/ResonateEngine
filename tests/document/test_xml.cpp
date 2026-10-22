#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

#include <resonate/dom/document.h>

namespace
{

using resonate::dom::Document;
using resonate::dom::NodeHandle;

const char* const kDocument = R"xml(<?xml version="1.0" encoding="utf-8"?>
<document version="1" schemas="resonate.render, resonate.ui">
  <!-- A small mixed tree: 3D nodes and a UI panel. -->
  <resources>
    <asset id="checker" guid="0192a0c4-0000-7000-8000-000000000000"/>
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

} // namespace

TEST_CASE("document xml: parses the explicit and short component forms", "[document][xml]")
{
    std::vector<resonate::dom::Diagnostic> diagnostics;
    auto document = Document::parse(kDocument, diagnostics);
    REQUIRE(document != nullptr);
    CHECK(diagnostics.empty());
    CHECK(document->formatVersion() == 1U);
    CHECK(document->schemas().size() == 2U);
    CHECK(document->resources().size() == 1U);

    const NodeHandle root = document->root();
    REQUIRE(root.valid());
    CHECK(document->key(root) == document->intern("root"));
    CHECK(document->name(root) == document->intern("Root & World"));
    const auto classes = document->classes(root);
    REQUIRE(classes.size() == 2U);
    const auto has_class = [&classes, &document](const char* name)
    {
        const auto id = document->intern(name);
        return std::find(classes.begin(), classes.end(), id) != classes.end();
    };
    CHECK(has_class("root"));
    CHECK(has_class("scene"));

    const NodeHandle camera = document->firstChild(root);
    REQUIRE(camera.valid());
    CHECK(document->hasComponent(camera, document->intern("LocalTransform")));
    CHECK(document->hasComponent(camera, document->intern("resonate.render.Camera")));
    const auto* transform = document->component(camera, document->intern("LocalTransform"));
    REQUIRE(transform != nullptr);
    REQUIRE(transform->fields.size() == 1U);
    CHECK(transform->fields[0].value == "0 2.5 -7");

    const NodeHandle panel = document->findKey(document->intern("panel"));
    REQUIRE(panel.valid());
    CHECK(document->id(panel) == document->intern("hud-panel"));
    CHECK(document->type(panel) == document->intern("ui.Panel"));
    CHECK(document->hasComponent(panel, document->intern("Background")));
    const NodeHandle title = document->firstChild(panel);
    REQUIRE(title.valid());
    const auto* background = document->component(panel, document->intern("Background"));
    REQUIRE(background != nullptr);
    CHECK(background->fields[0].value == "0.06 0.07 0.10 0.92");

    const NodeHandle via_id = document->findId(document->intern("hud-panel"));
    CHECK(via_id == panel);
}

TEST_CASE("document xml: the canonical writer round-trips", "[document][xml]")
{
    std::vector<resonate::dom::Diagnostic> diagnostics;
    auto document = Document::parse(kDocument, diagnostics);
    REQUIRE(document != nullptr);

    const std::string first = document->toXml();
    CHECK(first.find("<component type=\"resonate.render.Camera\" fov_y_degrees=\"60\"/>") !=
          std::string::npos);

    diagnostics.clear();
    auto reparsed = Document::parse(first, diagnostics);
    REQUIRE(reparsed != nullptr);
    CHECK(diagnostics.empty());
    CHECK(reparsed->toXml() == first);
}

TEST_CASE("document xml: malformed documents are rejected with a diagnostic", "[document][xml]")
{
    const char* const invalid[] = {
        /* DTD / declarations */
        "<document version=\"1\"><!DOCTYPE x><node/></document>",
        /* text content */
        "<document version=\"1\"><node>hello</node></document>",
        /* duplicate key */
        "<document version=\"1\"><node key=\"a\"/><node key=\"a\"/></document>",
        /* duplicate id on one node */
        "<document version=\"1\"><node id=\"a\" id=\"a\"/></document>",
        /* duplicate component */
        "<document version=\"1\"><node><Background/><component type=\"Background\"/></node></document>",
        /* unknown node attribute */
        "<document version=\"1\"><node foo=\"1\"/></document>",
        /* two roots */
        "<document version=\"1\"><node/><node/></document>",
        /* namespace */
        "<document version=\"1\"><node xmlns=\"x\"/></document>",
        /* unknown entity */
        "<document version=\"1\"><node id=\"&bogus;\"/></document>",
        /* empty key */
        "<document version=\"1\"><node key=\"\"/></document>",
        /* nested component values are not part of the text grammar yet */
        "<document version=\"1\"><node><RectTransform><Inner a=\"1\"/></RectTransform></node></document>",
        /* unterminated tag */
        "<document version=\"1\"><node>",
        /* no root */
        "<document version=\"1\"/>",
        /* processing instruction other than the declaration */
        "<document version=\"1\"><?pi x?><node/></document>",
    };

    for (const char* text : invalid)
    {
        std::vector<resonate::dom::Diagnostic> diagnostics;
        auto document = Document::parse(text, diagnostics);
        INFO(text);
        CHECK(document == nullptr);
        CHECK_FALSE(diagnostics.empty());
    }
}
