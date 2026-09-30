#include <catch2/catch_all.hpp>

#include <resonate/core/allocator.h>
#include <resonate/module/capability.h>
#include <resonate/module/host.hpp>
#include <resonate/render/canvas.h>
#include <resonate/render/offscreen.hpp>

TEST_CASE("an offscreen canvas reports a size and no native target", "[render]")
{
    auto host = resonate::ModuleHost::create(resonate::systemAllocator());
    REQUIRE(host != nullptr);

    REQUIRE(resonate::render::publishOffscreen(host->capabilities(), 640U, 480U) == RESONATE_OK);

    void* instance = resonate_capability_find(
        host->capabilities(), resonate_id_make("Resonate.Render.Canvas"), 1U, nullptr);
    REQUIRE(instance != nullptr);

    auto* found = static_cast<ResonateRenderCanvas*>(instance);
    INFO("a renderer asks these instead of asking about a window");

    REQUIRE(found->width(found->self) == 640U);
    REQUIRE(found->height(found->self) == 480U);
    REQUIRE(found->is_window_backed(found->self) == 0U);
    REQUIRE(found->native_handle(found->self) == nullptr);
}

TEST_CASE("an offscreen canvas refuses a size that is not one", "[render]")
{
    auto host = resonate::ModuleHost::create(resonate::systemAllocator());
    REQUIRE(host != nullptr);

    REQUIRE(resonate::render::publishOffscreen(host->capabilities(), 0U, 480U) ==
            RESONATE_E_INVALID);
    REQUIRE(resonate::render::publishOffscreen(nullptr, 640U, 480U) == RESONATE_E_INVALID);
}
