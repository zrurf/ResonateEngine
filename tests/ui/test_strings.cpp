#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <vector>

#include <resonate/core/allocator.h>
#include <resonate/dom/document.h>
#include <resonate/ecs/world.h>
#include <resonate/hierarchy/tree.h>
#include <resonate/ui/document.h>
#include <resonate/ui/document_schema.h>
#include <resonate/ui/layout.h>
#include <resonate/ui/string_table.h>

#include "resonate.ui/components.gen.h"

namespace
{

using resonate::ecs::Entity;
using resonate::ecs::World;
using resonate::ui::StringTable;
using resonate::ui::Text;

struct Fixture
{
    World world{resonate::systemAllocator()};

    Fixture()
    {
        REQUIRE(resonate::hierarchy::registerComponents(world));
        REQUIRE(resonate::ui::registerComponents(world));
    }
};

std::string labelText(World& world, Entity label)
{
    const Text* text = world.get<Text>(label);
    REQUIRE(text != nullptr);
    REQUIRE(world.alive(text->content));
    return std::string(static_cast<const char*>(world.blobData(text->content)),
                       world.blobSize(text->content));
}

} // namespace

TEST_CASE("ui strings: a table resolves keys and decodes entities", "[ui][strings]")
{
    const char* const xml = R"xml(<?xml version="1.0" encoding="utf-8"?>
<strings locale="zh-Hans">
  <entry key="greeting">A &amp; B &lt;C&gt;</entry>
  <entry key="empty"/>
</strings>)xml";
    std::string error;
    std::unique_ptr<StringTable> table = StringTable::parse(xml, error);
    REQUIRE(table != nullptr);
    CHECK(error.empty());
    CHECK(table->locale() == "zh-Hans");
    const std::string* greeting = table->find("greeting");
    REQUIRE(greeting != nullptr);
    CHECK(*greeting == "A & B <C>");
    const std::string* empty = table->find("empty");
    REQUIRE(empty != nullptr);
    CHECK(empty->empty());
    CHECK(table->find("missing") == nullptr);
}

TEST_CASE("ui strings: malformed tables name the reason", "[ui][strings]")
{
    const auto fails = [](const char* xml, const char* reason)
    {
        std::string error;
        CHECK(StringTable::parse(xml, error) == nullptr);
        CHECK(error.find(reason) != std::string::npos);
    };
    fails("<strings><entry key=\"a\">1</entry><entry key=\"a\">2</entry></strings>",
          "duplicate key");
    fails("<strings><thing/></strings>", "unknown element");
    fails("<strings locale=\"en\" bad=\"x\"/>", "unknown attribute");
    fails("<strings><entry>text</entry></strings>", "needs a key");
    fails("<strings><entry key=\"a\"><b/></entry></strings>", "nested elements");
    fails("<strings><entry key=\"a\">&bogus;</entry></strings>", "unknown entity");
    fails("<strings><entry key=\"a\">text</strings>", "expected </entry>");
}

TEST_CASE("ui strings: markup keys resolve through the table", "[ui][strings]")
{
    const char* const text = R"xml(<document version="1" schemas="resonate.ui">
  <Screen key="hud">
    <Label key="title" text="#ui.demo.title" width="200" height="30" color="0 0 0 0"/>
  </Screen>
</document>)xml";
    std::vector<resonate::dom::Diagnostic> diagnostics;
    auto document = resonate::dom::Document::parse(text, &resonate::ui::documentSchema(),
                                                   diagnostics);
    REQUIRE(document != nullptr);

    std::string error;
    std::unique_ptr<StringTable> table = StringTable::parse(
        R"xml(<strings locale="zh-Hans"><entry key="ui.demo.title">动作演示</entry></strings>)xml",
        error);
    REQUIRE(table != nullptr);

    Fixture localized;
    const Entity root = resonate::ui::instantiate(localized.world, *document, nullptr, {},
                                                  table.get());
    REQUIRE(root.valid());
    const auto children = resonate::hierarchy::children(localized.world, root);
    REQUIRE(children.size() == 1U);
    CHECK(labelText(localized.world, children[0]) == "动作演示");

    /* Without a table the key stays visible, so the gap is obvious on screen. */
    Fixture plain;
    const Entity fallback = resonate::ui::instantiate(plain.world, *document);
    REQUIRE(fallback.valid());
    const auto fallback_children = resonate::hierarchy::children(plain.world, fallback);
    REQUIRE(fallback_children.size() == 1U);
    CHECK(labelText(plain.world, fallback_children[0]) == "ui.demo.title");
}
