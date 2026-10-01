#include <catch2/catch_all.hpp>

#include <string>
#include <vector>

#include <resonate/module/stage.h>

#include "fake_host.h"

namespace
{

struct Recorder
{
    std::vector<std::string>* order = nullptr;
    const char* label = "";

    static void run(void* context, float)
    {
        auto* self = static_cast<Recorder*>(context);
        self->order->emplace_back(self->label);
    }
};

ResonateSystemDesc describe(Recorder& recorder, ResonateStage stage, const char* name,
                            ResonateResourceGroup reads, ResonateResourceGroup writes)
{
    ResonateSystemDesc desc = {};
    desc.struct_size = sizeof(ResonateSystemDesc);
    desc.stage = stage;
    desc.run = &Recorder::run;
    desc.reads = reads;
    desc.writes = writes;
    desc.exclusive_stage = 0;
    desc.context = &recorder;
    desc.name = name;
    return desc;
}

constexpr ResonateResourceGroup TRANSFORMS = RESONATE_GROUP_DECLARE(0);
constexpr ResonateResourceGroup RENDER_LIST = RESONATE_GROUP_DECLARE(1);

} // namespace

TEST_CASE("stages run in order and systems run in registration order", "[module][scheduler]")
{
    resonate::test::FakeHost fake;
    ResonateScheduler* scheduler = nullptr;
    REQUIRE(resonate_scheduler_create(&scheduler, fake.api()) == RESONATE_OK);

    std::vector<std::string> order;

    Recorder late = {&order, "late"};
    Recorder early = {&order, "early"};
    Recorder early_second = {&order, "early-second"};

    /* Registered out of stage order on purpose: the stage decides, not registration. */
    ResonateSystemDesc late_desc =
        describe(late, RESONATE_STAGE_LATE_UPDATE, "late", 0, TRANSFORMS);
    ResonateSystemDesc early_desc = describe(early, RESONATE_STAGE_EARLY_UPDATE, "early", 0, 0);
    ResonateSystemDesc second_desc =
        describe(early_second, RESONATE_STAGE_EARLY_UPDATE, "early-second", 0, 0);

    REQUIRE(resonate_scheduler_add_system(scheduler, &late_desc) == RESONATE_OK);
    REQUIRE(resonate_scheduler_add_system(scheduler, &early_desc) == RESONATE_OK);
    REQUIRE(resonate_scheduler_add_system(scheduler, &second_desc) == RESONATE_OK);

    resonate_scheduler_run_frame(scheduler, 0.016F);

    REQUIRE(order.size() == 3);
    REQUIRE(order[0] == "early");
    REQUIRE(order[1] == "early-second");
    REQUIRE(order[2] == "late");

    resonate_scheduler_destroy(scheduler);
}

TEST_CASE("loading is a stage the frame loop does not run", "[module][scheduler]")
{
    resonate::test::FakeHost fake;
    ResonateScheduler* scheduler = nullptr;
    REQUIRE(resonate_scheduler_create(&scheduler, fake.api()) == RESONATE_OK);

    std::vector<std::string> order;
    Recorder loading = {&order, "loading"};
    ResonateSystemDesc desc = describe(loading, RESONATE_STAGE_LOADING, "loading", 0, 0);
    REQUIRE(resonate_scheduler_add_system(scheduler, &desc) == RESONATE_OK);

    resonate_scheduler_run_frame(scheduler, 0.016F);
    REQUIRE(order.empty());

    resonate_scheduler_run_stage(scheduler, RESONATE_STAGE_LOADING, 0.0F);
    REQUIRE(order.size() == 1);

    resonate_scheduler_destroy(scheduler);
}

TEST_CASE("a resource conflict is reported when the system is registered", "[module][scheduler]")
{
    resonate::test::FakeHost fake;
    ResonateScheduler* scheduler = nullptr;
    REQUIRE(resonate_scheduler_create(&scheduler, fake.api()) == RESONATE_OK);

    std::vector<std::string> order;
    Recorder writer = {&order, "writer"};
    Recorder reader = {&order, "reader"};
    Recorder unrelated = {&order, "unrelated"};

    ResonateSystemDesc writer_desc =
        describe(writer, RESONATE_STAGE_UPDATE, "writer", 0, TRANSFORMS);
    ResonateSystemDesc reader_desc =
        describe(reader, RESONATE_STAGE_UPDATE, "reader", TRANSFORMS, 0);
    ResonateSystemDesc unrelated_desc =
        describe(unrelated, RESONATE_STAGE_UPDATE, "unrelated", 0, RENDER_LIST);

    REQUIRE(resonate_scheduler_add_system(scheduler, &writer_desc) == RESONATE_OK);
    REQUIRE(resonate_scheduler_add_system(scheduler, &reader_desc) == RESONATE_OK);
    REQUIRE(resonate_scheduler_add_system(scheduler, &unrelated_desc) == RESONATE_OK);

    REQUIRE(fake.logged("both touch resource group"));
    REQUIRE(fake.logged("writer"));
    REQUIRE_FALSE(fake.logged("unrelated"));

    resonate_scheduler_destroy(scheduler);
}

TEST_CASE("a removed system stops running", "[module][scheduler]")
{
    resonate::test::FakeHost fake;
    ResonateScheduler* scheduler = nullptr;
    REQUIRE(resonate_scheduler_create(&scheduler, fake.api()) == RESONATE_OK);

    std::vector<std::string> order;
    Recorder recorder = {&order, "system"};
    ResonateSystemDesc desc = describe(recorder, RESONATE_STAGE_UPDATE, "system", 0, 0);
    REQUIRE(resonate_scheduler_add_system(scheduler, &desc) == RESONATE_OK);

    resonate_scheduler_run_frame(scheduler, 0.016F);
    REQUIRE(order.size() == 1);

    resonate_scheduler_remove_system(scheduler, "system");
    resonate_scheduler_run_frame(scheduler, 0.016F);
    REQUIRE(order.size() == 1);

    resonate_scheduler_destroy(scheduler);
}

TEST_CASE("exact removal takes the matching registration among same-named ones",
          "[module][scheduler]")
{
    resonate::test::FakeHost fake;
    ResonateScheduler* scheduler = nullptr;
    REQUIRE(resonate_scheduler_create(&scheduler, fake.api()) == RESONATE_OK);

    std::vector<std::string> order;
    Recorder first = {&order, "first"};
    Recorder second = {&order, "second"};
    Recorder unregistered = {&order, "unregistered"};

    /* Same name, different contexts: the name alone cannot say which one goes. */
    ResonateSystemDesc first_desc = describe(first, RESONATE_STAGE_UPDATE, "system", 0, 0);
    ResonateSystemDesc second_desc = describe(second, RESONATE_STAGE_UPDATE, "system", 0, 0);
    REQUIRE(resonate_scheduler_add_system(scheduler, &first_desc) == RESONATE_OK);
    REQUIRE(resonate_scheduler_add_system(scheduler, &second_desc) == RESONATE_OK);

    /* A triple that matches nothing removes nothing. */
    resonate_scheduler_remove_system_exact(scheduler, "system", &Recorder::run, &unregistered);
    resonate_scheduler_remove_system_exact(scheduler, "absent", &Recorder::run, &first);

    resonate_scheduler_remove_system_exact(scheduler, "system", &Recorder::run, &second);

    resonate_scheduler_run_frame(scheduler, 0.016F);
    REQUIRE(order == std::vector<std::string>{"first"});

    resonate_scheduler_destroy(scheduler);
}

TEST_CASE("a malformed descriptor is refused", "[module][scheduler]")
{
    resonate::test::FakeHost fake;
    ResonateScheduler* scheduler = nullptr;
    REQUIRE(resonate_scheduler_create(&scheduler, fake.api()) == RESONATE_OK);

    ResonateSystemDesc desc = {};
    desc.struct_size = sizeof(ResonateSystemDesc);
    desc.stage = RESONATE_STAGE_UPDATE;
    desc.name = "nameless";

    SECTION("no run function")
    {
        REQUIRE(resonate_scheduler_add_system(scheduler, &desc) == RESONATE_E_INVALID);
    }

    SECTION("a stage outside the enum")
    {
        std::vector<std::string> order;
        Recorder recorder = {&order, "system"};
        desc.run = &Recorder::run;
        desc.context = &recorder;
        desc.stage = static_cast<ResonateStage>(RESONATE_STAGE_COUNT);
        REQUIRE(resonate_scheduler_add_system(scheduler, &desc) == RESONATE_E_INVALID);
    }

    resonate_scheduler_destroy(scheduler);
}

TEST_CASE("a null name is carried as an empty label", "[module][scheduler]")
{
    resonate::test::FakeHost fake;
    ResonateScheduler* scheduler = nullptr;
    REQUIRE(resonate_scheduler_create(&scheduler, fake.api()) == RESONATE_OK);

    std::vector<std::string> order;
    Recorder recorder = {&order, "system"};
    ResonateSystemDesc desc = describe(recorder, RESONATE_STAGE_UPDATE, nullptr, 0, 0);

    REQUIRE(resonate_scheduler_add_system(scheduler, &desc) == RESONATE_OK);
    resonate_scheduler_run_frame(scheduler, 0.016F);
    REQUIRE(order.size() == 1);

    resonate_scheduler_destroy(scheduler);
}

TEST_CASE("registration keeps working past the initial capacity", "[module][scheduler]")
{
    resonate::test::FakeHost fake;
    ResonateScheduler* scheduler = nullptr;
    REQUIRE(resonate_scheduler_create(&scheduler, fake.api()) == RESONATE_OK);

    constexpr int COUNT = 40;
    std::vector<std::string> order;
    std::vector<Recorder> recorders(COUNT);
    std::vector<ResonateSystemDesc> descriptions(COUNT);

    for (int index = 0; index < COUNT; ++index)
    {
        recorders[static_cast<std::size_t>(index)] = Recorder{&order, "system"};
        descriptions[static_cast<std::size_t>(index)] = describe(
            recorders[static_cast<std::size_t>(index)], RESONATE_STAGE_UPDATE, "system", 0, 0);
        REQUIRE(resonate_scheduler_add_system(
                    scheduler, &descriptions[static_cast<std::size_t>(index)]) == RESONATE_OK);
    }

    resonate_scheduler_run_frame(scheduler, 0.016F);
    REQUIRE(order.size() == COUNT);

    resonate_scheduler_destroy(scheduler);
}
