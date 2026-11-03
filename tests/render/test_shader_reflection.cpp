#include <catch2/catch_all.hpp>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include <resonate/module/host.hpp>
#include <resonate/render/device.h>
#include <resonate/render/offscreen.hpp>

#include "paths.h"

namespace {

/* Reads one compiled module from the build's shader directory. */
std::vector<std::uint32_t> readSpirv(const std::string& directory, const char* file)
{
    std::ifstream in(directory + "/" + file, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());
    std::vector<std::uint32_t> words(bytes.size() / 4U);
    for (std::size_t index = 0U; index < words.size(); ++index)
    {
        std::memcpy(&words[index], bytes.data() + index * 4U, 4U);
    }
    return words;
}

/* SPIR-V instructions the patches below need: OpDecorate (71) carries a
   decoration literal; OpConstant (43) carries a value literal. */
constexpr std::uint32_t kOpDecorate = 71U;
constexpr std::uint32_t kOpConstant = 43U;
constexpr std::uint32_t kDecorationBinding = 33U;
constexpr std::uint32_t kDecorationDescriptorSet = 34U;

bool patchDecoration(std::vector<std::uint32_t>& words, std::uint32_t decoration,
                     std::uint32_t from, std::uint32_t to)
{
    for (std::size_t index = 5U; index < words.size();)
    {
        const std::uint32_t count = words[index] >> 16U;
        if (count == 0U || index + count > words.size())
        {
            break;
        }
        if ((words[index] & 0xFFFFU) == kOpDecorate && count >= 4U &&
            words[index + 2U] == decoration && words[index + 3U] == from)
        {
            words[index + 3U] = to;
            return true;
        }
        index += count;
    }
    return false;
}

bool patchConstant(std::vector<std::uint32_t>& words, std::uint32_t from, std::uint32_t to)
{
    for (std::size_t index = 5U; index < words.size();)
    {
        const std::uint32_t count = words[index] >> 16U;
        if (count == 0U || index + count > words.size())
        {
            break;
        }
        if ((words[index] & 0xFFFFU) == kOpConstant && count == 4U && words[index + 3U] == from)
        {
            words[index + 3U] = to;
            return true;
        }
        index += count;
    }
    return false;
}

} // namespace

/* create_shader walks the module's descriptors before the driver sees it: a
   table whose length drifted, an out-of-contract binding or a set the engine
   never binds must fail here with the offence, not as a draw that reads
   nothing (R6). */
TEST_CASE("offscreen: shader reflection holds the descriptor contract",
          "[render][offscreen]")
{
    const std::string plugin_directory = resonate::test::findBuildDirectory("plugins");
    REQUIRE_FALSE(plugin_directory.empty());
    const std::string shader_directory = resonate::test::findBuildDirectory("shaders");
    REQUIRE_FALSE(shader_directory.empty());

    auto host = resonate::ModuleHost::create(resonate::systemAllocator());
    REQUIRE(host != nullptr);
    REQUIRE(resonate::render::publishOffscreen(host->capabilities(), 64U, 64U) == RESONATE_OK);
    REQUIRE(host->discover(plugin_directory) == RESONATE_OK);
    REQUIRE(host->resolve() == RESONATE_OK);
    REQUIRE(host->attachAll() == RESONATE_OK);

    auto* device = static_cast<ResonateRenderDevice*>(resonate_capability_find(
        host->capabilities(), resonate::detail::CapabilityTraits<ResonateRenderDevice>::id.value(),
        1U, nullptr));
    REQUIRE(device != nullptr);
    REQUIRE(device->attach_offscreen(device->self, 64U, 64U) == RESONATE_OK);

    /* The blend module as compiled passes its own contract. */
    const std::vector<std::uint32_t> blend = readSpirv(shader_directory, "post.blendFragment.spv");
    REQUIRE_FALSE(blend.empty());
    ResonateRenderShader shader{};
    REQUIRE(device->create_shader(device->self, RESONATE_RENDER_SHADER_FRAGMENT, blend.data(),
                                  blend.size() * 4U, &shader) == RESONATE_OK);
    device->destroy_shader(device->self, shader);

    /* An image table that drifted back to 16 entries. */
    std::vector<std::uint32_t> drifted = blend;
    REQUIRE(patchConstant(drifted, 64U, 16U));
    CHECK(device->create_shader(device->self, RESONATE_RENDER_SHADER_FRAGMENT, drifted.data(),
                                drifted.size() * 4U, &shader) == RESONATE_E_INVALID);

    /* A binding outside the engine's tables. */
    drifted = blend;
    REQUIRE(patchDecoration(drifted, kDecorationBinding, 5U, 9U));
    CHECK(device->create_shader(device->self, RESONATE_RENDER_SHADER_FRAGMENT, drifted.data(),
                                drifted.size() * 4U, &shader) == RESONATE_E_INVALID);

    /* A set the engine never binds. */
    drifted = blend;
    REQUIRE(patchDecoration(drifted, kDecorationDescriptorSet, 0U, 1U));
    CHECK(device->create_shader(device->self, RESONATE_RENDER_SHADER_FRAGMENT, drifted.data(),
                                drifted.size() * 4U, &shader) == RESONATE_E_INVALID);

    /* A fragment module offered as a vertex shader: no vertex entry point. */
    const std::vector<std::uint32_t> vertex = readSpirv(shader_directory, "post.postVertex.spv");
    REQUIRE_FALSE(vertex.empty());
    CHECK(device->create_shader(device->self, RESONATE_RENDER_SHADER_VERTEX, vertex.data(),
                                vertex.size() * 4U, &shader) == RESONATE_OK);
    device->destroy_shader(device->self, shader);
    CHECK(device->create_shader(device->self, RESONATE_RENDER_SHADER_FRAGMENT, vertex.data(),
                                vertex.size() * 4U, &shader) == RESONATE_E_INVALID);

    host->detachAll();
}
