#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <vector>

#include <resonate/dom/abi.h>

extern "C" int resonate_dom_c_abi_check(void);

namespace
{

struct Diagnostics
{
    int errors = 0;
    std::string last;

    static void sink(void* user, const ResonateDomDiagnostic* diagnostic)
    {
        auto* self = static_cast<Diagnostics*>(user);
        self->errors += 1;
        self->last = diagnostic->message != nullptr ? diagnostic->message : "";
    }
};

const char* const kDocument = R"xml(<document version="1" schemas="resonate.ui">
  <node key="root" name="Root">
    <node key="child" id="child-id" type="Panel" class="panel primary">
      <component type="RectTransform" x="10" width="100"/>
    </node>
  </node>
</document>)xml";

} // namespace

TEST_CASE("document abi: the C surface compiles and links as C", "[document][abi]")
{
    CHECK(resonate_dom_c_abi_check() == 0);
}

TEST_CASE("document abi: handles, strings, components and queries", "[document][abi]")
{
    ResonateDomDocument* document = resonate_dom_create();
    REQUIRE(document != nullptr);

    Diagnostics diagnostics;
    const std::uint32_t size = static_cast<std::uint32_t>(std::char_traits<char>::length(kDocument));
    REQUIRE(resonate_dom_parse(document, kDocument, size, &Diagnostics::sink, &diagnostics) ==
            RESONATE_OK);
    CHECK(diagnostics.errors == 0);

    const auto key_root = resonate_dom_intern(document, "root", 4);
    const auto key_child = resonate_dom_intern(document, "child", 5);
    const ResonateDomNode root = resonate_dom_root(document);
    REQUIRE(root.value != 0U);
    CHECK(resonate_dom_alive(document, root) == 1);
    CHECK(resonate_dom_key(document, root).id == key_root.id);
    CHECK(resonate_dom_child_count(document, root) == 1U);

    const ResonateDomNode child = resonate_dom_first_child(document, root);
    REQUIRE(child.value != 0U);
    CHECK(resonate_dom_key(document, child).id == key_child.id);
    CHECK(resonate_dom_find_key(document, key_child).value == child.value);

    const auto component = resonate_dom_intern(document, "RectTransform", 13);
    CHECK(resonate_dom_component_count(document, child) == 1U);
    CHECK(resonate_dom_component_type_at(document, child, 0).id == component.id);
    CHECK(resonate_dom_field_count(document, child, component) == 2U);
    std::uint32_t value_size = 0;
    const char* value = resonate_dom_field_value_at(document, child, component, 0, &value_size);
    REQUIRE(value != nullptr);
    CHECK(std::string(value, value_size) == "10");

    const auto width = resonate_dom_intern(document, "width", 5);
    REQUIRE(resonate_dom_set_field(document, child, component, width, "250", 3) == RESONATE_OK);
    value = resonate_dom_field_value_at(document, child, component, 1, &value_size);
    REQUIRE(value != nullptr);
    CHECK(std::string(value, value_size) == "250");

    const auto selector = resonate_dom_intern(document, "selector", 8);
    REQUIRE(resonate_dom_set_property(document, child, selector, ".panel", 6) == RESONATE_OK);
    CHECK(resonate_dom_property_count(document, child) == 1U);
    value = resonate_dom_property_value_at(document, child, 0, &value_size);
    REQUIRE(value != nullptr);
    CHECK(std::string(value, value_size) == ".panel");

    const auto panel_type = resonate_dom_intern(document, "Panel", 5);
    CHECK(resonate_dom_find_type_count(document, panel_type) == 1U);
    ResonateDomNode found[2] = {};
    CHECK(resonate_dom_find_type_fill(document, panel_type, found, 2) == 1U);
    CHECK(found[0].value == child.value);

    const std::uint32_t xml_size = resonate_dom_xml_size(document);
    REQUIRE(xml_size > 0U);
    std::vector<char> xml(xml_size);
    CHECK(resonate_dom_xml(document, xml.data(), xml_size) == xml_size);
    CHECK(std::string(xml.data(), xml.size()).find("selector=\".panel\"") != std::string::npos);

    resonate_dom_destroy(document);
}

TEST_CASE("document abi: a failed parse leaves the document unchanged", "[document][abi]")
{
    ResonateDomDocument* document = resonate_dom_create();
    REQUIRE(document != nullptr);
    Diagnostics diagnostics;
    const char* const broken = "<document version=\"1\"><node></document>";
    const std::uint32_t size = static_cast<std::uint32_t>(std::char_traits<char>::length(broken));
    CHECK(resonate_dom_parse(document, broken, size, &Diagnostics::sink, &diagnostics) ==
          RESONATE_E_INVALID);
    CHECK(diagnostics.errors > 0);
    CHECK(resonate_dom_root(document).value == 0U);
    resonate_dom_destroy(document);
}
