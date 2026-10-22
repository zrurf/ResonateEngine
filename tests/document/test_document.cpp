#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <resonate/dom/document.h>

namespace
{

using resonate::dom::Component;
using resonate::dom::Document;
using resonate::dom::Field;
using resonate::dom::NodeHandle;
using resonate::dom::StringId;

Component makeComponent(Document& document, const char* type,
                        std::initializer_list<std::pair<const char*, std::string>> fields)
{
    Component component;
    component.type = document.intern(type);
    for (const auto& [name, value] : fields)
    {
        component.fields.push_back(Field{document.intern(name), value});
    }
    return component;
}

} // namespace

TEST_CASE("document: handles survive moves and die with the node", "[document]")
{
    Document document;
    const auto group = document.intern("Group");
    const NodeHandle root = document.create(group);
    REQUIRE(document.setRoot(root));
    const NodeHandle first = document.create();
    const NodeHandle second = document.create();
    REQUIRE(document.alive(first));
    REQUIRE(document.append(root, first));
    REQUIRE(document.append(root, second));

    CHECK(document.parent(first) == root);
    CHECK(document.parent(second) == root);
    CHECK(document.firstChild(root) == first);
    CHECK(document.lastChild(root) == second);
    CHECK(document.nextSibling(first) == second);
    CHECK(document.previousSibling(second) == first);
    CHECK(document.childCount(root) == 2U);

    /* Moving a node keeps its handle; the tree links change around it. */
    REQUIRE(document.insertBefore(root, second, first));
    CHECK(document.firstChild(root) == second);
    CHECK(document.lastChild(root) == first);
    CHECK(document.nextSibling(second) == first);
    CHECK(document.previousSibling(first) == second);
    CHECK(document.alive(second));

    REQUIRE(document.detach(second));
    CHECK(document.alive(second));
    CHECK_FALSE(document.parent(second).valid());
    CHECK(document.childCount(root) == 1U);
    REQUIRE(document.append(root, second));

    const std::uint64_t revision = document.revision();
    REQUIRE(document.destroy(first));
    CHECK_FALSE(document.alive(first));
    CHECK(document.alive(second));
    CHECK(document.revision() > revision);
}

TEST_CASE("document: a node cannot be folded into its own subtree", "[document]")
{
    Document document;
    const NodeHandle root = document.create();
    REQUIRE(document.setRoot(root));
    const NodeHandle child = document.create();
    const NodeHandle grandchild = document.create();
    REQUIRE(document.append(root, child));
    REQUIRE(document.append(child, grandchild));

    CHECK_FALSE(document.append(grandchild, root));
    CHECK_FALSE(document.append(child, child));
    CHECK_FALSE(document.detach(root));
    CHECK_FALSE(document.append(root, root));
}

TEST_CASE("document: key and id are separate unique handles", "[document]")
{
    Document document;
    const NodeHandle root = document.create(document.intern("Group"));
    REQUIRE(document.setRoot(root));
    const NodeHandle other = document.create(document.intern("Child"));
    REQUIRE(document.append(root, other));

    const StringId key = document.intern("panel");
    const StringId id = document.intern("hud-panel");
    REQUIRE(document.setKey(root, key));
    REQUIRE(document.setId(root, id));
    CHECK(document.findKey(key) == root);
    CHECK(document.findId(id) == root);

    CHECK_FALSE(document.setKey(other, key));
    CHECK_FALSE(document.setId(other, id));
    CHECK_FALSE(document.findKey(document.intern("missing")).valid());

    /* A detached node keeps its lookup entries; the tree walk does not. */
    REQUIRE(document.detach(other));
    REQUIRE(document.setKey(other, document.intern("detached")));
    CHECK(document.findKey(document.intern("detached")) == other);
    CHECK(document.findType(document.intern("Child")).empty());
}

TEST_CASE("document: classes and components", "[document]")
{
    Document document;
    const NodeHandle root = document.create();
    REQUIRE(document.setRoot(root));

    const StringId alpha = document.intern("alpha");
    const StringId beta = document.intern("beta");
    const std::array<StringId, 3> classes{beta, alpha, beta};
    REQUIRE(document.setClasses(root, resonate::Span<const StringId>(classes.data(), 3U)));
    REQUIRE(document.classes(root).size() == 2U);
    CHECK(document.classes(root)[0] == alpha);
    CHECK(document.classes(root)[1] == beta);
    CHECK(document.findClass(beta).size() == 1U);

    const Component transform = makeComponent(document, "RectTransform", {{"x", "40"}, {"y", "20"}});
    REQUIRE(document.setComponent(root, transform));
    REQUIRE(document.hasComponent(root, transform.type));
    const Component* found = document.component(root, transform.type);
    REQUIRE(found != nullptr);
    REQUIRE(found->fields.size() == 2U);
    CHECK(found->fields[0].value == "40");

    Component duplicate = makeComponent(document, "RectTransform", {{"x", "1"}, {"x", "2"}});
    CHECK_FALSE(document.setComponent(root, duplicate));
    CHECK(document.removeComponent(root, transform.type));
    CHECK_FALSE(document.hasComponent(root, transform.type));
}

TEST_CASE("document: clone copies the subtree and chooses its identity", "[document]")
{
    Document document;
    const NodeHandle root = document.create(document.intern("Panel"));
    REQUIRE(document.setRoot(root));
    REQUIRE(document.setName(root, document.intern("Panel")));
    REQUIRE(document.setKey(root, document.intern("panel")));
    const StringId cls = document.intern("primary");
    REQUIRE(document.setClasses(root, resonate::Span<const StringId>(&cls, 1U)));
    REQUIRE(document.setComponent(root, makeComponent(document, "Background", {{"color", "1 1 1 1"}})));
    const NodeHandle child = document.create(document.intern("Label"));
    REQUIRE(document.append(root, child));

    const NodeHandle copy = document.clone(root);
    REQUIRE(copy.valid());
    CHECK(copy != root);
    CHECK(document.type(copy) == document.type(root));
    CHECK(document.name(copy) == document.name(root));
    CHECK(document.key(copy) == resonate::dom::kInvalidString);
    CHECK(document.classes(copy).size() == 1U);
    CHECK(document.hasComponent(copy, document.intern("Background")));
    CHECK(document.childCount(copy) == 1U);
    CHECK(document.firstChild(copy) != child);

    resonate::dom::CloneOptions bare;
    bare.copy_name = false;
    bare.copy_classes = false;
    const NodeHandle stripped = document.clone(root, bare);
    REQUIRE(stripped.valid());
    CHECK(document.name(stripped) == resonate::dom::kInvalidString);
    CHECK(document.classes(stripped).empty());
}
