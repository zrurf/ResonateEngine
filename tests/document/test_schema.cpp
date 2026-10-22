#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <resonate/dom/document.h>
#include <resonate/dom/schema.h>

namespace
{

using resonate::Span;
using resonate::dom::Document;
using resonate::dom::AuthoringField;
using resonate::dom::ExposedField;
using resonate::dom::NodeTypeDesc;
using resonate::dom::UsedComponent;

constexpr ExposedField kRectExposed[] = {
    {"x", "RectTransform", "x"},
    {"width", "RectTransform", "width"},
};

constexpr ExposedField kBackgroundExposed[] = {
    {"color", "Background", "color"},
};

constexpr UsedComponent kPanelUses[] = {
    {"RectTransform", Span<const ExposedField>(kRectExposed, std::size(kRectExposed))},
    {"Background", Span<const ExposedField>(kBackgroundExposed, std::size(kBackgroundExposed))},
};

constexpr UsedComponent kLabelUses[] = {
    {"RectTransform", Span<const ExposedField>(kRectExposed, std::size(kRectExposed))},
};

constexpr AuthoringField kPanelAuthoring[] = {
    {"selector", "string"},
};

constexpr NodeTypeDesc kTypes[] = {
    {"Panel", "test.Panel", Span<const UsedComponent>(kPanelUses, std::size(kPanelUses)),
     Span<const AuthoringField>(kPanelAuthoring, std::size(kPanelAuthoring))},
    {"Label", "test.Label", Span<const UsedComponent>(kLabelUses, std::size(kLabelUses)),
     Span<const AuthoringField>{}},
};

class TestSchema final : public resonate::dom::SchemaRegistry
{
  public:
    explicit TestSchema(bool ambiguous = false) : ambiguous_(ambiguous)
    {
    }

    [[nodiscard]] const NodeTypeDesc* findNodeType(std::string_view name) const override
    {
        for (const NodeTypeDesc& type : kTypes)
        {
            if (name == type.name || name == type.qualified_name)
            {
                return &type;
            }
        }
        return nullptr;
    }

    [[nodiscard]] bool isComponent(std::string_view name) const override
    {
        return name == "RectTransform" || name == "Background" ||
               (ambiguous_ && name == "Panel");
    }

  private:
    bool ambiguous_ = false;
};

std::string fieldValue(Document& document, resonate::dom::NodeHandle node, const char* component,
                       const char* field)
{
    const auto* found = document.component(node, document.intern(component));
    REQUIRE(found != nullptr);
    for (const auto& entry : found->fields)
    {
        if (document.text(entry.name) == field)
        {
            return entry.value;
        }
    }
    return {};
}

} // namespace

TEST_CASE("document schema: implicit elements expand to node types and components",
          "[document][schema]")
{
    const TestSchema schema;
    const char* text = R"xml(<document version="1" schemas="test">
  <Panel key="panel" id="p" selector=".panel" x="10" width="100">
    <component type="RectTransform" width="120"/>
    <Label key="title" x="4"/>
  </Panel>
</document>)xml";

    std::vector<resonate::dom::Diagnostic> diagnostics;
    auto document = Document::parse(text, &schema, diagnostics);
    REQUIRE(document != nullptr);
    CHECK(diagnostics.empty());

    const auto root = document->root();
    REQUIRE(root.valid());
    CHECK(document->type(root) == document->intern("Panel"));
    const auto* selector = document->property(root, document->intern("selector"));
    REQUIRE(selector != nullptr);
    CHECK(selector->value == ".panel");
    CHECK(document->hasComponent(root, document->intern("RectTransform")));
    CHECK(document->hasComponent(root, document->intern("Background")));
    /* The exposed x and the explicit component's width share one component. */
    CHECK(fieldValue(*document, root, "RectTransform", "x") == "10");
    CHECK(fieldValue(*document, root, "RectTransform", "width") == "120");

    const auto title = document->firstChild(root);
    REQUIRE(title.valid());
    CHECK(document->type(title) == document->intern("Label"));
    CHECK(document->hasComponent(title, document->intern("RectTransform")));
    CHECK(fieldValue(*document, title, "RectTransform", "x") == "4");

    const std::string xml = document->toXml();
    CHECK(xml.find("selector=\".panel\"") != std::string::npos);
    std::vector<resonate::dom::Diagnostic> round_diagnostics;
    auto reparsed = Document::parse(xml, &schema, round_diagnostics);
    REQUIRE(reparsed != nullptr);
    CHECK(reparsed->property(reparsed->root(), reparsed->intern("selector"))->value == ".panel");
}

TEST_CASE("document schema: unknown and ambiguous implicit elements are refused",
          "[document][schema]")
{
    const TestSchema schema;
    const TestSchema ambiguous(true);
    const char* const invalid[] = {
        "<document version=\"1\"><Panel foo=\"1\"/></document>",
        "<document version=\"1\"><Unknown/></document>",
    };

    for (const char* text : invalid)
    {
        std::vector<resonate::dom::Diagnostic> diagnostics;
        auto document = Document::parse(text, &schema, diagnostics);
        INFO(text);
        CHECK(document == nullptr);
        CHECK_FALSE(diagnostics.empty());
    }

    std::vector<resonate::dom::Diagnostic> diagnostics;
    auto ambiguous_document =
        Document::parse("<document version=\"1\"><Panel/></document>", &ambiguous, diagnostics);
    CHECK(ambiguous_document == nullptr);
    CHECK_FALSE(diagnostics.empty());
}
