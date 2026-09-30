#include <catch2/catch_all.hpp>

#include <array>
#include <functional>
#include <string>
#include <vector>

#include <resonate/module/module.hpp>

#include "fake_host.h"

TEST_CASE("subscribers run in connection order", "[module][signal]")
{
    resonate::test::FakeHost fake;
    resonate::Host host(fake.api());

    resonate::Signal<int> signal;
    REQUIRE(signal.create(host) == RESONATE_OK);

    std::vector<std::string> order;
    auto first = [&order](int value) { order.emplace_back("first:" + std::to_string(value)); };
    auto second = [&order](int value) { order.emplace_back("second:" + std::to_string(value)); };

    resonate::SignalNode first_node;
    resonate::SignalNode second_node;
    REQUIRE(first_node.connect(signal, first) == RESONATE_OK);
    REQUIRE(second_node.connect(signal, second) == RESONATE_OK);
    REQUIRE(signal.subscriberCount() == 2);

    signal.emit(7);

    REQUIRE(order.size() == 2);
    REQUIRE(order[0] == "first:7");
    REQUIRE(order[1] == "second:7");
}

TEST_CASE("a subscriber disconnecting from inside its own callback is safe", "[module][signal]")
{
    resonate::test::FakeHost fake;
    resonate::Host host(fake.api());

    resonate::Signal<> signal;
    REQUIRE(signal.create(host) == RESONATE_OK);

    int second_calls = 0;
    auto second = [&second_calls]() { ++second_calls; };
    resonate::SignalNode second_node;

    /* first_node is connected first, so its callback runs before second_node is
       reached and the disconnect lands in time. */
    auto first = [&second_node]() { second_node.disconnect(); };
    resonate::SignalNode first_node;
    REQUIRE(first_node.connect(signal, first) == RESONATE_OK);
    REQUIRE(second_node.connect(signal, second) == RESONATE_OK);

    signal.emit();
    REQUIRE(second_calls == 0);
    REQUIRE(signal.subscriberCount() == 1);

    /* The removal happened during the emit, so the next one must not resurrect it. */
    signal.emit();
    REQUIRE(second_calls == 0);
}

TEST_CASE("a scoped connection ends with its node", "[module][signal]")
{
    resonate::test::FakeHost fake;
    resonate::Host host(fake.api());

    resonate::Signal<int> signal;
    REQUIRE(signal.create(host) == RESONATE_OK);

    int calls = 0;
    auto callback = [&calls](int) { ++calls; };

    {
        resonate::SignalNode node;
        REQUIRE(node.connect(signal, callback) == RESONATE_OK);
        signal.emit(1);
        REQUIRE(calls == 1);
        REQUIRE(signal.subscriberCount() == 1);
    }

    signal.emit(1);
    REQUIRE(calls == 1);
    REQUIRE(signal.subscriberCount() == 0);
}

TEST_CASE("emit_until stops at the first subscriber that answers", "[module][signal]")
{
    resonate::test::FakeHost fake;
    resonate::Host host(fake.api());

    resonate::Signal<int> signal;
    REQUIRE(signal.create(host) == RESONATE_OK);

    int later_calls = 0;
    auto declines = [](int) { return 0U; };
    auto claims = [](int) { return 99U; };
    auto later = [&later_calls](int)
    {
        ++later_calls;
        return 0U;
    };

    resonate::SignalNode declining_node;
    resonate::SignalNode claiming_node;
    resonate::SignalNode later_node;
    REQUIRE(declining_node.connect(signal, declines) == RESONATE_OK);
    REQUIRE(claiming_node.connect(signal, claims) == RESONATE_OK);
    REQUIRE(later_node.connect(signal, later) == RESONATE_OK);

    REQUIRE(signal.emitUntil(3) == 99U);
    REQUIRE(later_calls == 0);

    signal.emit(3);
    REQUIRE(later_calls == 1);
}

TEST_CASE("reconnecting one node replaces its connection", "[module][signal]")
{
    resonate::test::FakeHost fake;
    resonate::Host host(fake.api());

    resonate::Signal<> signal;
    REQUIRE(signal.create(host) == RESONATE_OK);

    int first_calls = 0;
    int second_calls = 0;
    auto first = [&first_calls]() { ++first_calls; };
    auto second = [&second_calls]() { ++second_calls; };

    resonate::SignalNode node;
    REQUIRE(node.connect(signal, first) == RESONATE_OK);
    REQUIRE(node.connect(signal, second) == RESONATE_OK);
    REQUIRE(signal.subscriberCount() == 1);

    signal.emit();
    REQUIRE(first_calls == 0);
    REQUIRE(second_calls == 1);
}

TEST_CASE("the C layer refuses a node that is already connected", "[module][signal]")
{
    resonate::test::FakeHost fake;
    resonate::Host host(fake.api());

    resonate::Signal<> signal;
    REQUIRE(signal.create(host) == RESONATE_OK);

    ResonateSignalNode node = {};
    ResonateSignalInvoke invoke = [](void*, const void*) { return 0U; };

    REQUIRE(resonate_signal_connect(signal.storage(), &node, invoke) == RESONATE_OK);
    REQUIRE(resonate_signal_connect(signal.storage(), &node, invoke) == RESONATE_E_INVALID);
    REQUIRE(resonate_signal_subscriber_count(signal.storage()) == 1U);

    ResonateSignalNode other = {};
    REQUIRE(resonate_signal_connect(signal.storage(), &other, invoke) == RESONATE_OK);
    REQUIRE(resonate_signal_subscriber_count(signal.storage()) == 2U);
}

TEST_CASE("a null argument is refused wherever it appears", "[module][signal]")
{
    resonate::test::FakeHost fake;
    resonate::Host host(fake.api());

    REQUIRE(resonate_signal_create(nullptr, fake.api()) == RESONATE_E_INVALID);

    ResonateSignalStorage* storage = nullptr;
    REQUIRE(resonate_signal_create(&storage, nullptr) == RESONATE_E_INVALID);
    REQUIRE(resonate_signal_create(&storage, fake.api()) == RESONATE_OK);

    ResonateSignalNode node = {};
    ResonateSignalInvoke invoke = [](void*, const void*) { return 0U; };
    REQUIRE(resonate_signal_connect(storage, nullptr, invoke) == RESONATE_E_INVALID);
    REQUIRE(resonate_signal_connect(storage, &node, nullptr) == RESONATE_E_INVALID);

    REQUIRE(resonate_signal_subscriber_count(storage) == 0U);
    REQUIRE(resonate_signal_emit(storage, nullptr) == 0U);

    resonate_signal_destroy(storage);
}

TEST_CASE("subscriptions keep working past the initial capacity", "[module][signal]")
{
    resonate::test::FakeHost fake;
    resonate::Host host(fake.api());

    resonate::Signal<> signal;
    REQUIRE(signal.create(host) == RESONATE_OK);

    /* array, not vector: a node keeps a pointer to its callback, so growing the
       container would leave earlier nodes pointing at freed storage. */
    constexpr int COUNT = 40;
    std::vector<resonate::SignalNode> nodes(COUNT);
    std::array<int, COUNT> calls{};
    std::array<std::function<void()>, COUNT> callbacks;

    for (int index = 0; index < COUNT; ++index)
    {
        callbacks[static_cast<std::size_t>(index)] = [&calls, index] { ++calls[index]; };
        REQUIRE(nodes[static_cast<std::size_t>(index)].connect(
                    signal, callbacks[static_cast<std::size_t>(index)]) == RESONATE_OK);
    }
    REQUIRE(signal.subscriberCount() == COUNT);

    signal.emit();
    for (int index = 0; index < COUNT; ++index)
    {
        INFO("index " << index);
        REQUIRE(calls[index] == 1);
    }
}

TEST_CASE("a node names its connection, and the host clears it", "[module][signal]")
{
    resonate::test::FakeHost fake;

    ResonateSignalStorage* storage = nullptr;
    REQUIRE(resonate_signal_create(&storage, fake.api()) == RESONATE_OK);

    ResonateSignalNode node = {};
    ResonateSignalInvoke invoke = [](void*, const void*) { return 0U; };
    REQUIRE(resonate_signal_connect(storage, &node, invoke) == RESONATE_OK);
    REQUIRE(node.storage == storage);

    resonate_signal_disconnect(storage, &node);
    REQUIRE(node.storage == nullptr);

    /* And a node is told when the storage is what goes away first, which is the
       case the node cannot see for itself. */
    REQUIRE(resonate_signal_connect(storage, &node, invoke) == RESONATE_OK);
    resonate_signal_destroy(storage);
    REQUIRE(node.storage == nullptr);
}

TEST_CASE("a node whose signal is gone does not call into it", "[module][signal]")
{
    resonate::test::FakeHost fake;
    resonate::Host host(fake.api());

    int calls = 0;
    auto callback = [&calls] { ++calls; };

    resonate::Signal<> signal;
    REQUIRE(signal.create(host) == RESONATE_OK);

    resonate::SignalNode node;
    REQUIRE(node.connect(signal, callback) == RESONATE_OK);

    /* The storage is freed while the node is still alive: what a module reaches by
       declaring the node before the member holding the signal, and what its own
       detach does when it destroys the signal first. */
    signal.destroy();

    /* Calling through into the freed storage would be a use-after-free; the count
       is what makes the difference observable without one. */
    node.disconnect();
    REQUIRE(fake.signalDisconnectCount() == 0U);
}

TEST_CASE("a signal is created and released through the host API", "[module][signal]")
{
    resonate::test::FakeHost fake;
    resonate::Host host(fake.api());

    REQUIRE(fake.signalCount() == 0U);

    resonate::Signal<int> signal;
    REQUIRE(signal.create(host) == RESONATE_OK);
    REQUIRE(fake.signalCount() == 1U);
    REQUIRE(signal.subscriberCount() == 0U);

    /* Once per signal: a second create would drop the storage already made. */
    REQUIRE(signal.create(host) == RESONATE_E_STATE);

    /* The host holds the storage until the module releases it, which is what
       makes the reclaim at on_detach possible. */
    signal.destroy();
    REQUIRE(fake.signalCount() == 0U);
}

TEST_CASE("releasing a signal the host is not holding is ignored, not freed", "[module][signal]")
{
    resonate::test::FakeHost fake;

    ResonateSignalStorage* storage = nullptr;
    REQUIRE(fake.api()->signal_create(fake.api()->user_data, &storage) == RESONATE_OK);
    REQUIRE(fake.signalCount() == 1U);

    fake.api()->signal_destroy(fake.api()->user_data, storage);
    REQUIRE(fake.signalCount() == 0U);

    /* A module's own destructor runs when its library unloads, which is after the
       host reclaimed whatever detach left behind, so the second request has to be
       a reported no-op rather than a second free. */
    fake.api()->signal_destroy(fake.api()->user_data, storage);
    REQUIRE(fake.signalCount() == 0U);
}
