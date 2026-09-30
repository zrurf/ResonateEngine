#include <catch2/catch_all.hpp>

#include <string>
#include <vector>

#include <resonate/core/allocator.h>
#include <resonate/module/host.hpp>
#include <resonate/module/module.hpp>
#include <resonate/pal/io.h>
#include <resonate/window/host.hpp>

#include "fake_host.h"
#include "paths.h"

namespace
{

/* What the host reclaimed is only observable in its log. */
struct LogCollector
{
    std::vector<std::string> lines;

    static void sink(void* user_data, int32_t, const char*, int32_t, const char* message)
    {
        static_cast<LogCollector*>(user_data)->lines.emplace_back(message != nullptr ? message
                                                                                     : "");
    }

    [[nodiscard]] bool contains(const char* fragment) const
    {
        for (const std::string& line : lines)
        {
            if (line.find(fragment) != std::string::npos)
            {
                return true;
            }
        }
        return false;
    }
};

template <typename Capability> ResonateCapabilityRecord recordFor(Capability& instance)
{
    ResonateCapabilityRecord record = {};
    record.id = resonate::detail::CapabilityTraits<Capability>::id.value();
    record.version = resonate::detail::CapabilityTraits<Capability>::version;
    record.instance = &instance;
    record.struct_size = instance.header.struct_size;
    record.name = resonate::detail::CapabilityTraits<Capability>::name;
    return record;
}

bool provides(const std::vector<resonate::ModuleRecord*>& order, const char* capability_name)
{
    const ResonateId id = resonate_id_make(capability_name);
    for (const resonate::ModuleRecord* record : order)
    {
        for (const resonate::ModuleCapability& provided : record->manifest.provides)
        {
            if (provided.id.value() == id)
            {
                return true;
            }
        }
    }
    return false;
}

std::size_t positionOf(const std::vector<resonate::ModuleRecord*>& order, const char* module_id)
{
    for (std::size_t index = 0; index < order.size(); ++index)
    {
        if (order[index]->manifest.id == module_id)
        {
            return index;
        }
    }
    return order.size();
}

int frames = 0;

void countFrame(void*, float)
{
    ++frames;
}

resonate::ModuleRecord makeRecord(const char* id, std::vector<std::string> depends_on)
{
    resonate::ModuleRecord record;
    record.manifest.id = id;
    record.manifest.name = id;
    record.manifest.version = "0.1.0";
    record.manifest.depends_on = std::move(depends_on);
    return record;
}

} // namespace

TEST_CASE("a manifest is read into the fields the loader needs", "[module][loader]")
{
    const char* text = R"({
      "id": "resonate.test",
      "name": "Test",
      "version": "1.2.3",
      "abi": 1,
      "min_host_abi": 1,
      "summary": "A manifest used by a test.",
      "provides": ["Resonate.Test.One"],
      "requires": ["Resonate.Test.Two"],
      "optional": ["Resonate.Test.Three", "Resonate.Test.Four"],
      "depends_on": ["resonate.other"]
    })";

    resonate::ModuleManifest manifest;
    REQUIRE(resonate::parseManifest(text, manifest) == RESONATE_OK);
    REQUIRE(manifest.id == "resonate.test");
    REQUIRE(manifest.name == "Test");
    REQUIRE(manifest.version == "1.2.3");
    REQUIRE(manifest.abi == 1U);
    REQUIRE(manifest.min_host_abi == 1U);
    REQUIRE(manifest.provides.size() == 1U);
    REQUIRE(manifest.provides[0].id.value() == resonate_id_make("Resonate.Test.One"));
    /* Kept so a failure can name the capability instead of its hash. */
    REQUIRE(manifest.provides[0].name == "Resonate.Test.One");
    REQUIRE(manifest.requirements.size() == 1U);
    REQUIRE(manifest.optional.size() == 2U);
    REQUIRE(manifest.depends_on.size() == 1U);
    REQUIRE(manifest.depends_on[0] == "resonate.other");
}

TEST_CASE("a manifest with a missing or malformed field is refused", "[module][loader]")
{
    resonate::ModuleManifest manifest;

    REQUIRE(resonate::parseManifest("{ \"id\": \"resonate.test\" }", manifest) ==
            RESONATE_E_INVALID);
    REQUIRE(resonate::parseManifest("not json at all", manifest) == RESONATE_E_INVALID);
    REQUIRE(resonate::parseManifest("[]", manifest) == RESONATE_E_INVALID);

    /* abi as a string is the mistake a hand-written manifest makes. */
    REQUIRE(resonate::parseManifest(
                R"({"id":"a","name":"b","version":"1","abi":"1","min_host_abi":1,
                    "provides":[],"requires":[],"optional":[],"depends_on":[]})",
                manifest) == RESONATE_E_INVALID);
}

TEST_CASE("the graph orders a module after whatever it depends on", "[module][loader]")
{
    SECTION("depends_on")
    {
        std::vector<resonate::ModuleRecord> records;
        records.push_back(makeRecord("resonate.a", {}));
        records.push_back(makeRecord("resonate.c", {"resonate.b"}));
        records.push_back(makeRecord("resonate.b", {"resonate.a"}));

        resonate::ModuleGraph graph;
        REQUIRE(graph.build(records) == RESONATE_OK);
        REQUIRE(graph.order().size() == 3U);
        REQUIRE(graph.order()[0]->manifest.id == "resonate.a");
        REQUIRE(graph.order()[1]->manifest.id == "resonate.b");
        REQUIRE(graph.order()[2]->manifest.id == "resonate.c");
    }

    SECTION("a required capability orders the provider first")
    {
        std::vector<resonate::ModuleRecord> records;
        records.push_back(makeRecord("resonate.consumer", {}));
        records.push_back(makeRecord("resonate.provider", {}));
        const resonate::ModuleCapability widget{"Resonate.Test.Widget",
                                                resonate::Id("Resonate.Test.Widget")};
        records[0].manifest.requirements.push_back(widget);
        records[1].manifest.provides.push_back(widget);

        resonate::ModuleGraph graph;
        REQUIRE(graph.build(records) == RESONATE_OK);
        REQUIRE(graph.order()[0]->manifest.id == "resonate.provider");
        REQUIRE(graph.order()[1]->manifest.id == "resonate.consumer");
    }

    SECTION("a cycle behind an emittable module is reported, not walked off the end")
    {
        /* resonate.a can be emitted on its own, so it is no longer part of the
           cycle; a report that followed it would ask an empty prerequisite list
           for its first entry. */
        std::vector<resonate::ModuleRecord> records;
        records.push_back(makeRecord("resonate.a", {}));
        records.push_back(makeRecord("resonate.b", {"resonate.a", "resonate.c"}));
        records.push_back(makeRecord("resonate.c", {"resonate.b"}));

        resonate::ModuleGraph graph;
        REQUIRE(graph.build(records) == RESONATE_E_INVALID);
        REQUIRE(graph.order().empty());
        REQUIRE(graph.error().find("resonate.b") != std::string::npos);
        REQUIRE(graph.error().find("resonate.c") != std::string::npos);
    }

    SECTION("a cycle is refused and named")
    {
        std::vector<resonate::ModuleRecord> records;
        records.push_back(makeRecord("resonate.a", {"resonate.b"}));
        records.push_back(makeRecord("resonate.b", {"resonate.a"}));

        resonate::ModuleGraph graph;
        REQUIRE(graph.build(records) == RESONATE_E_INVALID);
        REQUIRE(graph.order().empty());
        REQUIRE(graph.error().find("resonate.a") != std::string::npos);
        REQUIRE(graph.error().find("resonate.b") != std::string::npos);
    }

    SECTION("a dependency that is not present is refused")
    {
        std::vector<resonate::ModuleRecord> records;
        records.push_back(makeRecord("resonate.a", {"resonate.absent"}));

        resonate::ModuleGraph graph;
        REQUIRE(graph.build(records) == RESONATE_E_MISSING);
        REQUIRE(graph.error().find("resonate.absent") != std::string::npos);
    }
}

TEST_CASE("discovery reports a directory it cannot read", "[module][loader]")
{
    auto host = resonate::ModuleHost::create(resonate::systemAllocator());
    REQUIRE(host != nullptr);

    REQUIRE(host->discover("no_such_plugin_directory") == RESONATE_E_MISSING);
    REQUIRE(host->records().empty());
}

TEST_CASE("the host loads the built plugins and runs one frame", "[module][loader]")
{
    /* This target depends on the plugins being built, so a directory that is not
       there is a build problem rather than a reason to pass without checking. */
    const std::string plugin_directory = resonate::test::findBuildDirectory("plugins");
    INFO("the plugin targets are a build dependency of this test target");
    REQUIRE_FALSE(plugin_directory.empty());

    auto host = resonate::ModuleHost::create(resonate::systemAllocator());
    REQUIRE(host != nullptr);

    /* The display belongs to the host, not to a module, and the modules that draw
       require the canvas. Both are registered without a window existing: what they
       check is that the capability resolves, and the window-backed canvas reports
       a zero size until there is one. */
    ResonateWindow& window = resonate::window::windowCapability();
    ResonateRenderCanvas& canvas = resonate::window::windowCanvas();
    const ResonateCapabilityRecord window_record = recordFor(window);
    const ResonateCapabilityRecord canvas_record = recordFor(canvas);
    REQUIRE(resonate_capability_register(host->capabilities(), &window_record) == RESONATE_OK);
    REQUIRE(resonate_capability_register(host->capabilities(), &canvas_record) == RESONATE_OK);

    REQUIRE(host->discover(plugin_directory) == RESONATE_OK);
    REQUIRE(host->records().size() == 4U);

    REQUIRE(host->resolve() == RESONATE_OK);

    /* ui depends on render, so it attaches after it. */
    const std::vector<resonate::ModuleRecord*>& order = host->order();
    REQUIRE(order.size() == 4U);
    REQUIRE(positionOf(order, "resonate.render") < positionOf(order, "resonate.ui"));
    REQUIRE(provides(order, "Resonate.UI.Context"));

    /* A system on the host's scheduler runs as part of a frame. */
    ResonateSystemDesc system = {};
    system.struct_size = sizeof(ResonateSystemDesc);
    system.stage = RESONATE_STAGE_UPDATE;
    system.run = &countFrame;
    system.name = "test.frame";
    REQUIRE(resonate_scheduler_add_system(host->scheduler(), &system) == RESONATE_OK);

    REQUIRE(host->attachAll() == RESONATE_OK);
    for (const resonate::ModuleRecord& record : host->records())
    {
        INFO(record.manifest.id);
        REQUIRE(record.attached);
    }

    /* Every capability the modules declared is now published by the host. */
    for (const char* capability : {"Resonate.Physics.World", "Resonate.Audio.Device",
                                   "Resonate.Render.Device", "Resonate.UI.Context"})
    {
        INFO(capability);
        REQUIRE(resonate_capability_find(host->capabilities(), resonate_id_make(capability), 1U,
                                         nullptr) != nullptr);
    }

    frames = 0;
    host->runFrame(1.0F / 60.0F);
    REQUIRE(frames == 1);

    /* Detach withdraws what the modules published, and leaves the host able to
       load them again. */
    host->detachAll();
    for (const resonate::ModuleRecord& record : host->records())
    {
        INFO(record.manifest.id);
        REQUIRE_FALSE(record.attached);
    }
    for (const char* capability : {"Resonate.Physics.World", "Resonate.Audio.Device",
                                   "Resonate.Render.Device", "Resonate.UI.Context"})
    {
        INFO(capability);
        REQUIRE(resonate_capability_find(host->capabilities(), resonate_id_make(capability), 1U,
                                         nullptr) == nullptr);
    }
    REQUIRE(resonate_capability_find(host->capabilities(), resonate_id_make("Resonate.Window"), 1U,
                                     nullptr) == &window);
}

TEST_CASE("a module that fails to attach leaves nothing of its own behind", "[module][loader]")
{
    const std::string plugin_directory = resonate::test::findBuildDirectory("failing-plugins");
    INFO("the failing module is a build dependency of this test target");
    REQUIRE_FALSE(plugin_directory.empty());

    auto host = resonate::ModuleHost::create(resonate::systemAllocator());
    REQUIRE(host != nullptr);

    LogCollector log;
    host->setLogSink(&log, &LogCollector::sink);

    REQUIRE(host->discover(plugin_directory) == RESONATE_OK);
    REQUIRE(host->records().size() == 1U);
    REQUIRE(host->resolve() == RESONATE_OK);

    /* The module fails on purpose, after registering a capability, creating a
       signal and allocating memory. */
    REQUIRE(host->attachAll() == RESONATE_E_INTERNAL);
    REQUIRE_FALSE(host->records()[0].attached);

    /* All three were withdrawn, which is what the log reports. */
    REQUIRE(log.contains("left capability"));
    REQUIRE(log.contains("left a signal alive through detach"));
    REQUIRE(log.contains("allocated through detach"));

    /* And the withdrawal is what makes a retry a retry rather than a collision:
       the same module taking the same id again has to be able to. */
    log.lines.clear();
    REQUIRE(host->attachAll() == RESONATE_E_INTERNAL);
    REQUIRE_FALSE(log.contains("already registered"));
    REQUIRE(log.contains("left capability"));
}

TEST_CASE("a plugin uses a signal and a message stream, and the host reclaims what it leaves",
          "[module][loader]")
{
    /* Built as a dependency of this target, like the shipped plugins. */
    const std::string plugin_directory = resonate::test::findBuildDirectory("probe-plugins");
    INFO("the probe module is a build dependency of this test target");
    REQUIRE_FALSE(plugin_directory.empty());

    auto host = resonate::ModuleHost::create(resonate::systemAllocator());
    REQUIRE(host != nullptr);

    LogCollector log;
    host->setLogSink(&log, &LogCollector::sink);

    REQUIRE(host->discover(plugin_directory) == RESONATE_OK);
    REQUIRE(host->records().size() == 1U);

    /* The probe reports what it exercised by failing to attach, so a green
       attach is the assertion: its signal delivered and its stream round-tripped,
       through entry points that are on the host API rather than in its own
       library's link line. */
    REQUIRE(host->resolve() == RESONATE_OK);
    REQUIRE(host->attachAll() == RESONATE_OK);

    /* Cleared before the loop, so it is not the assertion below passing on
       something else. */
    log.lines.clear();

    host->detachAll();

    REQUIRE(log.contains("left a signal alive through detach"));
    REQUIRE(log.contains("left a message writer alive through detach"));
    REQUIRE(log.contains("allocated through detach"));

    /* Unloading the library destroys the module's own members, and those release
       storage the host has already reclaimed. The tracking turns that into a
       report instead of a second free. */
    log.lines.clear();
    host.reset();
    REQUIRE(log.contains("destroyed a signal it does not hold"));
    REQUIRE(log.contains("released memory it does not hold"));
}
