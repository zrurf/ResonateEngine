#include <catch2/catch_test_macros.hpp>

#include <resonate/renderer/renderer.h>

#include <string>

namespace
{

using resonate::renderer::FeatureStack;

} // namespace

TEST_CASE("feature stack: the defaults keep the full post chain", "[render][features]")
{
    const FeatureStack stack = FeatureStack::defaults();
    std::string error;
    CHECK(resonate::renderer::validateFeatureStack(stack, error));
    CHECK(stack.has("bloom"));
    CHECK(stack.has("tonemap"));
    CHECK(stack.antialiases());
    CHECK(stack.bloom_threshold == 1.0F);
}

TEST_CASE("feature stack: the chain comes from the configured names", "[render][features]")
{
    FeatureStack stack;
    std::string error;

    REQUIRE(resonate::renderer::parseFeatureStack("bloom, tonemap", "fxaa", stack, error));
    CHECK(stack.chain.size() == 2U);
    CHECK(stack.has("bloom"));
    CHECK(stack.antialiases());

    REQUIRE(resonate::renderer::parseFeatureStack("tonemap", "none", stack, error));
    CHECK_FALSE(stack.has("bloom"));
    CHECK_FALSE(stack.antialiases());

    /* An empty AA name takes the default slot value, like an absent key. */
    REQUIRE(resonate::renderer::parseFeatureStack("  tonemap  ", "", stack, error));
    CHECK(stack.chain.size() == 1U);
    CHECK(stack.antialiases());
}

TEST_CASE("feature stack: bad data is refused with the offending entry", "[render][features]")
{
    FeatureStack stack;
    std::string error;

    CHECK_FALSE(resonate::renderer::parseFeatureStack("tonemap, vignette", "fxaa", stack, error));
    CHECK(error.find("vignette") != std::string::npos);

    CHECK_FALSE(resonate::renderer::parseFeatureStack("tonemap, tonemap", "fxaa", stack, error));
    CHECK(error.find("twice") != std::string::npos);

    CHECK_FALSE(resonate::renderer::parseFeatureStack("tonemap, bloom", "fxaa", stack, error));
    CHECK(error.find("precede") != std::string::npos);

    CHECK_FALSE(resonate::renderer::parseFeatureStack("bloom", "fxaa", stack, error));
    CHECK(error.find("tonemap") != std::string::npos);

    CHECK_FALSE(resonate::renderer::parseFeatureStack("bloom, tonemap", "taa", stack, error));
    CHECK(error.find("taa") != std::string::npos);

    /* A hand-built stack goes through the same rules. */
    stack = FeatureStack::defaults();
    stack.antialiasing.clear();
    CHECK_FALSE(resonate::renderer::validateFeatureStack(stack, error));
    CHECK(error.find("antialiasing") != std::string::npos);
}
