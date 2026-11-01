#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

#include <resonate/core/allocator.h>
#include <resonate/dom/document.h>
#include <resonate/dom/schema.h>
#include <resonate/ecs/command_buffer.h>
#include <resonate/ecs/world.h>
#include <resonate/hierarchy/name.h>
#include <resonate/hierarchy/tree.h>
#include <resonate/renderer/renderer.h>
#include <resonate/scene/capture.h>
#include <resonate/scene/document.h>
#include <resonate/ui/document.h>
#include <resonate/ui/layout.h>

#include "resonate.animation/components.gen.h"
#include "resonate.animation/nodes.gen.h"
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
        REQUIRE(resonate::animation::AnimationPlayer::registerComponents(world));
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
    static const resonate::dom::TableComponentRegistry animation_table(
        resonate::Span<const resonate::dom::ComponentDesc>(
            resonate::animation::generated::kComponentDescs,
            resonate::animation::generated::kComponentDescCount));
    static const std::array<const resonate::dom::ComponentRegistry*, 3> tables{
        &hierarchy_table, &render_table, &animation_table};
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
    static const resonate::dom::TableSchemaRegistry animation_table(
        resonate::Span<const resonate::dom::NodeTypeDesc>(
            resonate::animation::generated::kNodeTypes,
            resonate::animation::generated::kNodeTypeCount),
        resonate::Span<const char* const>(resonate::animation::generated::kComponents,
                                          resonate::animation::generated::kComponentCount));
    static const resonate::dom::TableSchemaRegistry scene_table(
        resonate::Span<const resonate::dom::NodeTypeDesc>(
            resonate::scene::generated::kNodeTypes,
            resonate::scene::generated::kNodeTypeCount),
        resonate::Span<const char* const>(resonate::scene::generated::kComponents,
                                          resonate::scene::generated::kComponentCount));
    static const std::array<const resonate::dom::SchemaRegistry*, 4> tables{
        &hierarchy_table, &render_table, &animation_table, &scene_table};
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

TEST_CASE("scene: a capture round trips through a second instantiate", "[scene]")
{
    std::vector<resonate::dom::Diagnostic> diagnostics;
    auto document = resonate::dom::Document::parse(kScene, &sceneSchemas(), diagnostics);
    REQUIRE(document != nullptr);

    Fixture fixture;
    REQUIRE(resonate::ui::registerComponents(fixture.world));
    Entity root{};
    std::string error;
    INFO("instantiate: " << error);
    REQUIRE(resonate::scene::instantiate(fixture.world, *document, &sceneComponents(), root,
                                         error));

    /* Move the box and re-temper its material: the capture must carry both. */
    const Entity crate = resonate::scene::findByKey(fixture.world, root, "crate");
    REQUIRE(crate.valid());
    auto* transform = fixture.world.get<resonate::hierarchy::LocalTransform>(crate);
    REQUIRE(transform != nullptr);
    transform->position = resonate::Vec3{3.5F, 1.25F, -0.5F};
    auto* material = fixture.world.get<resonate::render::Material>(crate);
    REQUIRE(material != nullptr);
    material->roughness = 0.85F;
    material->metallic = 0.25F;

    std::string xml;
    REQUIRE(resonate::scene::capture(fixture.world, root, xml, error));
    CHECK(error.empty());
    const bool has_key = xml.find("key=\"crate\"") != std::string::npos;
    CHECK(has_key);
    CHECK(xml.find("3.5 1.25 -0.5") != std::string::npos);
    CHECK(xml.find("roughness=\"0.85\"") != std::string::npos);
    /* Fields at their defaults are not written. */
    CHECK(xml.find("scale=") == std::string::npos);

    /* The capture parses, instantiates, and answers the same scene. */
    auto captured = resonate::dom::Document::parse(xml, &sceneSchemas(), diagnostics);
    REQUIRE(captured != nullptr);

    World second{resonate::systemAllocator()};
    REQUIRE(resonate::hierarchy::registerComponents(second));
    REQUIRE(resonate::renderer::RenderSystem::registerComponents(second));
    REQUIRE(resonate::ui::registerComponents(second));
    REQUIRE(resonate::animation::AnimationPlayer::registerComponents(second));
    Entity second_root{};
    REQUIRE(resonate::scene::instantiate(second, *captured, &sceneComponents(), second_root,
                                         error));

    const Entity second_crate = resonate::scene::findByKey(second, second_root, "crate");
    REQUIRE(second_crate.valid());
    const auto* second_transform =
        second.get<resonate::hierarchy::LocalTransform>(second_crate);
    REQUIRE(second_transform != nullptr);
    CHECK(near(second_transform->position.x, 3.5F));
    CHECK(near(second_transform->position.y, 1.25F));
    const auto* second_material = second.get<resonate::render::Material>(second_crate);
    REQUIRE(second_material != nullptr);
    CHECK(near(second_material->roughness, 0.85F));
    CHECK(near(second_material->metallic, 0.25F));
    const Entity second_lid = resonate::scene::findByKey(second, second_root, "lid");
    REQUIRE(second_lid.valid());
    CHECK(resonate::hierarchy::parentOf(second, second_lid) == second_crate);
    CHECK(resonate::hierarchy::nameOf(second, second_lid) == "箱盖");
}

TEST_CASE("scene: a captured rotation reads back as the same rotation", "[scene]")
{
    constexpr const char* kRotated = R"xml(<document version="1" schemas="resonate.scene">
  <Scene key="yard">
    <Object key="crate">
      <LocalTransform position="1 2 3" rotation="0.1 0.2 0.3 0.9"/>
    </Object>
  </Scene>
</document>)xml";
    std::vector<resonate::dom::Diagnostic> diagnostics;
    auto document = resonate::dom::Document::parse(kRotated, &sceneSchemas(), diagnostics);
    REQUIRE(document != nullptr);

    Fixture fixture;
    REQUIRE(resonate::ui::registerComponents(fixture.world));
    Entity root{};
    std::string error;
    REQUIRE(resonate::scene::instantiate(fixture.world, *document, &sceneComponents(), root,
                                         error));
    const Entity crate = resonate::scene::findByKey(fixture.world, root, "crate");
    REQUIRE(crate.valid());

    std::string xml;
    REQUIRE(resonate::scene::capture(fixture.world, crate, xml, error));
    auto captured = resonate::dom::Document::parse(xml, &sceneSchemas(), diagnostics);
    REQUIRE(captured != nullptr);
    World second{resonate::systemAllocator()};
    REQUIRE(resonate::hierarchy::registerComponents(second));
    REQUIRE(resonate::renderer::RenderSystem::registerComponents(second));
    REQUIRE(resonate::ui::registerComponents(second));
    REQUIRE(resonate::animation::AnimationPlayer::registerComponents(second));
    Entity second_root{};
    REQUIRE(resonate::scene::instantiate(second, *captured, &sceneComponents(), second_root,
                                         error));
    const Entity restored = resonate::scene::findByKey(second, second_root, "crate");
    REQUIRE(restored.valid());

    /* The reader takes a quat's tokens in member order (x y z w); a writer that
       puts w first transposes the rotation. */
    const auto* before = fixture.world.get<resonate::hierarchy::LocalTransform>(crate);
    const auto* after = second.get<resonate::hierarchy::LocalTransform>(restored);
    REQUIRE(before != nullptr);
    REQUIRE(after != nullptr);
    CHECK(near(after->rotation.x, before->rotation.x));
    CHECK(near(after->rotation.y, before->rotation.y));
    CHECK(near(after->rotation.z, before->rotation.z));
    CHECK(near(after->rotation.w, before->rotation.w));
}

TEST_CASE("scene: a capture keeps a texture reference as an asset id", "[scene]")
{
    constexpr const char* kTextured = R"xml(<document version="1" schemas="resonate.scene">
  <Scene key="yard">
    <Object key="yard-root">
      <Object key="crate">
        <MeshInstance mesh="1"/>
        <Material texture="@0704c7df-7013-4c31-818d-b515207bb7df"/>
      </Object>
      <Object key="lid">
        <MeshInstance mesh="1"/>
        <Material texture="@0704c7df-7013-4c31-818d-b515207bb7df:3"/>
      </Object>
    </Object>
  </Scene>
</document>)xml";
    std::vector<resonate::dom::Diagnostic> diagnostics;
    auto document = resonate::dom::Document::parse(kTextured, &sceneSchemas(), diagnostics);
    REQUIRE(document != nullptr);

    Fixture fixture;
    REQUIRE(resonate::ui::registerComponents(fixture.world));
    Entity root{};
    std::string error;
    REQUIRE(resonate::scene::instantiate(fixture.world, *document, &sceneComponents(), root,
                                         error));
    const Entity crate = resonate::scene::findByKey(fixture.world, root, "crate");
    REQUIRE(crate.valid());
    const auto* reference = fixture.world.get<resonate::render::TextureRef>(crate);
    REQUIRE(reference != nullptr);
    CHECK(reference->sub_id == 1U);

    std::string xml;
    REQUIRE(resonate::scene::capture(fixture.world, root, xml, error));
    CHECK(xml.find("texture=\"@0704c7df-7013-4c31-818d-b515207bb7df\"") != std::string::npos);
    CHECK(xml.find("texture=\"@0704c7df-7013-4c31-818d-b515207bb7df:3\"") != std::string::npos);

    /* The capture instantiates back to the same references, not to slots. */
    auto captured = resonate::dom::Document::parse(xml, &sceneSchemas(), diagnostics);
    REQUIRE(captured != nullptr);
    World second{resonate::systemAllocator()};
    REQUIRE(resonate::hierarchy::registerComponents(second));
    REQUIRE(resonate::renderer::RenderSystem::registerComponents(second));
    REQUIRE(resonate::ui::registerComponents(second));
    REQUIRE(resonate::animation::AnimationPlayer::registerComponents(second));
    Entity second_root{};
    REQUIRE(resonate::scene::instantiate(second, *captured, &sceneComponents(), second_root,
                                         error));
    const Entity second_lid = resonate::scene::findByKey(second, second_root, "lid");
    REQUIRE(second_lid.valid());
    const auto* second_reference = second.get<resonate::render::TextureRef>(second_lid);
    REQUIRE(second_reference != nullptr);
    CHECK(second_reference->sub_id == 3U);
    CHECK(second_reference->guid_high == reference->guid_high);
}

TEST_CASE("scene: the demo document survives a save unchanged in meaning", "[scene]")
{
    std::string text;
    {
        std::ifstream in(std::string(RESONATE_SOURCE_DIR) + "/content/scene/demo.rscene",
                         std::ios::binary);
        REQUIRE(in.is_open());
        text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    std::vector<resonate::dom::Diagnostic> diagnostics;
    auto document = resonate::dom::Document::parse(text, &sceneSchemas(), diagnostics);
    REQUIRE(document != nullptr);

    Fixture fixture;
    REQUIRE(resonate::ui::registerComponents(fixture.world));
    Entity root{};
    std::string error;
    REQUIRE(resonate::scene::instantiate(fixture.world, *document, &sceneComponents(), root,
                                         error));

    /* The editor's save writes this capture back over the authored file: every
       material's asset reference must survive it, and an alias stays an alias
       instead of expanding to the id nine times. */
    std::string xml;
    REQUIRE(resonate::scene::capture(fixture.world, root, xml, error));
    constexpr const char* kReference = "texture=\"#checker\"";
    std::size_t references = 0;
    for (std::size_t at = xml.find(kReference); at != std::string::npos;
         at = xml.find(kReference, at + 1U))
    {
        ++references;
    }
    CHECK(references == 9U);
    CHECK(xml.find("<Resource key=\"checker\" "
                   "asset=\"@0704c7df-7013-4c31-818d-b515207bb7df\"/>") != std::string::npos);
    CHECK(xml.find("texture=\"@") == std::string::npos);

    auto captured = resonate::dom::Document::parse(xml, &sceneSchemas(), diagnostics);
    REQUIRE(captured != nullptr);
    World second{resonate::systemAllocator()};
    REQUIRE(resonate::hierarchy::registerComponents(second));
    REQUIRE(resonate::renderer::RenderSystem::registerComponents(second));
    REQUIRE(resonate::ui::registerComponents(second));
    REQUIRE(resonate::animation::AnimationPlayer::registerComponents(second));
    Entity second_root{};
    INFO("instantiate: " << error);
    REQUIRE(resonate::scene::instantiate(second, *captured, &sceneComponents(), second_root,
                                         error));
    for (const char* const key : {"box-a1", "box-a2", "box-a3", "box-b1", "box-center",
                                  "box-b3", "box-c1", "box-c2", "box-c3"})
    {
        INFO("key " << key);
        const Entity box = resonate::scene::findByKey(second, second_root, key);
        REQUIRE(box.valid());
        CHECK(second.get<resonate::render::TextureRef>(box) != nullptr);
    }
    CHECK(second.get<resonate::render::TextureRef>(
              resonate::scene::findByKey(second, second_root, "ground")) == nullptr);
}

TEST_CASE("scene: a document with several roots reports all of them", "[scene]")
{
    constexpr const char* kTwoRoots = R"xml(<document version="1" schemas="resonate.scene">
  <Scene key="yard">
    <Object key="left">
      <LocalTransform position="-1 0 0"/>
    </Object>
    <Object key="right">
      <LocalTransform position="1 0 0"/>
    </Object>
  </Scene>
</document>)xml";
    std::vector<resonate::dom::Diagnostic> diagnostics;
    auto document = resonate::dom::Document::parse(kTwoRoots, &sceneSchemas(), diagnostics);
    REQUIRE(document != nullptr);

    Fixture fixture;
    REQUIRE(resonate::ui::registerComponents(fixture.world));
    Entity first{};
    std::string error;
    std::vector<Entity> roots;
    REQUIRE(resonate::scene::instantiate(fixture.world, *document, &sceneComponents(), first,
                                         error, &roots));
    /* A caller that tears a scene down or writes it back needs every root, not
       just the one the answer names. */
    REQUIRE(roots.size() == 2U);
    CHECK(first == roots[0]);
    CHECK(resonate::scene::findByKey(fixture.world, first, "left") == roots[0]);
    CHECK(resonate::scene::findByKey(fixture.world, roots[1], "right") == roots[1]);
}

TEST_CASE("scene: a captured subtree reinstantiates under its old parent", "[scene]")
{
    std::vector<resonate::dom::Diagnostic> diagnostics;
    auto document = resonate::dom::Document::parse(kScene, &sceneSchemas(), diagnostics);
    REQUIRE(document != nullptr);

    Fixture fixture;
    REQUIRE(resonate::ui::registerComponents(fixture.world));
    Entity root{};
    std::string error;
    REQUIRE(resonate::scene::instantiate(fixture.world, *document, &sceneComponents(), root,
                                         error));
    const Entity crate = resonate::scene::findByKey(fixture.world, root, "crate");
    REQUIRE(crate.valid());
    const Entity parent = resonate::hierarchy::parentOf(fixture.world, crate);
    REQUIRE(parent == root);

    /* What an editor's delete captures before it destroys the object. */
    std::string xml;
    REQUIRE(resonate::scene::capture(fixture.world, crate, xml, error));
    resonate::ecs::CommandBuffer commands(fixture.world);
    resonate::hierarchy::destroyEntity(commands, crate);
    fixture.world.play(commands);
    CHECK_FALSE(fixture.world.alive(crate));
    CHECK_FALSE(resonate::scene::findByKey(fixture.world, root, "crate").valid());

    /* The undo: the captured document instantiates back under the parent, with
       its components, its name and its own child. */
    auto captured = resonate::dom::Document::parse(xml, &sceneSchemas(), diagnostics);
    REQUIRE(captured != nullptr);
    Entity restored{};
    REQUIRE(resonate::scene::instantiateUnder(fixture.world, *captured, &sceneComponents(),
                                              parent, restored, error));
    REQUIRE(restored.valid());
    CHECK(resonate::hierarchy::parentOf(fixture.world, restored) == parent);
    CHECK(resonate::hierarchy::nameOf(fixture.world, restored) == "箱子");
    const auto* material = fixture.world.get<resonate::render::Material>(restored);
    REQUIRE(material != nullptr);
    CHECK(near(material->roughness, 0.4F));
    const Entity lid = resonate::scene::findByKey(fixture.world, root, "lid");
    REQUIRE(lid.valid());
    CHECK(resonate::hierarchy::parentOf(fixture.world, lid) == restored);
}

TEST_CASE("scene: a Resources alias stands for an asset id", "[scene]")
{
    constexpr const char* kAliased = R"xml(<document version="1" schemas="resonate.scene">
  <Scene key="yard">
    <Resources>
      <Resource key="checker" asset="@0704c7df-7013-4c31-818d-b515207bb7df"/>
    </Resources>
    <Object key="crate">
      <MeshInstance mesh="1"/>
      <Material texture="#checker"/>
    </Object>
  </Scene>
</document>)xml";
    std::vector<resonate::dom::Diagnostic> diagnostics;
    auto document = resonate::dom::Document::parse(kAliased, &sceneSchemas(), diagnostics);
    REQUIRE(document != nullptr);
    CHECK(diagnostics.empty());

    Fixture fixture;
    REQUIRE(resonate::ui::registerComponents(fixture.world));
    Entity root{};
    std::string error;
    REQUIRE(resonate::scene::instantiate(fixture.world, *document, &sceneComponents(), root,
                                         error));
    const Entity crate = resonate::scene::findByKey(fixture.world, root, "crate");
    REQUIRE(crate.valid());
    const auto* reference = fixture.world.get<resonate::render::TextureRef>(crate);
    REQUIRE(reference != nullptr);
    CHECK(reference->sub_id == 1U);
    CHECK((reference->guid_low != 0U || reference->guid_high != 0U));
    const auto* alias = fixture.world.get<resonate::scene::TextureAlias>(crate);
    REQUIRE(alias != nullptr);
    const std::string_view alias_text(
        static_cast<const char*>(fixture.world.blobData(alias->text)),
        fixture.world.blobSize(alias->text));
    CHECK(alias_text == "checker");

    /* The capture keeps the name and writes the table it came from, so the
       document does not grow a guid per material. */
    std::string xml;
    REQUIRE(resonate::scene::capture(fixture.world, root, xml, error));
    CHECK(xml.find("texture=\"#checker\"") != std::string::npos);
    CHECK(xml.find("<Resource key=\"checker\" "
                   "asset=\"@0704c7df-7013-4c31-818d-b515207bb7df\"/>") != std::string::npos);

    /* A name the table lacks is refused, and the message names it. */
    constexpr const char* kMissing = R"xml(<document version="1" schemas="resonate.scene">
  <Scene key="yard">
    <Object key="crate">
      <Material texture="#missing"/>
    </Object>
  </Scene>
</document>)xml";
    auto broken = resonate::dom::Document::parse(kMissing, &sceneSchemas(), diagnostics);
    REQUIRE(broken != nullptr);
    World second{resonate::systemAllocator()};
    REQUIRE(resonate::hierarchy::registerComponents(second));
    REQUIRE(resonate::renderer::RenderSystem::registerComponents(second));
    REQUIRE(resonate::ui::registerComponents(second));
    REQUIRE(resonate::animation::AnimationPlayer::registerComponents(second));
    Entity broken_root{};
    CHECK_FALSE(resonate::scene::instantiate(second, *broken, &sceneComponents(), broken_root,
                                             error));
    CHECK(error.find("missing") != std::string::npos);
}

TEST_CASE("scene: every scene component is capturable or deliberately not", "[scene]")
{
    /* The writer walks these tables, so a component added here without being
       derived, adapter-managed or made of writable field kinds would be
       silently dropped on save. This is the gate that says so. */
    const auto expect = [](resonate::Span<const resonate::dom::ComponentDesc> descs,
                           std::initializer_list<std::string_view> exempt)
    {
        for (const resonate::dom::ComponentDesc& desc : descs)
        {
            INFO("component " << desc.qualified_name);
            const bool listed = std::find(exempt.begin(), exempt.end(),
                                          std::string_view(desc.qualified_name)) != exempt.end();
            CHECK(resonate::scene::capturable(desc) == !listed);
        }
    };
    expect(resonate::Span<const resonate::dom::ComponentDesc>(
               resonate::hierarchy::generated::kComponentDescs,
               resonate::hierarchy::generated::kComponentDescCount),
           {"resonate.hierarchy.Name", "resonate.hierarchy.Parent",
            "resonate.hierarchy.Children", "resonate.hierarchy.WorldTransform"});
    expect(resonate::Span<const resonate::dom::ComponentDesc>(
               resonate::render::generated::kComponentDescs,
               resonate::render::generated::kComponentDescCount),
           {"resonate.render.TextureRef"});
    expect(resonate::Span<const resonate::dom::ComponentDesc>(
               resonate::scene::generated::kComponentDescs,
               resonate::scene::generated::kComponentDescCount),
           {"resonate.scene.SourceKey", "resonate.scene.TextureAlias"});
}

TEST_CASE("scene: several roots write back as one document", "[scene]")
{
    std::vector<resonate::dom::Diagnostic> diagnostics;
    auto document = resonate::dom::Document::parse(kScene, &sceneSchemas(), diagnostics);
    REQUIRE(document != nullptr);

    Fixture fixture;
    REQUIRE(resonate::ui::registerComponents(fixture.world));
    Entity root{};
    std::string error;
    REQUIRE(resonate::scene::instantiate(fixture.world, *document, &sceneComponents(), root,
                                         error));
    const Entity ground = resonate::scene::findByKey(fixture.world, root, "ground");
    REQUIRE(ground.valid());

    /* The document's root object and the ground become siblings. */
    resonate::ecs::CommandBuffer commands(fixture.world);
    resonate::hierarchy::clearParent(commands, ground);
    fixture.world.play(commands);
    const std::array<resonate::ecs::Entity, 2> roots{root, ground};
    std::string xml;
    REQUIRE(resonate::scene::capture(fixture.world,
                                     resonate::Span<const resonate::ecs::Entity>(roots.data(), 2U),
                                     xml, error));
    auto captured = resonate::dom::Document::parse(xml, &sceneSchemas(), diagnostics);
    REQUIRE(captured != nullptr);

    World second{resonate::systemAllocator()};
    REQUIRE(resonate::hierarchy::registerComponents(second));
    REQUIRE(resonate::renderer::RenderSystem::registerComponents(second));
    REQUIRE(resonate::ui::registerComponents(second));
    REQUIRE(resonate::animation::AnimationPlayer::registerComponents(second));
    Entity first_root{};
    std::vector<Entity> restored;
    REQUIRE(resonate::scene::instantiate(second, *captured, &sceneComponents(), first_root, error,
                                         &restored));
    CHECK(restored.size() == 2U);
    CHECK(resonate::scene::findByKey(second, restored[0], "yard-root").valid());
    CHECK(resonate::scene::findByKey(second, restored[1], "ground").valid());
}

TEST_CASE("scene: an Animation component round trips as an asset id", "[scene]")
{
    constexpr const char* kAnimated = R"xml(<document version="1" schemas="resonate.scene">
  <Scene key="yard">
    <Object key="crate">
      <Animation clip="@0704c7df-7013-4c31-818d-b515207bb7df" speed="2"/>
    </Object>
  </Scene>
</document>)xml";
    std::vector<resonate::dom::Diagnostic> diagnostics;
    auto document = resonate::dom::Document::parse(kAnimated, &sceneSchemas(), diagnostics);
    REQUIRE(document != nullptr);
    CHECK(diagnostics.empty());

    Fixture fixture;
    REQUIRE(resonate::ui::registerComponents(fixture.world));
    REQUIRE(resonate::animation::AnimationPlayer::registerComponents(fixture.world));
    Entity root{};
    std::string error;
    REQUIRE(resonate::scene::instantiate(fixture.world, *document, &sceneComponents(), root,
                                         error));
    const Entity crate = resonate::scene::findByKey(fixture.world, root, "crate");
    REQUIRE(crate.valid());
    const auto* authored = fixture.world.get<resonate::animation::Animation>(crate);
    REQUIRE(authored != nullptr);
    CHECK(authored->speed == 2.0F);
    CHECK(authored->clip_high != 0U);

    std::string xml;
    REQUIRE(resonate::scene::capture(fixture.world, crate, xml, error));
    INFO("capture: " << xml);
    /* The authoring form, not the two numbers the struct carries. */
    CHECK(xml.find("clip=\"@0704c7df-7013-4c31-818d-b515207bb7df\"") != std::string::npos);
    CHECK(xml.find("clip_low") == std::string::npos);
    CHECK(xml.find("speed=\"2\"") != std::string::npos);

    auto captured = resonate::dom::Document::parse(xml, &sceneSchemas(), diagnostics);
    REQUIRE(captured != nullptr);
    World second{resonate::systemAllocator()};
    REQUIRE(resonate::hierarchy::registerComponents(second));
    REQUIRE(resonate::renderer::RenderSystem::registerComponents(second));
    REQUIRE(resonate::ui::registerComponents(second));
    REQUIRE(resonate::animation::AnimationPlayer::registerComponents(second));
    REQUIRE(resonate::animation::AnimationPlayer::registerComponents(second));
    Entity second_root{};
    REQUIRE(resonate::scene::instantiate(second, *captured, &sceneComponents(), second_root,
                                         error));
    const Entity restored = resonate::scene::findByKey(second, second_root, "crate");
    REQUIRE(restored.valid());
    const auto* again = second.get<resonate::animation::Animation>(restored);
    REQUIRE(again != nullptr);
    CHECK(again->clip_low == authored->clip_low);
    CHECK(again->clip_high == authored->clip_high);
    CHECK(again->speed == 2.0F);
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
