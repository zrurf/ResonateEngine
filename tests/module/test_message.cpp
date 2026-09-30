#include <catch2/catch_all.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>

#include <resonate/module/message.h>
#include <resonate/module/module.hpp>

#include "fake_host.h"

namespace
{

struct Loaded
{
    static constexpr std::uint32_t typeId()
    {
        return 7U;
    }

    std::int32_t value;
};

struct Other
{
    static constexpr std::uint32_t typeId()
    {
        return 9U;
    }

    std::int32_t value;
};

constexpr std::uint32_t SLOT_BYTES = 64;
constexpr std::uint32_t SLOT_COUNT = 4;

} // namespace

TEST_CASE("every reader sees every message in write order", "[module][message]")
{
    resonate::test::FakeHost fake;
    ResonateMessageWriter* writer = nullptr;
    REQUIRE(resonate_message_writer_create(&writer, fake.api(), SLOT_BYTES, SLOT_COUNT) ==
            RESONATE_OK);

    const Loaded first = {1};
    const Loaded second = {2};
    REQUIRE(resonate_message_write(writer, Loaded::typeId(), &first, sizeof(first)) == 1U);
    REQUIRE(resonate_message_write(writer, Loaded::typeId(), &second, sizeof(second)) == 2U);

    ResonateMessageStream stream = {};
    REQUIRE(resonate_message_open_reader(writer, &stream) == RESONATE_OK);

    ResonateMessageCursor cursor = resonate_message_cursor_begin(&stream);

    ResonateMessageHeader read_header = {};
    const void* payload = nullptr;
    REQUIRE(stream.vtable->read(stream.self, &cursor, &read_header, &payload) == 1U);
    REQUIRE(read_header.sequence == 1U);
    REQUIRE(read_header.type_id == Loaded::typeId());
    REQUIRE(read_header.payload_size == sizeof(Loaded));
    REQUIRE(static_cast<const Loaded*>(payload)->value == 1);

    REQUIRE(stream.vtable->read(stream.self, &cursor, &read_header, &payload) == 1U);
    REQUIRE(read_header.sequence == 2U);
    REQUIRE(static_cast<const Loaded*>(payload)->value == 2);

    /* Caught up, not an error. */
    REQUIRE(stream.vtable->read(stream.self, &cursor, &read_header, &payload) == 0U);
    REQUIRE(stream.vtable->latest_sequence(stream.self) == 2U);

    resonate_message_close_reader(&stream);
    resonate_message_writer_destroy(writer);
}

TEST_CASE("a reader only receives its own message type", "[module][message]")
{
    resonate::test::FakeHost fake;
    resonate::Host host(fake.api());
    ResonateMessageWriter* writer = nullptr;
    REQUIRE(resonate_message_writer_create(&writer, fake.api(), SLOT_BYTES, SLOT_COUNT) ==
            RESONATE_OK);

    const Other other = {5};
    const Loaded loaded = {6};
    const Other tail = {7};
    REQUIRE(resonate_message_write(writer, Other::typeId(), &other, sizeof(other)) != 0U);
    REQUIRE(resonate_message_write(writer, Loaded::typeId(), &loaded, sizeof(loaded)) != 0U);
    REQUIRE(resonate_message_write(writer, Other::typeId(), &tail, sizeof(tail)) != 0U);

    ResonateMessageStream stream = {};
    REQUIRE(resonate_message_open_reader(writer, &stream) == RESONATE_OK);

    resonate::MessageReader<Loaded> reader;
    reader.open(host, stream);

    Loaded received = {};
    REQUIRE(reader.read(received));
    REQUIRE(received.value == 6);
    REQUIRE_FALSE(reader.read(received));

    resonate::MessageReader<Other> other_reader;
    other_reader.open(host, stream);

    Other received_other = {};
    REQUIRE(other_reader.read(received_other));
    REQUIRE(received_other.value == 5);
    REQUIRE(other_reader.read(received_other));
    REQUIRE(received_other.value == 7);

    resonate_message_close_reader(&stream);
    resonate_message_writer_destroy(writer);
}

TEST_CASE("a record larger than a slot is refused, not truncated", "[module][message]")
{
    resonate::test::FakeHost fake;
    ResonateMessageWriter* writer = nullptr;
    REQUIRE(resonate_message_writer_create(&writer, fake.api(), SLOT_BYTES, SLOT_COUNT) ==
            RESONATE_OK);

    struct Big
    {
        std::uint8_t bytes[SLOT_BYTES];
    } big = {};

    REQUIRE(resonate_message_write(writer, 1U, &big, sizeof(big)) == 0U);

    resonate_message_writer_destroy(writer);
}

TEST_CASE("a reader that falls behind can tell, rather than skipping silently", "[module][message]")
{
    resonate::test::FakeHost fake;
    resonate::Host host(fake.api());
    ResonateMessageWriter* writer = nullptr;
    REQUIRE(resonate_message_writer_create(&writer, fake.api(), SLOT_BYTES, SLOT_COUNT) ==
            RESONATE_OK);

    ResonateMessageStream stream = {};
    REQUIRE(resonate_message_open_reader(writer, &stream) == RESONATE_OK);

    resonate::MessageReader<Loaded> reader;
    reader.open(host, stream);

    const Loaded value = {1};
    REQUIRE(resonate_message_write(writer, Loaded::typeId(), &value, sizeof(value)) == 1U);

    Loaded received = {};
    REQUIRE(reader.read(received));
    REQUIRE_FALSE(reader.fellBehind());

    /* Overflow the ring while the reader is idle. */
    for (int index = 0; index < static_cast<int>(SLOT_COUNT) + 2; ++index)
    {
        REQUIRE(resonate_message_write(writer, Loaded::typeId(), &value, sizeof(value)) != 0U);
    }

    REQUIRE_FALSE(reader.read(received));
    REQUIRE(reader.fellBehind());
    REQUIRE(reader.oldestSequence() > reader.cursor() + 1U);

    resonate_message_close_reader(&stream);
    resonate_message_writer_destroy(writer);
}

TEST_CASE("a byte payload survives the round trip", "[module][message]")
{
    resonate::test::FakeHost fake;
    ResonateMessageWriter* writer = nullptr;
    REQUIRE(resonate_message_writer_create(&writer, fake.api(), SLOT_BYTES, SLOT_COUNT) ==
            RESONATE_OK);

    const std::uint8_t bytes[5] = {1U, 2U, 3U, 4U, 5U};
    REQUIRE(resonate_message_write(writer, 3U, bytes, sizeof(bytes)) == 1U);

    ResonateMessageStream stream = {};
    REQUIRE(resonate_message_open_reader(writer, &stream) == RESONATE_OK);

    ResonateMessageCursor cursor = resonate_message_cursor_begin(&stream);
    ResonateMessageHeader header = {};
    const void* payload = nullptr;
    REQUIRE(stream.vtable->read(stream.self, &cursor, &header, &payload) == 1U);
    REQUIRE(header.payload_size == sizeof(bytes));

    /* The payload is aligned for the widest trivially copyable type. */
    REQUIRE(reinterpret_cast<std::uintptr_t>(payload) % alignof(std::max_align_t) == 0U);
    REQUIRE(std::memcmp(payload, bytes, sizeof(bytes)) == 0);

    resonate_message_close_reader(&stream);
    resonate_message_writer_destroy(writer);
}

TEST_CASE("a slot holds a record of exactly the size it was created with", "[module][message]")
{
    resonate::test::FakeHost fake;

    /* slot_bytes counts the header, so this slot holds a 32-byte payload. */
    constexpr std::uint32_t PAYLOAD = 32;
    constexpr std::uint32_t SLOT =
        static_cast<std::uint32_t>(sizeof(ResonateMessageHeader)) + PAYLOAD;

    ResonateMessageWriter* writer = nullptr;
    REQUIRE(resonate_message_writer_create(&writer, fake.api(), SLOT, SLOT_COUNT) == RESONATE_OK);

    std::uint8_t payload[PAYLOAD + 1] = {};
    REQUIRE(resonate_message_write(writer, 1U, payload, PAYLOAD) == 1U);
    REQUIRE(resonate_message_write(writer, 1U, payload, PAYLOAD + 1U) == 0U);

    ResonateMessageStream stream = {};
    REQUIRE(resonate_message_open_reader(writer, &stream) == RESONATE_OK);

    ResonateMessageCursor cursor = resonate_message_cursor_begin(&stream);
    ResonateMessageHeader header = {};
    const void* read_payload = nullptr;
    REQUIRE(stream.vtable->read(stream.self, &cursor, &header, &read_payload) == 1U);
    REQUIRE(header.payload_size == PAYLOAD);

    resonate_message_close_reader(&stream);
    resonate_message_writer_destroy(writer);
}

TEST_CASE("a stream carries one reader", "[module][message]")
{
    resonate::test::FakeHost fake;
    ResonateMessageWriter* writer = nullptr;
    REQUIRE(resonate_message_writer_create(&writer, fake.api(), SLOT_BYTES, SLOT_COUNT) ==
            RESONATE_OK);

    ResonateMessageStream stream = {};
    REQUIRE(resonate_message_open_reader(writer, &stream) == RESONATE_OK);

    /* A second open would overwrite the stream's link back to the first reader,
       which would then go dead without a word to anyone. */
    REQUIRE(resonate_message_open_reader(writer, &stream) == RESONATE_E_STATE);

    /* And the refusal leaves the stream that is already open working. */
    const Loaded loaded = {1};
    REQUIRE(resonate_message_write(writer, Loaded::typeId(), &loaded, sizeof(loaded)) == 1U);

    ResonateMessageCursor cursor = resonate_message_cursor_begin(&stream);
    ResonateMessageHeader header = {};
    const void* payload = nullptr;
    REQUIRE(stream.vtable->read(stream.self, &cursor, &header, &payload) == 1U);
    REQUIRE(static_cast<const Loaded*>(payload)->value == 1);

    resonate_message_close_reader(&stream);
    resonate_message_writer_destroy(writer);
}

TEST_CASE("a reader whose stream was closed reports nothing rather than calling through it",
          "[module][message]")
{
    resonate::test::FakeHost fake;
    resonate::Host host(fake.api());
    ResonateMessageWriter* writer = nullptr;
    REQUIRE(resonate_message_writer_create(&writer, fake.api(), SLOT_BYTES, SLOT_COUNT) ==
            RESONATE_OK);

    ResonateMessageStream stream = {};
    REQUIRE(resonate_message_open_reader(writer, &stream) == RESONATE_OK);

    resonate::MessageReader<Loaded> reader;
    reader.open(host, stream);
    REQUIRE(reader.valid());

    resonate_message_close_reader(&stream);

    REQUIRE_FALSE(reader.valid());
    REQUIRE(reader.oldestSequence() == 0U);
    REQUIRE(reader.latestSequence() == 0U);
    REQUIRE_FALSE(reader.fellBehind());

    Loaded received = {};
    REQUIRE_FALSE(reader.read(received));

    resonate_message_writer_destroy(writer);
}

TEST_CASE("destroying the writer closes the reader a module left open", "[module][message]")
{
    resonate::test::FakeHost fake;
    resonate::Host host(fake.api());
    ResonateMessageWriter* writer = nullptr;
    REQUIRE(resonate_message_writer_create(&writer, fake.api(), SLOT_BYTES, SLOT_COUNT) ==
            RESONATE_OK);

    ResonateMessageStream stream = {};
    REQUIRE(resonate_message_open_reader(writer, &stream) == RESONATE_OK);

    /* The module owns the stream and never closes it; the writer's teardown is
       what has to end the connection, or the stream stays pointing at a freed
       reader. */
    resonate_message_writer_destroy(writer);

    REQUIRE(stream.vtable == nullptr);
    REQUIRE(stream.self == nullptr);

    /* Closing again is what a reader's own teardown does, so it must be safe. */
    resonate_message_close_reader(&stream);
}

TEST_CASE("the typed writer goes through the host API like a module's", "[module][message]")
{
    resonate::test::FakeHost fake;
    resonate::Host host(fake.api());

    REQUIRE(fake.writerCount() == 0U);

    resonate::MessageWriter<Loaded> writer;
    REQUIRE(writer.create(host, SLOT_COUNT) == RESONATE_OK);
    REQUIRE(fake.writerCount() == 1U);

    ResonateMessageStream stream = {};
    REQUIRE(writer.open(stream) == RESONATE_OK);

    resonate::MessageReader<Loaded> reader;
    reader.open(host, stream);

    const Loaded sent = {11U};
    REQUIRE(writer.write(sent) != 0U);

    Loaded received = {};
    REQUIRE(reader.read(received));
    REQUIRE(received.value == 11U);

    reader.close();

    /* The host holds the storage until the module releases it. */
    writer.destroy();
    REQUIRE(fake.writerCount() == 0U);
}
