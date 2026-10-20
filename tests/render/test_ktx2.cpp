#include <catch2/catch_all.hpp>

#include <cstdint>
#include <string>

#include <resonate/renderer/texture.h>

#include "paths.h"

TEST_CASE("ktx2: the cooked checker decodes", "[render][ktx2]")
{
    /* The renderer target's build hook cooks content/render/textures into
       build/content; the test target depends on the renderer, so a missing
       directory is a build problem rather than a reason to pass without
       checking. */
    const std::string content = resonate::test::findBuildDirectory("content");
    INFO("run `xmake texcook` (the renderer target cooks on build)");
    REQUIRE_FALSE(content.empty());

    resonate::renderer::TextureImage image;
    std::string error;
    REQUIRE(resonate::renderer::loadKtx2Rgba8(content + "/textures/checker.ktx2", image, error));
    CHECK(image.width == 16U);
    CHECK(image.height == 16U);
    CHECK(image.srgb);
    REQUIRE(image.rgba.size() == 16U * 16U * 4U);

    const auto value = [&](std::uint32_t x, std::uint32_t y)
    { return image.rgba[(static_cast<std::size_t>(y) * 16U + x) * 4U]; };
    CHECK(value(0U, 0U) == 235U);
    CHECK(value(8U, 0U) == 120U);
    CHECK(value(0U, 8U) == 120U);
    CHECK(value(8U, 8U) == 235U);
    CHECK(image.rgba[3] == 255U);
}

TEST_CASE("ktx2: a malformed container is refused", "[render][ktx2]")
{
    resonate::renderer::TextureImage image;
    std::string error;
    CHECK_FALSE(resonate::renderer::decodeKtx2Rgba8({1, 2, 3}, image, error));
    CHECK(error.find("KTX2") != std::string::npos);
    CHECK(image.rgba.empty());
}
