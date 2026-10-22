#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include <resonate/core/allocator.h>
#include <resonate/csharp/managed_systems.hpp>
#include <resonate/ecs/command_buffer.h>
#include <resonate/ecs/world.h>

#include "paths.h"

/* The component the patch adds. The patch carries its own C# mirror, so a new
   entity composition is created at runtime without a base-assembly change. */
struct ReloadEntity
{
    float value = 0.0F;
};

namespace resonate::ecs
{
RESONATE_COMPONENT(ReloadEntity, "bench.ReloadEntity");
}

namespace
{

using resonate::ecs::CommandBuffer;
using resonate::ecs::Entity;
using resonate::ecs::World;

/* The acceptance bar: write code, rebuild, swap the patch in, see the new
   behaviour. The rebuild is an incremental dotnet build of one small file. */
constexpr double kReloadBarMs = 2000.0;
constexpr int kIterations = 5;

/* The bootstrap resolves the type inside the patch assembly, so the name is
   unqualified. */
constexpr const char* kPatchType = "Resonate.CSharp.Generated.BehaviourRegistration";
constexpr const char* kBootstrapType =
    "Resonate.Tests.Bootstrap.PatchHost, Resonate.Tests.Bootstrap";

double millisecondsSince(const std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() * 1e3;
}

bool writeSource(const std::string& path, const int version)
{
    std::ofstream source(path, std::ios::binary | std::ios::trunc);
    if (!source)
    {
        return false;
    }
    source << "using Resonate.CSharp;\n"
              "using Resonate.CSharp.Bindings;\n"
              "\n"
              "[assembly: ResonateBehaviour]\n"
              "\n"
              "namespace Resonate.Tests.Reload;\n"
              "\n"
              "public struct ReloadEntity\n"
              "{\n"
              "    public float Value;\n"
              "}\n"
              "\n"
              "public static unsafe class PatchBehaviour\n"
              "{\n"
              "    [UpdateIn(ResonateStage.Update)]\n"
              "    public static void Tick(SystemContext* frame)\n"
              "    {\n"
              "        int* counters = (int*)frame->User;\n"
              "        counters[1] = "
           << version
           << ";\n"
              "        Commands commands = Commands.For(frame);\n"
              "        if (commands.Spawn(out EntityHandle entity) &&\n"
              "            commands.Add(entity, \"bench.ReloadEntity\"u8,\n"
              "                         new ReloadEntity { Value = "
           << version
           << " }))\n"
              "        {\n"
              "            counters[2] = (int)entity.Index;\n"
              "            counters[3] = (int)entity.Generation;\n"
              "        }\n"
              "    }\n"
              "}\n";
    return source.good();
}

} // namespace

int main()
{
    const std::string fixture = resonate::test::findBuildDirectory("tools/csharp-test");
    const std::string bootstrap = resonate::test::findBuildDirectory("tools/csharp-bootstrap");
    const std::string reload = resonate::test::findBuildDirectory("tools/csharp-reload");
    if (fixture.empty() || bootstrap.empty() || reload.empty())
    {
        std::printf("[csharp-reload] missing fixtures; run scripts/csharp_reload.lua\n");
        return 1;
    }

    const std::string project = reload + "/Reload.Patch.csproj";
    const std::string source = reload + "/Behaviour.cs";
    const std::string output = reload + "/out";
    const std::string patch = output + "/Resonate.Tests.Reload.Patch.dll";

    resonate::csharp::ManagedSystems systems;
    std::string error;
    if (!systems.load(fixture + "/Resonate.Tests.Managed.runtimeconfig.json",
                      fixture + "/Resonate.Tests.Managed.dll",
                      "Resonate.CSharp.Generated.BehaviourRegistration, Resonate.Tests.Managed",
                      error))
    {
        std::printf("[csharp-reload] base assembly: %s\n", error.c_str());
        return 1;
    }

    World world(resonate::systemAllocator());
    if (world.registerComponent<ReloadEntity>() == resonate::ecs::kInvalidComponent)
    {
        std::printf("[csharp-reload] cannot register bench.ReloadEntity\n");
        return 1;
    }
    CommandBuffer commands(world);
    int counters[4] = {0, -1, 0, 0};
    systems.set_user(counters);

    std::vector<double> times;
    times.reserve(kIterations);

    const std::string build = "dotnet build \"" + project + "\" -c Debug -o \"" + output +
                              "\" --nologo -v quiet";

    /* One untimed build warms the compiler server, the generator and the
       analyzer: those start once per session, not once per edit, and charging
       them to the first edit would measure the toolchain's start-up. */
    if (!writeSource(source, 0))
    {
        std::printf("[csharp-reload] cannot write %s\n", source.c_str());
        return 1;
    }
    if (std::system(build.c_str()) != 0)
    {
        std::printf("[csharp-reload] warm-up build failed\n");
        return 1;
    }

    for (int version = 1; version <= kIterations; ++version)
    {
        const auto start = std::chrono::steady_clock::now();

        if (!writeSource(source, version + 1))
        {
            std::printf("[csharp-reload] cannot write %s\n", source.c_str());
            return 1;
        }
        /* Unload before the rebuild: the ALC holds the old DLL open. */
        if (systems.patched() && !systems.unload_patch(error))
        {
            std::printf("[csharp-reload] unload: %s\n", error.c_str());
            return 1;
        }

        const int status = std::system(build.c_str());
        if (status != 0)
        {
            std::printf("[csharp-reload] dotnet build failed (%d)\n", status);
            return 1;
        }

        if (!systems.load_patch(bootstrap + "/Resonate.Tests.Bootstrap.dll", kBootstrapType,
                                patch, kPatchType, error))
        {
            std::printf("[csharp-reload] install: %s\n", error.c_str());
            return 1;
        }

        counters[1] = -1;
        counters[2] = 0;
        counters[3] = 0;
        resonate::csharp::ManagedSystems::invoke(&systems, world, commands, 0.016F);
        /* The new entity composition becomes alive at the frame's sync point,
           which is part of "visible". */
        world.play(commands);
        const double elapsed = millisecondsSince(start);
        const int expected = version + 1;
        if (counters[1] != expected)
        {
            std::printf("[csharp-reload] version %d is not visible (counter %d)\n", expected,
                        counters[1]);
            return 1;
        }
        const Entity spawned{static_cast<std::uint32_t>(counters[2]),
                             static_cast<std::uint32_t>(counters[3])};
        const auto* added = world.get<ReloadEntity>(spawned);
        if (added == nullptr || added->value != static_cast<float>(expected))
        {
            std::printf("[csharp-reload] version %d did not spawn its entity\n", expected);
            return 1;
        }
        times.push_back(elapsed);
        std::printf("[csharp-reload] version %d: %.0f ms\n", expected, elapsed);
    }

    std::vector<double> sorted = times;
    std::sort(sorted.begin(), sorted.end());
    const double median = sorted[sorted.size() / 2U];
    const double worst = sorted.back();
    std::printf("[csharp-reload] median %.0f ms, worst %.0f ms (bar %.0f ms) %s\n", median,
                worst, kReloadBarMs, median < kReloadBarMs ? "PASS" : "FAIL");
    return median < kReloadBarMs ? 0 : 1;
}
