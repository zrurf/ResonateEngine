#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include <resonate/core/allocator.h>
#include <resonate/dom/document.h>
#include <resonate/ecs/world.h>
#include <resonate/hierarchy/tree.h>
#include <resonate/ui/binding.h>
#include <resonate/ui/document.h>
#include <resonate/ui/document_schema.h>
#include <resonate/ui/layout.h>

#include "resonate.ui/components.gen.h"

namespace
{

using resonate::ecs::Entity;
using resonate::ecs::World;
using resonate::ui::Bindings;
using resonate::ui::ComputedRect;
using resonate::ui::DataContext;
using resonate::ui::RectTransform;

const char* const kBound = R"xml(<document version="1" schemas="resonate.ui">
  <Screen key="hud" health="120" max_health="200" ammo="30" max_ammo="60">
    <Panel key="panel" x="0" y="0" width="300" height="100"
           direction="Column" align="Start" gap="4">
      <Label key="health" height="10" bind-width="DataContext.health"/>
      <Label key="ammo" height="10" bind-width="DataContext.ammo"/>
    </Panel>
  </Screen>
</document>)xml";

struct Fixture
{
    World world{resonate::systemAllocator()};

    Fixture()
    {
        REQUIRE(resonate::hierarchy::registerComponents(world));
        REQUIRE(resonate::ui::registerComponents(world));
    }
};

} // namespace

TEST_CASE("ui binding: a canvas DataContext drives bound widget fields", "[ui][binding]")
{
    std::vector<resonate::dom::Diagnostic> diagnostics;
    auto document =
        resonate::dom::Document::parse(kBound, &resonate::ui::documentSchema(), diagnostics);
    REQUIRE(document != nullptr);
    CHECK(diagnostics.empty());

    Fixture fixture;
    const Entity root = resonate::ui::instantiate(fixture.world, *document);
    REQUIRE(root.valid());
    const Bindings* bindings = fixture.world.get<Bindings>(root);
    REQUIRE(bindings != nullptr);
    CHECK(fixture.world.alive(bindings->records));

    const auto panel = resonate::hierarchy::children(fixture.world, root)[0];
    const auto labels = resonate::hierarchy::children(fixture.world, panel);
    REQUIRE(labels.size() == 2U);
    CHECK(fixture.world.get<RectTransform>(labels[0])->width == 0.0F);

    resonate::ui::updateBindings(fixture.world, root);
    CHECK(fixture.world.get<RectTransform>(labels[0])->width == 120.0F);
    CHECK(fixture.world.get<RectTransform>(labels[1])->width == 30.0F);

    REQUIRE(resonate::ui::layout(fixture.world, root, 300.0F, 100.0F));
    const ComputedRect* health_rect = fixture.world.get<ComputedRect>(labels[0]);
    REQUIRE(health_rect != nullptr);
    CHECK(health_rect->width == 120.0F);
    CHECK(fixture.world.get<ComputedRect>(labels[1])->width == 30.0F);

    /* The next frame's source value reaches the target and layout. */
    DataContext* data = fixture.world.get<DataContext>(root);
    REQUIRE(data != nullptr);
    data->health = 200.0F;
    resonate::ui::updateBindings(fixture.world, root);
    CHECK(fixture.world.get<RectTransform>(labels[0])->width == 200.0F);
    REQUIRE(resonate::ui::layout(fixture.world, root, 300.0F, 100.0F));
    CHECK(fixture.world.get<ComputedRect>(labels[0])->width == 200.0F);
}

TEST_CASE("ui binding: malformed expressions and explicit conflicts are refused",
          "[ui][binding]")
{
    const char* const invalid[] = {
        R"xml(<document version="1"><Screen><Panel bind-width="Health.current"/></Screen></document>)xml",
        R"xml(<document version="1"><Screen><Panel bind-width="DataContext.missing"/></Screen></document>)xml",
        R"xml(<document version="1"><Screen><Panel width="10" bind-width="DataContext.health"/></Screen></document>)xml",
        R"xml(<document version="1"><Screen><Bindings/></Screen></document>)xml",
    };

    for (const char* text : invalid)
    {
        std::vector<resonate::dom::Diagnostic> diagnostics;
        auto document =
            resonate::dom::Document::parse(text, &resonate::ui::documentSchema(), diagnostics);
        if (document == nullptr)
        {
            CHECK_FALSE(diagnostics.empty());
            continue;
        }
        Fixture fixture;
        CHECK_FALSE(resonate::ui::instantiate(fixture.world, *document).valid());
    }
}
