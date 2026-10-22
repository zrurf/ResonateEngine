#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>

#include <resonate/core/allocator.h>
#include <resonate/core/span.h>
#include <resonate/csharp/managed_systems.hpp>
#include <resonate/ecs/command_buffer.h>
#include <resonate/ecs/query.h>
#include <resonate/ecs/world.h>

#include "paths.h"

/* The managed bench mirrors this layout (tests/csharp/bench/Swarm.cs); the
   name is what the self view checks against. */
struct SwarmTransform
{
    float px = 0.0F;
    float py = 0.0F;
    float pz = 0.0F;
    float rx = 0.0F;
    float ry = 0.0F;
    float rz = 0.0F;
    float rw = 1.0F;
    float sx = 1.0F;
    float sy = 1.0F;
    float sz = 1.0F;
};

namespace resonate::ecs
{
RESONATE_COMPONENT(SwarmTransform, "bench.SwarmTransform");
}

namespace
{

using resonate::ecs::ChunkView;
using resonate::ecs::CommandBuffer;
using resonate::ecs::ComponentIndex;
using resonate::ecs::Entity;
using resonate::ecs::Query;
using resonate::ecs::QueryDesc;
using resonate::ecs::World;

constexpr std::uint32_t kEntities = 100000;
constexpr int kWarmups = 12;
constexpr int kReps = 9;

/* The design's FFI budget: 100k entities through a C# Span must reach 90% of
   the same pass written in C++. */
constexpr double kFfiBar = 0.90;

double millisecondsSince(const std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() * 1e3;
}

/* The native reference does the same work as the managed entry: one dense
   write per row and the change mark the self view sends on dispose. */
struct NativePass
{
    ComponentIndex component = resonate::ecs::kInvalidComponent;
    World* world = nullptr;

    static void run(void* context, ChunkView view)
    {
        auto* self = static_cast<NativePass*>(context);
        auto* rows = static_cast<SwarmTransform*>(view.componentData(self->component));
        for (std::uint32_t row = 0; row < view.count; ++row)
        {
            rows[row].py = rows[row].px;
            self->world->markChanged(view.entities[row], self->component);
        }
    }
};

/* The swarm lane's native reference: a dense read with no change mark, the
   same pass the managed ChunkView<T> makes. */
struct NativeRead
{
    ComponentIndex component = resonate::ecs::kInvalidComponent;
    float total = 0.0F;

    static void run(void* context, ChunkView view)
    {
        auto* self = static_cast<NativeRead*>(context);
        const auto* rows = static_cast<const SwarmTransform*>(view.componentData(self->component));
        for (std::uint32_t row = 0; row < view.count; ++row)
        {
            self->total += rows[row].px;
        }
    }
};

} // namespace

int main()
{
    const std::string assets = resonate::test::findBuildDirectory("tools/csharp-bench");
    if (assets.empty())
    {
        std::printf("[csharp-bench] missing build/tools/csharp-bench; "
                    "run scripts/csharp_bench.lua\n");
        return 1;
    }

    World world(resonate::systemAllocator());
    const ComponentIndex component = world.registerComponent<SwarmTransform>();
    if (component == resonate::ecs::kInvalidComponent)
    {
        std::printf("[csharp-bench] component registration failed\n");
        return 1;
    }
    Entity first_entity{};
    for (std::uint32_t index = 0; index < kEntities; ++index)
    {
        const Entity entity = world.create();
        if (index == 0U)
        {
            first_entity = entity;
        }
        world.add(entity, SwarmTransform{static_cast<float>(index), 0.0F, 0.0F});
    }

    resonate::csharp::ManagedSystems systems;
    std::string error;
    if (!systems.load(assets + "/Resonate.Tests.Bench.runtimeconfig.json",
                      assets + "/Resonate.Tests.Bench.dll",
                      "Resonate.CSharp.Generated.BehaviourRegistration, Resonate.Tests.Bench",
                      error))
    {
        std::printf("[csharp-bench] managed bench: %s\n", error.c_str());
        return 1;
    }
    systems.set_component("bench.SwarmTransform");

    int counters[4] = {0, 0, 0, 0};
    systems.set_user(counters);

    CommandBuffer commands(world);
    const ComponentIndex all[] = {component};
    QueryDesc desc;
    desc.all = resonate::Span<const ComponentIndex>(all, 1U);
    Query query = world.createQuery(desc);

    /* The write path: the self view's Span pass with its dispose mark. */
    counters[0] = 0;
    for (int rep = 0; rep < kWarmups; ++rep)
    {
        resonate::csharp::ManagedSystems::invoke(&systems, world, commands, 0.016F);
    }
    double managed_write_ms = 1e9;
    for (int rep = 0; rep < kReps; ++rep)
    {
        const auto start = std::chrono::steady_clock::now();
        resonate::csharp::ManagedSystems::invoke(&systems, world, commands, 0.016F);
        managed_write_ms = std::min(managed_write_ms, millisecondsSince(start));
    }

    const auto* first = world.get<SwarmTransform>(first_entity);
    if (first == nullptr || first->py != first->px)
    {
        std::printf("[csharp-bench] the managed write pass did not run\n");
        return 1;
    }

    NativePass native_write{component, &world};
    for (int rep = 0; rep < kWarmups; ++rep)
    {
        query.forEachChunk(&NativePass::run, &native_write);
    }
    double native_write_ms = 1e9;
    for (int rep = 0; rep < kReps; ++rep)
    {
        const auto start = std::chrono::steady_clock::now();
        query.forEachChunk(&NativePass::run, &native_write);
        native_write_ms = std::min(native_write_ms, millisecondsSince(start));
    }

    /* The design's bar: the swarm lane's read-only iteration. */
    counters[0] = 1;
    for (int rep = 0; rep < kWarmups; ++rep)
    {
        resonate::csharp::ManagedSystems::invoke(&systems, world, commands, 0.016F);
    }
    double managed_read_ms = 1e9;
    for (int rep = 0; rep < kReps; ++rep)
    {
        const auto start = std::chrono::steady_clock::now();
        resonate::csharp::ManagedSystems::invoke(&systems, world, commands, 0.016F);
        managed_read_ms = std::min(managed_read_ms, millisecondsSince(start));
    }

    NativeRead native_read{component};
    for (int rep = 0; rep < kWarmups; ++rep)
    {
        query.forEachChunk(&NativeRead::run, &native_read);
    }
    double native_read_ms = 1e9;
    for (int rep = 0; rep < kReps; ++rep)
    {
        const auto start = std::chrono::steady_clock::now();
        query.forEachChunk(&NativeRead::run, &native_read);
        native_read_ms = std::min(native_read_ms, millisecondsSince(start));
    }

    if (counters[1] == 0 || native_read.total <= 0.0F)
    {
        std::printf("[csharp-bench] the managed read pass did not run\n");
        return 1;
    }

    const double write_ratio = native_write_ms / managed_write_ms;
    const double read_ratio = native_read_ms / managed_read_ms;
    std::printf("[csharp-bench] write path managed %.4f ms, native %.4f ms, ratio %.4f\n",
                managed_write_ms, native_write_ms, write_ratio);
    std::printf("[csharp-bench] read path  managed %.4f ms, native %.4f ms, ratio %.4f "
                "(bar %.2f) %s\n",
                managed_read_ms, native_read_ms, read_ratio, kFfiBar,
                read_ratio >= kFfiBar ? "PASS" : "FAIL");
    return read_ratio >= kFfiBar ? 0 : 1;
}
