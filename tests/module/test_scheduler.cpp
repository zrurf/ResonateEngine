#include <catch2/catch_all.hpp>

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include <resonate/module/stage.h>
#include <resonate/pal/sync.h>
#include <resonate/pal/thread.h>

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

ResonateSystemDesc describeRun(ResonateSystemFn run, void* context, ResonateStage stage,
                               const char* name, ResonateResourceGroup reads,
                               ResonateResourceGroup writes)
{
    ResonateSystemDesc desc = {};
    desc.struct_size = sizeof(ResonateSystemDesc);
    desc.stage = stage;
    desc.run = run;
    desc.reads = reads;
    desc.writes = writes;
    desc.exclusive_stage = 0;
    desc.context = context;
    desc.name = name;
    return desc;
}

ResonateSystemDesc describe(Recorder& recorder, ResonateStage stage, const char* name,
                            ResonateResourceGroup reads, ResonateResourceGroup writes)
{
    return describeRun(&Recorder::run, &recorder, stage, name, reads, writes);
}

constexpr ResonateResourceGroup TRANSFORMS = RESONATE_GROUP_DECLARE(0);
constexpr ResonateResourceGroup RENDER_LIST = RESONATE_GROUP_DECLARE(1);
constexpr ResonateResourceGroup AUDIO_MIX = RESONATE_GROUP_DECLARE(2);

/* Records the worst number of bodies ever inside at once and stamps a completion
   rank, so a test can tell serialisation from order alone. Atomics only: these
   bodies run on the pool's threads. */
struct Racer
{
    std::atomic<std::uint32_t>* next = nullptr;
    std::atomic<std::uint32_t>* rank = nullptr;
    std::atomic<std::uint32_t>* inside = nullptr;
    std::atomic<std::uint32_t>* worst = nullptr;
    std::uint32_t spins = 0;

    static void run(void* context, float)
    {
        auto* self = static_cast<Racer*>(context);
        const std::uint32_t now = self->inside->fetch_add(1) + 1;
        std::uint32_t worst = self->worst->load(std::memory_order_relaxed);
        while (worst < now &&
               !self->worst->compare_exchange_weak(worst, now, std::memory_order_relaxed))
        {
        }

        for (std::uint32_t spin = 0; spin < self->spins; ++spin)
        {
            resonate_pal_thread_yield();
        }

        self->inside->fetch_sub(1);
        self->rank->store(self->next->fetch_add(1, std::memory_order_relaxed),
                          std::memory_order_relaxed);
    }
};

/* Two bodies that wait for each other inside, so the pair completes only if both
   were running at the same time; the bounded spin reports failure instead of
   hanging if the schedule never lets them meet. */
struct Rendezvous
{
    std::atomic<std::uint32_t> enteredA{0};
    std::atomic<std::uint32_t> enteredB{0};
    std::atomic<bool> sawEachOther{false};

    static void first(void* context, float)
    {
        pair(context, true);
    }

    static void second(void* context, float)
    {
        pair(context, false);
    }

  private:
    static void pair(void* context, bool is_first)
    {
        auto* self = static_cast<Rendezvous*>(context);
        std::atomic<std::uint32_t>& entered = is_first ? self->enteredA : self->enteredB;
        std::atomic<std::uint32_t>& other = is_first ? self->enteredB : self->enteredA;

        entered.fetch_add(1);
        for (int spin = 0; spin < 2000000 && other.load() == 0; ++spin)
        {
            resonate_pal_thread_yield();
        }
        if (other.load() != 0)
        {
            self->sawEachOther.store(true);
        }
        entered.fetch_sub(1);
    }
};

/* Burns its spins, then marks itself finished. */
struct SlowBody
{
    std::atomic<bool> done{false};
    std::uint32_t spins = 0;

    static void run(void* context, float)
    {
        auto* self = static_cast<SlowBody*>(context);
        for (std::uint32_t spin = 0; spin < self->spins; ++spin)
        {
            resonate_pal_thread_yield();
        }
        self->done.store(true);
    }
};

} // namespace

TEST_CASE("stages run in order, and systems that declare no group keep registration order",
          "[module][scheduler]")
{
    resonate::test::FakeHost fake;
    ResonateScheduler* scheduler = nullptr;
    REQUIRE(resonate_scheduler_create(&scheduler, fake.api()) == RESONATE_OK);

    /* Declaring no group claims nothing, so these three are serialised in
       submission order even though they share nothing: were the two EARLY_UPDATE
       bodies allowed to run together, the overlap watermark would show it. */
    std::atomic<std::uint32_t> inside{0};
    std::atomic<std::uint32_t> worst{0};
    std::atomic<std::uint32_t> next{0};
    std::atomic<std::uint32_t> ranks[3] = {};
    Racer racers[3] = {{&next, &ranks[0], &inside, &worst, 20000},
                       {&next, &ranks[1], &inside, &worst, 20000},
                       {&next, &ranks[2], &inside, &worst, 0}};

    /* Registered out of stage order on purpose: the stage decides, not registration. */
    ResonateSystemDesc late =
        describeRun(&Racer::run, &racers[2], RESONATE_STAGE_LATE_UPDATE, "late", 0, TRANSFORMS);
    ResonateSystemDesc early =
        describeRun(&Racer::run, &racers[0], RESONATE_STAGE_EARLY_UPDATE, "early", 0, 0);
    ResonateSystemDesc second =
        describeRun(&Racer::run, &racers[1], RESONATE_STAGE_EARLY_UPDATE, "early-second", 0, 0);

    REQUIRE(resonate_scheduler_add_system(scheduler, &late) == RESONATE_OK);
    REQUIRE(resonate_scheduler_add_system(scheduler, &early) == RESONATE_OK);
    REQUIRE(resonate_scheduler_add_system(scheduler, &second) == RESONATE_OK);

    resonate_scheduler_run_frame(scheduler, 0.016F);

    REQUIRE(worst.load() == 1);
    for (std::uint32_t index = 0; index < 3U; ++index)
    {
        REQUIRE(ranks[index].load() == index);
    }

    resonate_scheduler_destroy(scheduler);
}

TEST_CASE("systems that share a group run one at a time in registration order",
          "[module][scheduler]")
{
    resonate::test::FakeHost fake;
    ResonateScheduler* scheduler = nullptr;
    REQUIRE(resonate_scheduler_create(&scheduler, fake.api()) == RESONATE_OK);

    constexpr std::uint32_t COUNT = 4;
    std::atomic<std::uint32_t> inside{0};
    std::atomic<std::uint32_t> worst{0};
    std::atomic<std::uint32_t> next{0};
    std::atomic<std::uint32_t> ranks[COUNT] = {};
    Racer racers[COUNT] = {};
    ResonateSystemDesc descs[COUNT] = {};

    for (std::uint32_t index = 0; index < COUNT; ++index)
    {
        racers[index] = Racer{&next, &ranks[index], &inside, &worst, 20000};
        descs[index] =
            describeRun(&Racer::run, &racers[index], RESONATE_STAGE_UPDATE, "racer", 0, TRANSFORMS);
        REQUIRE(resonate_scheduler_add_system(scheduler, &descs[index]) == RESONATE_OK);
    }

    resonate_scheduler_run_frame(scheduler, 0.016F);

    REQUIRE(worst.load() == 1);
    for (std::uint32_t index = 0; index < COUNT; ++index)
    {
        REQUIRE(ranks[index].load() == index);
    }

    resonate_scheduler_destroy(scheduler);
}

TEST_CASE("systems that do not conflict can run at the same time", "[module][scheduler]")
{
    if (resonate_pal_sync_hardware_concurrency() < 2)
    {
        SKIP("needs a second core");
    }

    resonate::test::FakeHost fake;
    ResonateScheduler* scheduler = nullptr;
    REQUIRE(resonate_scheduler_create(&scheduler, fake.api()) == RESONATE_OK);

    Rendezvous pair;

    SECTION("writers of different groups")
    {
        ResonateSystemDesc first =
            describeRun(&Rendezvous::first, &pair, RESONATE_STAGE_UPDATE, "first", 0, TRANSFORMS);
        ResonateSystemDesc second = describeRun(&Rendezvous::second, &pair, RESONATE_STAGE_UPDATE,
                                                "second", 0, RENDER_LIST);
        REQUIRE(resonate_scheduler_add_system(scheduler, &first) == RESONATE_OK);
        REQUIRE(resonate_scheduler_add_system(scheduler, &second) == RESONATE_OK);

        resonate_scheduler_run_frame(scheduler, 0.016F);
        REQUIRE(pair.sawEachOther.load());
    }

    SECTION("readers of one group")
    {
        ResonateSystemDesc first =
            describeRun(&Rendezvous::first, &pair, RESONATE_STAGE_UPDATE, "first", TRANSFORMS, 0);
        ResonateSystemDesc second =
            describeRun(&Rendezvous::second, &pair, RESONATE_STAGE_UPDATE, "second", TRANSFORMS, 0);
        REQUIRE(resonate_scheduler_add_system(scheduler, &first) == RESONATE_OK);
        REQUIRE(resonate_scheduler_add_system(scheduler, &second) == RESONATE_OK);

        resonate_scheduler_run_frame(scheduler, 0.016F);
        REQUIRE(pair.sawEachOther.load());
    }

    resonate_scheduler_destroy(scheduler);
}

TEST_CASE("a stage returns only after its systems have finished", "[module][scheduler]")
{
    resonate::test::FakeHost fake;
    ResonateScheduler* scheduler = nullptr;
    REQUIRE(resonate_scheduler_create(&scheduler, fake.api()) == RESONATE_OK);

    /* Disjoint groups, one slow: the run must not return while either is still
       on a worker. */
    SlowBody slow = {.spins = 500000};
    SlowBody fast = {.spins = 0};

    ResonateSystemDesc slow_desc =
        describeRun(&SlowBody::run, &slow, RESONATE_STAGE_UPDATE, "slow", 0, TRANSFORMS);
    ResonateSystemDesc fast_desc =
        describeRun(&SlowBody::run, &fast, RESONATE_STAGE_UPDATE, "fast", 0, RENDER_LIST);
    REQUIRE(resonate_scheduler_add_system(scheduler, &slow_desc) == RESONATE_OK);
    REQUIRE(resonate_scheduler_add_system(scheduler, &fast_desc) == RESONATE_OK);

    resonate_scheduler_run_frame(scheduler, 0.016F);

    REQUIRE(slow.done.load());
    REQUIRE(fast.done.load());

    resonate_scheduler_destroy(scheduler);
}

TEST_CASE("an exclusive system is ordered against systems it does not conflict with",
          "[module][scheduler]")
{
    resonate::test::FakeHost fake;
    ResonateScheduler* scheduler = nullptr;
    REQUIRE(resonate_scheduler_create(&scheduler, fake.api()) == RESONATE_OK);

    std::atomic<std::uint32_t> inside{0};
    std::atomic<std::uint32_t> worst{0};
    std::atomic<std::uint32_t> next{0};
    std::atomic<std::uint32_t> ranks[3] = {};
    Racer racers[3] = {{&next, &ranks[0], &inside, &worst, 20000},
                       {&next, &ranks[1], &inside, &worst, 20000},
                       {&next, &ranks[2], &inside, &worst, 0}};

    /* The middle one declares a group its neighbours do not touch; exclusive
       still forces it between them, and nothing shares the stage with it. */
    ResonateSystemDesc first =
        describeRun(&Racer::run, &racers[0], RESONATE_STAGE_UPDATE, "first", 0, TRANSFORMS);
    ResonateSystemDesc middle =
        describeRun(&Racer::run, &racers[1], RESONATE_STAGE_UPDATE, "middle", 0, RENDER_LIST);
    middle.exclusive_stage = 1;
    ResonateSystemDesc last =
        describeRun(&Racer::run, &racers[2], RESONATE_STAGE_UPDATE, "last", 0, AUDIO_MIX);

    REQUIRE(resonate_scheduler_add_system(scheduler, &first) == RESONATE_OK);
    REQUIRE(resonate_scheduler_add_system(scheduler, &middle) == RESONATE_OK);
    REQUIRE(resonate_scheduler_add_system(scheduler, &last) == RESONATE_OK);

    resonate_scheduler_run_frame(scheduler, 0.016F);

    REQUIRE(worst.load() == 1);
    for (std::uint32_t index = 0; index < 3U; ++index)
    {
        REQUIRE(ranks[index].load() == index);
    }

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

namespace
{

/* A system that carries both run forms, so a test can tell which one the
   schedule called: only one context, so both would write to the same object. */
struct DualRunner
{
    int plain = 0;
    int world = 0;
    ResonateWorld* seen_world = nullptr;
    ResonateCommands* seen_commands = nullptr;
    float seen_delta = 0.0F;

    static void run(void* context, float)
    {
        ++static_cast<DualRunner*>(context)->plain;
    }

    static void runWorld(void* context, ResonateWorld* world, ResonateCommands* commands,
                         float delta)
    {
        auto* self = static_cast<DualRunner*>(context);
        self->seen_world = world;
        self->seen_commands = commands;
        self->seen_delta = delta;
        ++self->world;
    }
};

ResonateSystemDesc describeWorld(void* context, ResonateStage stage, const char* name,
                                 ResonateResourceGroup reads, ResonateResourceGroup writes)
{
    ResonateSystemDesc desc = describeRun(nullptr, context, stage, name, reads, writes);
    desc.run = &DualRunner::run;
    desc.run_world = &DualRunner::runWorld;
    return desc;
}

} // namespace

TEST_CASE("a world-aware system is called with the handles it was registered with",
          "[module][scheduler]")
{
    resonate::test::FakeHost fake;
    ResonateScheduler* scheduler = nullptr;
    REQUIRE(resonate_scheduler_create(&scheduler, fake.api()) == RESONATE_OK);

    int world_marker = 0;
    int commands_marker = 0;
    ResonateWorld world = {};
    world.instance = &world_marker;
    ResonateCommands commands = {};
    commands.instance = &commands_marker;

    DualRunner runner;
    ResonateSystemDesc desc = describeWorld(&runner, RESONATE_STAGE_UPDATE, "world", 0, 0);
    desc.world = &world;
    desc.commands = &commands;
    REQUIRE(resonate_scheduler_add_system(scheduler, &desc) == RESONATE_OK);

    resonate_scheduler_run_frame(scheduler, 0.016F);

    /* The world-aware form replaces the plain one rather than adding to it. */
    REQUIRE(runner.plain == 0);
    REQUIRE(runner.world == 1);
    REQUIRE(runner.seen_world == &world);
    REQUIRE(runner.seen_commands == &commands);
    REQUIRE(runner.seen_delta == 0.016F);

    resonate_scheduler_destroy(scheduler);
}

TEST_CASE("a descriptor that does not cover the world-aware fields keeps its plain run",
          "[module][scheduler]")
{
    for (const uint32_t covered : {RESONATE_SYSTEM_DESC_BASE_SIZE, 0U})
    {
        resonate::test::FakeHost fake;
        ResonateScheduler* scheduler = nullptr;
        REQUIRE(resonate_scheduler_create(&scheduler, fake.api()) == RESONATE_OK);

        DualRunner runner;
        ResonateSystemDesc desc = describeWorld(&runner, RESONATE_STAGE_UPDATE, "legacy", 0, 0);
        desc.struct_size = covered;

        /* Filled in anyway, the way an uninitialised tail would be: the size is
           the writer's claim, and it does not cover the tail. */
        REQUIRE(resonate_scheduler_add_system(scheduler, &desc) == RESONATE_OK);
        resonate_scheduler_run_frame(scheduler, 0.016F);

        INFO("struct_size " << covered);
        REQUIRE(runner.plain == 1);
        REQUIRE(runner.world == 0);

        resonate_scheduler_destroy(scheduler);
    }

    /* Below the base is not a descriptor of any version. */
    resonate::test::FakeHost fake;
    ResonateScheduler* scheduler = nullptr;
    REQUIRE(resonate_scheduler_create(&scheduler, fake.api()) == RESONATE_OK);

    ResonateSystemDesc too_small =
        describeRun(nullptr, nullptr, RESONATE_STAGE_UPDATE, "too-small", 0, 0);
    too_small.struct_size = RESONATE_SYSTEM_DESC_BASE_SIZE - 1U;
    REQUIRE(resonate_scheduler_add_system(scheduler, &too_small) == RESONATE_E_VERSION);

    resonate_scheduler_destroy(scheduler);
}

TEST_CASE("a world-aware system is ordered against every other system of its stage",
          "[module][scheduler]")
{
    resonate::test::FakeHost fake;
    ResonateScheduler* scheduler = nullptr;
    REQUIRE(resonate_scheduler_create(&scheduler, fake.api()) == RESONATE_OK);

    std::atomic<std::uint32_t> inside{0};
    std::atomic<std::uint32_t> worst{0};
    std::atomic<std::uint32_t> next{0};
    std::atomic<std::uint32_t> ranks[3] = {};
    Racer racers[3] = {{&next, &ranks[0], &inside, &worst, 20000},
                       {&next, &ranks[1], &inside, &worst, 20000},
                       {&next, &ranks[2], &inside, &worst, 0}};

    /* The middle registration is world-aware and declares a group its
       neighbours do not touch. Recording shares the world's slot table, so it
       takes one thread: the whole stage still runs it alone, in registration
       order. */
    struct WorldRacer
    {
        Racer* racer = nullptr;
        static void run(void* context, ResonateWorld*, ResonateCommands*, float)
        {
            Racer::run(static_cast<WorldRacer*>(context)->racer, 0.0F);
        }
    };
    WorldRacer world_racer{&racers[1]};

    ResonateSystemDesc first =
        describeRun(&Racer::run, &racers[0], RESONATE_STAGE_UPDATE, "first", 0, TRANSFORMS);
    ResonateSystemDesc middle =
        describeRun(nullptr, &world_racer, RESONATE_STAGE_UPDATE, "middle", 0, RENDER_LIST);
    middle.run_world = &WorldRacer::run;
    ResonateSystemDesc last =
        describeRun(&Racer::run, &racers[2], RESONATE_STAGE_UPDATE, "last", 0, AUDIO_MIX);

    REQUIRE(resonate_scheduler_add_system(scheduler, &first) == RESONATE_OK);
    REQUIRE(resonate_scheduler_add_system(scheduler, &middle) == RESONATE_OK);
    REQUIRE(resonate_scheduler_add_system(scheduler, &last) == RESONATE_OK);

    resonate_scheduler_run_frame(scheduler, 0.016F);

    REQUIRE(worst.load() == 1);
    for (std::uint32_t index = 0; index < 3U; ++index)
    {
        REQUIRE(ranks[index].load() == index);
    }

    resonate_scheduler_destroy(scheduler);
}

TEST_CASE("a descriptor with neither run function is refused", "[module][scheduler]")
{
    resonate::test::FakeHost fake;
    ResonateScheduler* scheduler = nullptr;
    REQUIRE(resonate_scheduler_create(&scheduler, fake.api()) == RESONATE_OK);

    ResonateSystemDesc desc = describeRun(nullptr, nullptr, RESONATE_STAGE_UPDATE, "empty", 0, 0);
    desc.run = nullptr;
    REQUIRE(resonate_scheduler_add_system(scheduler, &desc) == RESONATE_E_INVALID);

    resonate_scheduler_destroy(scheduler);
}
