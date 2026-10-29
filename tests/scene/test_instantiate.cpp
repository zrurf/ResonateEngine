#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <string>

#include <resonate/core/allocator.h>
#include <resonate/dom/document.h>
#include <resonate/dom/schema.h>
#include <resonate/ecs/world.h>
#include <resonate/hierarchy/name.h>
#include <resonate/hierarchy/tree.h>
#include <resonate/renderer/renderer.h>
#include <resonate/scene/document.h>
#include <resonate/ui/document.h>
#include <resonate/ui/layout.h>

#include "resonate.hierarchy/nodes.gen.h"
#include "resonate.render/nodes.gen.h"
#include "resonate.scene/nodes.gen.h"

/*
 * The scene adapter: a .rscene document becomes entities, nesting becomes
 * parenting, and the components apply through the schema descriptors of the
 * modules the scene names.
 */

namespace
{

using resonate::ecs::Entity;
using resonate::ecs::World;

const char* const kScene = R"xml(<document version="1" schemas="resonate.scene">
  <Scene key="yard">
    <Object key="yard-root" name="场景">
      <Object key="sun" name="平行光">
        <DirectionalLight direction="0.35 -1 0.25" color="1 0.96 0.88" intensity="1.1"/>
        <LocalTransform position="0 5 0"/>
      </Object>
      <Object key="ground" name="地面">
        <MeshInstance mesh="2"/>
        <LocalTransform position="0 0 0"/>
      </Object>
      <Object key="crate" name="箱子">
        <MeshInstance mesh="1"/>
        <Material tint="0.9 0.45 0.35" roughness="0.4" metallic="0.1"/>
        <LocalTransform position="-2.5 0.5 0"/>
        <Object key="lid" name="箱盖">
          <LocalTransform position="0 0.6 0"/>
        </Object>
      </Object>
    </Object>
  </Scene>
</document>)xml";

struct Fixture
{
    World world{resonate::systemAllocator()};

    Fixture()
    {
        REQUIRE(resonate::hierarchy::registerComponents(world));
        REQUIRE(resonate::renderer::RenderSystem::registerComponents(world));
    }
};

/* The registry a scene names: hierarchy and render, the modules demo.rscene
   composes. Startup builds the same composite over every content module. */
const resonate::dom::ComponentRegistry& sceneComponents()
{
    static const resonate::dom::TableComponentRegistry hierarchy_table(
        resonate::Span<const resonate::dom::ComponentDesc>(
            resonate::hierarchy::generated::kComponentDescs,
            resonate::hierarchy::generated::kComponentDescCount));
    static const resonate::dom::TableComponentRegistry render_table(
        resonate::Span<const resonate::dom::ComponentDesc>(
            resonate::render::generated::kComponentDescs,
            resonate::render::generated::kComponentDescCount));
    static const std::array<const resonate::dom::ComponentRegistry*, 2> tables{
        &hierarchy_table, &render_table};
    static const resonate::dom::CompositeComponentRegistry registry(
        resonate::Span<const resonate::dom::ComponentRegistry* const>(tables.data(),
                                                                     tables.size()));
    return registry;
}

/* The schema registry the parse validates against: node types plus the
   component names a scene document may author. */
const resonate::dom::SchemaRegistry& sceneSchemas()
{
    static const resonate::dom::TableSchemaRegistry hierarchy_table(
        resonate::Span<const resonate::dom::NodeTypeDesc>(
            resonate::hierarchy::generated::kNodeTypes,
            resonate::hierarchy::generated::kNodeTypeCount),
        resonate::Span<const char* const>(resonate::hierarchy::generated::kComponents,
                                          resonate::hierarchy::generated::kComponentCount));
    static const resonate::dom::TableSchemaRegistry render_table(
        resonate::Span<const resonate::dom::NodeTypeDesc>(
            resonate::render::generated::kNodeTypes,
            resonate::render::generated::kNodeTypeCount),
        resonate::Span<const char* const>(resonate::render::generated::kComponents,
                                          resonate::render::generated::kComponentCount));
    static const resonate::dom::TableSchemaRegistry scene_table(
        resonate::Span<const resonate::dom::NodeTypeDesc>(
            resonate::scene::generated::kNodeTypes,
            resonate::scene::generated::kNodeTypeCount),
        resonate::Span<const char* const>(resonate::scene::generated::kComponents,
                                          resonate::scene::generated::kComponentCount));
    static const std::array<const resonate::dom::SchemaRegistry*, 3> tables{
        &hierarchy_table, &render_table, &scene_table};
    static const resonate::dom::CompositeSchemaRegistry registry(
        resonate::Span<const resonate::dom::SchemaRegistry* const>(tables.data(),
                                                                   tables.size()));
    return registry;
}

bool near(float actual, float expected)
{
    return std::fabs(actual - expected) < 0.001F;
}

} // namespace

TEST_CASE("scene: a document instantiates into a parented entity tree", "[scene]")
{
    std::vector<resonate::dom::Diagnostic> diagnostics;
    auto document = resonate::dom::Document::parse(kScene, &sceneSchemas(), diagnostics);
    for (const resonate::dom::Diagnostic& diagnostic : diagnostics)
    {
        INFO("parse: " << diagnostic.message);
    }
    REQUIRE(document != nullptr);
    CHECK(diagnostics.empty());

    /* The key lives in the UI module's schema, so its components register too. */
    Fixture fixture;
    REQUIRE(resonate::ui::registerComponents(fixture.world));

    Entity root{};
    std::string error;
    REQUIRE(resonate::scene::instantiate(fixture.world, *document, &sceneComponents(), root,
                                         error));
    CHECK(error.empty());
    REQUIRE(root.valid());
    CHECK(root == resonate::scene::findByKey(fixture.world, root, "yard-root"));
    CHECK(resonate::hierarchy::nameOf(fixture.world, root) == "场景");

    const Entity sun = resonate::scene::findByKey(fixture.world, root, "sun");
    const Entity ground = resonate::scene::findByKey(fixture.world, root, "ground");
    const Entity crate = resonate::scene::findByKey(fixture.world, root, "crate");
    const Entity lid = resonate::scene::findByKey(fixture.world, root, "lid");
    REQUIRE(sun.valid());
    REQUIRE(ground.valid());
    REQUIRE(crate.valid());
    REQUIRE(lid.valid());
    CHECK(resonate::hierarchy::parentOf(fixture.world, lid) == crate);

    /* Names came from the core `name` attribute. */
    CHECK(resonate::hierarchy::nameOf(fixture.world, sun) == "平行光");
    CHECK(resonate::hierarchy::nameOf(fixture.world, crate) == "箱子");
    CHECK(resonate::hierarchy::nameOf(fixture.world, lid) == "箱盖");

    /* Components applied through the schema descriptors, defaults included. */
    const auto* light = fixture.world.get<resonate::render::DirectionalLight>(sun);
    REQUIRE(light != nullptr);
    CHECK(near(light->direction.y, -1.0F));
    CHECK(near(light->intensity, 1.1F));
    const auto* mesh = fixture.world.get<resonate::render::MeshInstance>(crate);
    REQUIRE(mesh != nullptr);
    CHECK(mesh->mesh == 1U);
    const auto* material = fixture.world.get<resonate::render::Material>(crate);
    REQUIRE(material != nullptr);
    CHECK(near(material->tint.y, 0.45F));
    CHECK(near(material->roughness, 0.4F));
    CHECK(near(material->metallic, 0.1F));

    /* A transform the document never mentions takes the schema's defaults. */
    const auto* ground_transform = fixture.world.get<resonate::hierarchy::LocalTransform>(ground);
    REQUIRE(ground_transform != nullptr);
    CHECK(near(ground_transform->scale.x, 1.0F));
}

TEST_CASE("scene: a malformed document is refused with a reason", "[scene]")
{
    const char* const broken = R"xml(<document version="1" schemas="resonate.scene">
  <Scene key="yard">
    <Object key="a">
      <LocalTransform position="abc"/>
    </Object>
  </Scene>
</document>)xml";
    std::vector<resonate::dom::Diagnostic> diagnostics;
    auto document = resonate::dom::Document::parse(broken, &sceneSchemas(), diagnostics);
    REQUIRE(document != nullptr);

    Fixture fixture;
    REQUIRE(resonate::ui::registerComponents(fixture.world));
    Entity root{};
    std::string error;
    CHECK_FALSE(resonate::scene::instantiate(fixture.world, *document, &sceneComponents(), root,
                                             error));
    CHECK(error.find("LocalTransform") != std::string::npos);
}
