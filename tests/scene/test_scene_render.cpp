#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include <resonate/core/allocator.h>
#include <resonate/dom/document.h>
#include <resonate/dom/schema.h>
#include <resonate/ecs/world.h>
#include <resonate/hierarchy/tree.h>
#include <resonate/module/host.hpp>
#include <resonate/render/device.h>
#include <resonate/render/offscreen.hpp>
#include <resonate/renderer/renderer.h>
#include <resonate/scene/capture.h>
#include <resonate/asset/identity.h>
#include <resonate/asset/vfs.h>
#include <resonate/scene/document.h>
#include <resonate/ui/document.h>
#include <resonate/ui/layout.h>

#include <cstdio>

#include "resonate.animation/components.gen.h"
#include "resonate.animation/nodes.gen.h"
#include "resonate.hierarchy/nodes.gen.h"
#include "resonate.render/nodes.gen.h"
#include "resonate.scene/nodes.gen.h"

#include "paths.h"

/*
 * RT1 end to end: the demo scene names its checker texture by asset id, the
 * renderer resolves that id through the products manifest its texture was
 * cooked with, and the boxes render with the checker (not the white fallback
 * an unresolved reference would leave).
 */

namespace
{

using resonate::ecs::Entity;
using resonate::ecs::World;

bool readSourceFile(const std::string& relative, std::string& out_text)
{
    std::ifstream in(std::string(RESONATE_SOURCE_DIR) + "/" + relative, std::ios::binary);
    if (!in.is_open())
    {
        return false;
    }
    out_text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

const resonate::dom::SchemaRegistry& sceneSchemas()
{
    static const resonate::dom::TableSchemaRegistry animation_table(
        resonate::Span<const resonate::dom::NodeTypeDesc>(
            resonate::animation::generated::kNodeTypes,
            resonate::animation::generated::kNodeTypeCount),
        resonate::Span<const char* const>(resonate::animation::generated::kComponents,
                                          resonate::animation::generated::kComponentCount));
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
    static const std::array<const resonate::dom::SchemaRegistry*, 4> tables{
        &hierarchy_table, &render_table, &animation_table, &scene_table};
    static const resonate::dom::CompositeSchemaRegistry registry(
        resonate::Span<const resonate::dom::SchemaRegistry* const>(tables.data(),
                                                                   tables.size()));
    return registry;
}

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

} // namespace

TEST_CASE("scene render: a guid texture reference resolves to the cooked texture", "[scene][rt1]")
{
    const std::string plugin_directory = resonate::test::findBuildDirectory("plugins");
    REQUIRE_FALSE(plugin_directory.empty());

    auto host = resonate::ModuleHost::create(resonate::systemAllocator());
    REQUIRE(host != nullptr);
    constexpr std::uint32_t kWidth = 640U;
    constexpr std::uint32_t kHeight = 400U;
    REQUIRE(resonate::render::publishOffscreen(host->capabilities(), kWidth, kHeight) ==
            RESONATE_OK);
    REQUIRE(host->discover(plugin_directory) == RESONATE_OK);
    REQUIRE(host->resolve() == RESONATE_OK);
    REQUIRE(host->attachAll() == RESONATE_OK);

    auto* device = static_cast<ResonateRenderDevice*>(resonate_capability_find(
        host->capabilities(), resonate::detail::CapabilityTraits<ResonateRenderDevice>::id.value(),
        1U, nullptr));
    REQUIRE(device != nullptr);

    World world{resonate::systemAllocator()};
    REQUIRE(resonate::hierarchy::registerComponents(world));
    REQUIRE(resonate::renderer::RenderSystem::registerComponents(world));
    REQUIRE(resonate::ui::registerComponents(world));
    REQUIRE(resonate::animation::AnimationPlayer::registerComponents(world));

    std::string scene_text;
    REQUIRE(readSourceFile("content/scene/demo.rscene", scene_text));
    std::vector<resonate::dom::Diagnostic> diagnostics;
    auto document = resonate::dom::Document::parse(scene_text, &sceneSchemas(), diagnostics);
    REQUIRE(document != nullptr);
    CHECK(diagnostics.empty());

    Entity root{};
    std::string error;
    REQUIRE(resonate::scene::instantiate(world, *document, &sceneComponents(), root, error));
    CHECK(error.empty());

    /* The hierarchy propagation is a stage system; this test drives the
       renderer directly, so it composes the world matrices itself (the demo
       tree is one level deep, all parents at the origin). */
    resonate::ecs::QueryDesc transforms_desc{};
    const resonate::ecs::ComponentIndex transform_types[] = {
        resonate::ecs::ComponentTraits<resonate::hierarchy::LocalTransform>::index,
        resonate::ecs::ComponentTraits<resonate::hierarchy::WorldTransform>::index};
    transforms_desc.all =
        resonate::Span<const resonate::ecs::ComponentIndex>(transform_types, 2U);
    resonate::ecs::Query transforms = world.createQuery(transforms_desc);
    REQUIRE(transforms.valid());
    struct Compose
    {
        resonate::ecs::World* world;
    };
    Compose compose{&world};
    transforms.forEachChunk(
        [](void* context, resonate::ecs::ChunkView view)
        {
            auto* compose = static_cast<Compose*>(context);
            resonate::ecs::Column<resonate::hierarchy::LocalTransform> locals =
                resonate::ecs::column<resonate::hierarchy::LocalTransform>(view);
            resonate::ecs::Column<resonate::hierarchy::WorldTransform> worlds =
                resonate::ecs::column<resonate::hierarchy::WorldTransform>(view);
            for (std::uint32_t row = 0; row < view.count; ++row)
            {
                worlds.data[row].matrix = resonate::composeTrs(
                    locals.data[row].position, locals.data[row].rotation,
                    locals.data[row].scale);
            }
        },
        &compose);

    resonate::renderer::RenderSystem render_system;
    render_system.setRegistry(host->capabilities());
    /* The authored textures come through the store the cook wrote; without
       one the renderer's path-based checker remains the fallback. */
    std::string store_error;
    const std::unique_ptr<resonate::asset::DirectoryVfs> texture_store =
        resonate::asset::DirectoryVfs::open(
            resonate::test::findBuildDirectory("content/textures") + "/products.toml",
            store_error);
    REQUIRE(texture_store != nullptr);
    render_system.setAssetStore(texture_store.get());
    for (int frame = 0; frame < 8; ++frame)
    {
        render_system.run(world);
        render_system.waitIdle();
    }

    /* The manifest beside the cooked texture names the checker's asset; the
       renderer must map that id to its slot. */
    std::ifstream products(resonate::test::findBuildDirectory("content/textures") +
                               "/products.toml",
                           std::ios::binary);
    REQUIRE(products.is_open());
    const std::string products_text((std::istreambuf_iterator<char>(products)),
                                    std::istreambuf_iterator<char>());
    const std::size_t guid_at = products_text.find("guid = \"");
    REQUIRE(guid_at != std::string::npos);
    const std::string guid =
        products_text.substr(guid_at + 8U, 36U); /* canonical form, dashes included */

    /* The checker's sub id is 1 in the manifest; the guid parses through the
       same routine the scene reader uses. */
    resonate::asset::AssetId checker{};
    REQUIRE(resonate::asset::parseGuid(guid, checker));
    checker.sub_id = 1U;
    /* The slot is an image-table index whose value depends on the allocation
       order, which is exactly why references go by asset id: the answer only
       has to be the checker's slot (non-zero) and stable across calls. */
    const std::uint32_t slot =
        render_system.assetTextureSlot(checker.guid_low, checker.guid_high, checker.sub_id);
    CHECK(slot != 0U);
    CHECK(slot == render_system.assetTextureSlot(checker.guid_low, checker.guid_high,
                                                 checker.sub_id));
    /* An id nothing cooked answers the white fallback. */
    CHECK(render_system.assetTextureSlot(1U, 2U, 3U) == 0U);

    /* The boxes carry the reference the reader left beside their material. */
    const Entity center = resonate::scene::findByKey(world, root, "box-center");
    REQUIRE(center.valid());
    const auto* reference = world.get<resonate::render::TextureRef>(center);
    REQUIRE(reference != nullptr);
    CHECK(reference->guid_low == checker.guid_low);
    CHECK(reference->sub_id == 1U);

    /* The rendered frame keeps the checker's local contrast in the box region:
       an unresolved reference would render the flat white fallback instead. */
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(kWidth) * kHeight * 4U);
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    REQUIRE(device->readback_canvas(device->self, pixels.data(),
                                    static_cast<std::uint64_t>(pixels.size()), &width,
                                    &height) == RESONATE_OK);

    /* The boxes sit in the lower half of the frame at this camera; the checker
       alternates light and dark cells, so the band's luminance range is wide.
       A flat fallback (white) would keep the band near-uniform. */
    std::uint8_t minimum = 255U;
    std::uint8_t maximum = 0U;
    for (std::uint32_t y = kHeight * 55U / 100U; y < kHeight * 85U / 100U; ++y)
    {
        for (std::uint32_t x = kWidth * 20U / 100U; x < kWidth * 80U / 100U; ++x)
        {
            const std::size_t offset = (static_cast<std::size_t>(y) * width + x) * 4U;
            const std::uint8_t luminance = static_cast<std::uint8_t>(
                (pixels[offset] + pixels[offset + 1U] + pixels[offset + 2U]) / 3U);
            minimum = luminance < minimum ? luminance : minimum;
            maximum = luminance > maximum ? luminance : maximum;
        }
    }
    INFO("luminance range " << static_cast<int>(minimum) << ".." << static_cast<int>(maximum));
    if (const char* dump = std::getenv("RESONATE_DUMP_SCENE"); dump != nullptr)
    {
        std::ofstream bmp(dump, std::ios::binary);
        if (bmp.is_open())
        {
            const std::uint32_t stride = (width * 3U + 3U) & ~3U;
            const std::uint32_t size = 54U + stride * height;
            unsigned char header[54] = {'B', 'M', 0, 0, 0, 0, 0, 0, 0, 0, 54, 0, 0, 0,
                                        40, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
            const auto put32 = [&header](std::size_t at, std::uint32_t value)
            {
                for (int byte = 0; byte < 4; ++byte)
                {
                    header[at + static_cast<std::size_t>(byte)] =
                        static_cast<unsigned char>((value >> (8U * byte)) & 0xFFU);
                }
            };
            put32(2U, size);
            put32(18U, width);
            put32(22U, height);
            put32(28U, 24U);
            put32(34U, stride * height);
            bmp.write(reinterpret_cast<const char*>(header), sizeof(header));
            std::vector<unsigned char> row(stride, 0U);
            for (std::uint32_t y = 0U; y < height; ++y)
            {
                const std::uint8_t* source =
                    pixels.data() + static_cast<std::size_t>(height - 1U - y) * width * 4U;
                for (std::uint32_t x = 0U; x < width; ++x)
                {
                    row[x * 3U] = source[x * 4U + 2U];
                    row[x * 3U + 1U] = source[x * 4U + 1U];
                    row[x * 3U + 2U] = source[x * 4U];
                }
                bmp.write(reinterpret_cast<const char*>(row.data()), row.size());
            }
        }
    }
    CHECK(maximum - minimum > 30U);

    render_system.release();
    host->detachAll();
    host.reset();
}
