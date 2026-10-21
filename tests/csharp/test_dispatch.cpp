#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <thread>

#include <resonate/csharp/dispatch.h>

namespace
{

struct Counter
{
    int calls = 0;
    int last = 0;
};

void firstEntry(void* context)
{
    auto* counter = static_cast<Counter*>(context);
    ++counter->calls;
    counter->last = 1;
}

void secondEntry(void* context)
{
    auto* counter = static_cast<Counter*>(context);
    ++counter->calls;
    counter->last = 2;
}

struct Sink
{
    std::atomic<int> calls{0};
};

void sinkA(void* context)
{
    static_cast<Sink*>(context)->calls.fetch_add(1, std::memory_order_relaxed);
}

void sinkB(void* context)
{
    static_cast<Sink*>(context)->calls.fetch_add(1, std::memory_order_relaxed);
}

} // namespace

TEST_CASE("dispatch: a slot registers once and runs with its context", "[csharp]")
{
    resonate::csharp::DispatchTable table(4U);
    CHECK(table.capacity() == 4U);
    CHECK(table.set(2U, &firstEntry));

    Counter counter;
    CHECK(table.invoke(2U, &counter));
    CHECK(counter.calls == 1);
    CHECK(counter.last == 1);

    CHECK_FALSE(table.set(2U, &secondEntry));
    CHECK(table.get(2U) == &firstEntry);
}

TEST_CASE("dispatch: replace swaps the entry and an empty slot is a no-op", "[csharp]")
{
    resonate::csharp::DispatchTable table(2U);
    Counter counter;

    CHECK_FALSE(table.invoke(0U, &counter));
    CHECK(counter.calls == 0);

    CHECK(table.set(0U, &firstEntry));
    CHECK(table.replace(0U, &secondEntry));
    CHECK(table.invoke(0U, &counter));
    CHECK(counter.last == 2);

    CHECK(table.replace(0U, &firstEntry));
    CHECK(table.get(0U) == &firstEntry);
}

TEST_CASE("dispatch: out-of-range ids and null entries are refused", "[csharp]")
{
    resonate::csharp::DispatchTable table(2U);
    CHECK_FALSE(table.set(2U, &firstEntry));
    CHECK_FALSE(table.replace(3U, &firstEntry));
    CHECK(table.get(4U) == nullptr);
    CHECK_FALSE(table.set(0U, nullptr));
    CHECK_FALSE(table.replace(0U, nullptr));
}

TEST_CASE("dispatch: swapping during calls lands every call on a live entry", "[csharp]")
{
    resonate::csharp::DispatchTable table(1U);
    REQUIRE(table.set(0U, &sinkA));

    Sink sink;
    std::atomic<bool> stop{false};
    std::thread swapper(
        [&table, &stop]
        {
            bool use_a = true;
            while (!stop.load(std::memory_order_relaxed))
            {
                table.replace(0U, use_a ? &sinkB : &sinkA);
                use_a = !use_a;
            }
        });

    constexpr int kCalls = 20000;
    bool every_call_ran = true;
    for (int call = 0; call < kCalls; ++call)
    {
        every_call_ran = table.invoke(0U, &sink) && every_call_ran;
    }
    stop.store(true, std::memory_order_relaxed);
    swapper.join();

    CHECK(every_call_ran);
    CHECK(sink.calls.load(std::memory_order_relaxed) == kCalls);
}

TEST_CASE("dispatch: clear empties a slot", "[csharp]")
{
    resonate::csharp::DispatchTable table(2U);
    CHECK(table.set(0U, &firstEntry));
    CHECK(table.clear(0U));
    CHECK(table.get(0U) == nullptr);

    Counter counter;
    CHECK_FALSE(table.invoke(0U, &counter));
    CHECK(counter.calls == 0);
    CHECK_FALSE(table.clear(2U));
}
