#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <resonate/core/allocator.h>
#include <resonate/dom/document.h>
#include <resonate/dom/schema.h>
#include <resonate/ecs/world.h>
#include <resonate/hierarchy/tree.h>
#include <resonate/renderer/renderer.h>
#include <resonate/ui/document.h>
#include <resonate/ui/document_schema.h>
#include <resonate/ui/layout.h>

#include "resonate.hierarchy/nodes.gen.h"
#include "resonate.render/components.gen.h"
#include "resonate.render/nodes.gen.h"
#include "resonate.ui/nodes.gen.h"

namespace
{

using resonate::Span;
using resonate::dom::ComponentDesc;
using resonate::dom::NodeTypeDesc;

const resonate::dom::TableSchemaRegistry& uiTable()
{
    static const resonate::dom::TableSchemaRegistry table(
        Span<const NodeTypeDesc>(resonate::ui::generated::kNodeTypes,
                                 resonate::ui::generated::kNodeTypeCount),
        Span<const char* const>(resonate::ui::generated::kComponents,
                                resonate::ui::generated::kComponentCount));
    return table;
}

const resonate::dom::TableSchemaRegistry& hierarchyTable()
{
    static const resonate::dom::TableSchemaRegistry table(
        Span<const NodeTypeDesc>(resonate::hierarchy::generated::kNodeTypes,
                                 resonate::hierarchy::generated::kNodeTypeCount),
        Span<const char* const>(resonate::hierarchy::generated::kComponents,
                                resonate::hierarchy::generated::kComponentCount));
    return table;
}

const resonate::dom::TableSchemaRegistry& renderTable()
{
    static const resonate::dom::TableSchemaRegistry table(
        Span<const NodeTypeDesc>(resonate::render::generated::kNodeTypes,
                                 resonate::render::generated::kNodeTypeCount),
        Span<const char* const>(resonate::render::generated::kComponents,
                                resonate::render::generated::kComponentCount));
    return table;
}

const resonate::dom::CompositeSchemaRegistry& combinedSchema()
{
    static const std::array<const resonate::dom::SchemaRegistry*, 3> tables{
        &uiTable(), &hierarchyTable(), &renderTable()};
    static const resonate::dom::CompositeSchemaRegistry registry(
        Span<const resonate::dom::SchemaRegistry* const>(tables.data(), tables.size()));
    return registry;
}

const resonate::dom::TableComponentRegistry& uiComponents()
{
    static const resonate::dom::TableComponentRegistry table(
        Span<const ComponentDesc>(resonate::ui::generated::kComponentDescs,
                                  resonate::ui::generated::kComponentDescCount));
    return table;
}

const resonate::dom::TableComponentRegistry& hierarchyComponents()
{
    static const resonate::dom::TableComponentRegistry table(
        Span<const ComponentDesc>(resonate::hierarchy::generated::kComponentDescs,
                                  resonate::hierarchy::generated::kComponentDescCount));
    return table;
}

const resonate::dom::TableComponentRegistry& renderComponents()
{
    static const resonate::dom::TableComponentRegistry table(
        Span<const ComponentDesc>(resonate::render::generated::kComponentDescs,
                                  resonate::render::generated::kComponentDescCount));
    return table;
}

const resonate::dom::CompositeComponentRegistry& combinedComponents()
{
    static const std::array<const resonate::dom::ComponentRegistry*, 3> tables{
        &uiComponents(), &hierarchyComponents(), &renderComponents()};
    static const resonate::dom::CompositeComponentRegistry registry(
        Span<const resonate::dom::ComponentRegistry* const>(tables.data(), tables.size()));
    return registry;
}

const char* const kMixed = R"xml(<document version="1" schemas="resonate.ui,resonate.hierarchy,resonate.render">
  <Screen key="hud" health="1" max_health="1" ammo="1" max_ammo="1">
    <node key="box" name="Box">
      <LocalTransform position="0 1 0" rotation="0 0 0 1" scale="1 1 1"/>
      <MeshInstance mesh="1" color="0.8 0.2 0.2"/>
    </node>
  </Screen>
</document>)xml";

} // namespace

TEST_CASE("mixed document: scene components attach through schema descriptors",
          "[ui][document][mixed]")
{
    std::vector<resonate::dom::Diagnostic> diagnostics;
    auto document = resonate::dom::Document::parse(kMixed, &combinedSchema(), diagnostics);
    REQUIRE(document != nullptr);
    CHECK(diagnostics.empty());

    resonate::ecs::World world(resonate::systemAllocator());
    REQUIRE(resonate::hierarchy::registerComponents(world));
    REQUIRE(resonate::renderer::RenderSystem::registerComponents(world));
    REQUIRE(resonate::ui::registerComponents(world));

    const resonate::ecs::Entity root = resonate::ui::instantiate(world, *document, &combinedComponents());
    REQUIRE(root.valid());

    const auto children = resonate::hierarchy::children(world, root);
    REQUIRE(children.size() == 1U);
    const resonate::ecs::Entity box = children[0];

    const auto* transform = world.get<resonate::hierarchy::LocalTransform>(box);
    REQUIRE(transform != nullptr);
    CHECK(transform->position.y == 1.0F);
    CHECK(transform->rotation.w == 1.0F);
    CHECK(transform->scale.x == 1.0F);

    const auto* mesh = world.get<resonate::render::MeshInstance>(box);
    REQUIRE(mesh != nullptr);
    CHECK(mesh->mesh == 1U);
    CHECK(mesh->color.x > 0.79F);
    CHECK(mesh->color.y < 0.21F);
}
