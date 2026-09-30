#include <catch2/catch_all.hpp>

#include <cstdint>

#include <resonate/pal/event.h>
#include <resonate/pal/pal.h>

namespace
{

constexpr const char* TEST_FILE = "resonate_pal_event_test.bin";
constexpr std::uint32_t PAYLOAD_SIZE = 8;

void removeFile()
{
    if (resonate_pal_io_exists(TEST_FILE) != 0U)
    {
        resonate_pal_io_remove(TEST_FILE);
    }
}

/* The initial contents are written through the io layer, which needs a handle of
   its own: a file opened for the event loop cannot be read or written directly. */
void prepareFile(const void* contents)
{
    removeFile();

    ResonateFileHandle file = {};
    REQUIRE(resonate_pal_io_open_file(&file, TEST_FILE,
                                      RESONATE_FILE_WRITE | RESONATE_FILE_CREATE |
                                          RESONATE_FILE_TRUNCATE) == RESONATE_PAL_OK);
    REQUIRE(resonate_pal_io_write(&file, contents, PAYLOAD_SIZE, nullptr) == RESONATE_PAL_OK);
    resonate_pal_io_close_file(&file);
}

struct Dispatched
{
    std::uint32_t count = 0;
    ResonateEventToken token = 0;
    ResonateEventOperation operation = RESONATE_EVENT_OP_NONE;
    ResonatePalStatus status = RESONATE_PAL_UNKNOWN;
    std::uint32_t bytes = 0;
    const void* buffer = nullptr;
};

/* The token is the caller's identity for a submission, so a pointer to the
   caller's own state travels through it. */
ResonateEventToken tokenFor(Dispatched& state)
{
    return static_cast<ResonateEventToken>(reinterpret_cast<std::uintptr_t>(&state));
}

void capture(const ResonateEventCompletion* completion)
{
    auto* recorded = reinterpret_cast<Dispatched*>(static_cast<std::uintptr_t>(completion->token));
    ++recorded->count;
    recorded->token = completion->token;
    recorded->operation = completion->operation;
    recorded->status = completion->status;
    recorded->bytes = completion->bytes_transferred;
    recorded->buffer = completion->buffer;
}

} // namespace

TEST_CASE("an idle loop times out instead of blocking", "[pal][event]")
{
    ResonateEventLoop loop = {};
    REQUIRE(resonate_pal_event_create_loop(&loop) == RESONATE_PAL_OK);

    Dispatched dispatched;
    std::uint32_t count = 99;

    REQUIRE(resonate_pal_event_dispatch(&loop, &capture, 0, &count) == RESONATE_PAL_TIMEOUT);
    REQUIRE(count == 0U);
    REQUIRE(dispatched.count == 0U);

    resonate_pal_event_destroy_loop(&loop);
}

TEST_CASE("a submitted read completes with the bytes it was given", "[pal][event]")
{
    const std::uint32_t expected[PAYLOAD_SIZE / sizeof(std::uint32_t)] = {0xA1B2C3D4U, 0x11223344U};
    prepareFile(expected);

    ResonateEventLoop loop = {};
    REQUIRE(resonate_pal_event_create_loop(&loop) == RESONATE_PAL_OK);

    ResonateFileHandle file = {};
    REQUIRE(resonate_pal_io_open_file(&file, TEST_FILE,
                                      RESONATE_FILE_READ | RESONATE_FILE_OVERLAPPED) ==
            RESONATE_PAL_OK);

    std::uint32_t received[PAYLOAD_SIZE / sizeof(std::uint32_t)] = {};
    Dispatched dispatched;

    REQUIRE(resonate_pal_event_submit_read(&loop, tokenFor(dispatched), &file, received,
                                           PAYLOAD_SIZE, 0) == RESONATE_PAL_OK);

    std::uint32_t count = 0;
    REQUIRE(resonate_pal_event_dispatch(&loop, &capture, 2000000000ULL, &count) == RESONATE_PAL_OK);
    REQUIRE(count == 1U);

    REQUIRE(dispatched.token == tokenFor(dispatched));
    REQUIRE(dispatched.operation == RESONATE_EVENT_OP_FILE_READ);
    REQUIRE(dispatched.status == RESONATE_PAL_OK);
    REQUIRE(dispatched.bytes == PAYLOAD_SIZE);
    REQUIRE(received[0] == 0xA1B2C3D4U);
    REQUIRE(received[1] == 0x11223344U);

    resonate_pal_io_close_file(&file);
    resonate_pal_event_destroy_loop(&loop);
    removeFile();
}

TEST_CASE("a submitted write reaches the file", "[pal][event]")
{
    const std::uint32_t initial[PAYLOAD_SIZE / sizeof(std::uint32_t)] = {0U, 0U};
    prepareFile(initial);

    ResonateEventLoop loop = {};
    REQUIRE(resonate_pal_event_create_loop(&loop) == RESONATE_PAL_OK);

    ResonateFileHandle file = {};
    REQUIRE(resonate_pal_io_open_file(&file, TEST_FILE,
                                      RESONATE_FILE_READ | RESONATE_FILE_WRITE |
                                          RESONATE_FILE_OVERLAPPED) == RESONATE_PAL_OK);

    const std::uint32_t written[PAYLOAD_SIZE / sizeof(std::uint32_t)] = {0xDEADBEEFU, 0xFEEDFACEU};
    Dispatched dispatched;

    REQUIRE(resonate_pal_event_submit_write(&loop, tokenFor(dispatched), &file, written,
                                            PAYLOAD_SIZE, 0) == RESONATE_PAL_OK);

    std::uint32_t count = 0;
    REQUIRE(resonate_pal_event_dispatch(&loop, &capture, 2000000000ULL, &count) == RESONATE_PAL_OK);
    REQUIRE(dispatched.operation == RESONATE_EVENT_OP_FILE_WRITE);
    REQUIRE(dispatched.status == RESONATE_PAL_OK);
    REQUIRE(dispatched.bytes == PAYLOAD_SIZE);

    resonate_pal_io_close_file(&file);

    /* Read it back through the io layer, which is what proves the write landed. */
    ResonateFileHandle reader = {};
    REQUIRE(resonate_pal_io_open_file(&reader, TEST_FILE, RESONATE_FILE_READ) == RESONATE_PAL_OK);
    std::uint32_t read_back[PAYLOAD_SIZE / sizeof(std::uint32_t)] = {};
    REQUIRE(resonate_pal_io_read(&reader, read_back, PAYLOAD_SIZE, nullptr) == RESONATE_PAL_OK);
    REQUIRE(read_back[0] == 0xDEADBEEFU);
    REQUIRE(read_back[1] == 0xFEEDFACEU);
    resonate_pal_io_close_file(&reader);

    resonate_pal_event_destroy_loop(&loop);
    removeFile();
}

TEST_CASE("opening for append positions the file at its end", "[pal][event][io]")
{
    const std::uint32_t initial[PAYLOAD_SIZE / sizeof(std::uint32_t)] = {0U, 0U};
    prepareFile(initial);

    ResonateEventLoop loop = {};
    REQUIRE(resonate_pal_event_create_loop(&loop) == RESONATE_PAL_OK);

    /* The write names offset 0 on a file opened for append: that is where it has
       to land, on either backend. An append that overrode the offset would put it
       at the end instead, which is what Linux does to a write into an O_APPEND
       file and what this pins out. */
    ResonateFileHandle appending = {};
    REQUIRE(resonate_pal_io_open_file(&appending, TEST_FILE,
                                      RESONATE_FILE_WRITE | RESONATE_FILE_APPEND |
                                          RESONATE_FILE_OVERLAPPED) == RESONATE_PAL_OK);

    const std::uint32_t at_start[PAYLOAD_SIZE / sizeof(std::uint32_t)] = {0xAABBCCDDU, 0x12345678U};
    Dispatched dispatched;
    REQUIRE(resonate_pal_event_submit_write(&loop, tokenFor(dispatched), &appending, at_start,
                                            PAYLOAD_SIZE, 0) == RESONATE_PAL_OK);

    std::uint32_t count = 0;
    REQUIRE(resonate_pal_event_dispatch(&loop, &capture, 2000000000ULL, &count) == RESONATE_PAL_OK);
    REQUIRE(dispatched.status == RESONATE_PAL_OK);
    resonate_pal_io_close_file(&appending);

    ResonateFileHandle reader = {};
    REQUIRE(resonate_pal_io_open_file(&reader, TEST_FILE, RESONATE_FILE_READ) == RESONATE_PAL_OK);
    std::uint32_t read_back[PAYLOAD_SIZE / sizeof(std::uint32_t)] = {};
    REQUIRE(resonate_pal_io_read(&reader, read_back, PAYLOAD_SIZE, nullptr) == RESONATE_PAL_OK);
    REQUIRE(read_back[0] == 0xAABBCCDDU);
    REQUIRE(read_back[1] == 0x12345678U);
    resonate_pal_io_close_file(&reader);

    /* And a write that names no offset continues at the end, which is what makes
       the flag worth setting at all. */
    ResonateFileHandle writer = {};
    REQUIRE(resonate_pal_io_open_file(
                &writer, TEST_FILE, RESONATE_FILE_WRITE | RESONATE_FILE_APPEND) == RESONATE_PAL_OK);
    REQUIRE(resonate_pal_io_write(&writer, "Z", 1U, nullptr) == RESONATE_PAL_OK);
    resonate_pal_io_close_file(&writer);

    std::uint64_t size = 0;
    REQUIRE(resonate_pal_io_open_file(&reader, TEST_FILE, RESONATE_FILE_READ) == RESONATE_PAL_OK);
    REQUIRE(resonate_pal_io_size(&reader, &size) == RESONATE_PAL_OK);
    REQUIRE(size == PAYLOAD_SIZE + 1U);
    resonate_pal_io_close_file(&reader);

    resonate_pal_event_destroy_loop(&loop);
    removeFile();
}

TEST_CASE("a malformed submission is refused before anything is queued", "[pal][event]")
{
    ResonateEventLoop loop = {};
    REQUIRE(resonate_pal_event_create_loop(&loop) == RESONATE_PAL_OK);

    const std::uint32_t buffer = 0;
    ResonateFileHandle file = {};
    REQUIRE(resonate_pal_io_open_file(&file, TEST_FILE,
                                      RESONATE_FILE_WRITE | RESONATE_FILE_CREATE |
                                          RESONATE_FILE_TRUNCATE | RESONATE_FILE_OVERLAPPED) ==
            RESONATE_PAL_OK);

    REQUIRE(resonate_pal_event_submit_read(&loop, 1, &file, nullptr, PAYLOAD_SIZE, 0) ==
            RESONATE_PAL_INVALID);
    REQUIRE(resonate_pal_event_submit_read(&loop, 1, &file, const_cast<std::uint32_t*>(&buffer), 0,
                                           0) == RESONATE_PAL_INVALID);
    REQUIRE(resonate_pal_event_submit_read(&loop, 1, nullptr, const_cast<std::uint32_t*>(&buffer),
                                           PAYLOAD_SIZE, 0) == RESONATE_PAL_INVALID);

    resonate_pal_io_close_file(&file);
    resonate_pal_event_destroy_loop(&loop);
    removeFile();
}

TEST_CASE("the loop is usable again after every operation has been dispatched", "[pal][event]")
{
    const std::uint32_t expected[4] = {1U, 2U};
    prepareFile(expected);

    ResonateEventLoop loop = {};
    REQUIRE(resonate_pal_event_create_loop(&loop) == RESONATE_PAL_OK);

    ResonateFileHandle file = {};
    REQUIRE(resonate_pal_io_open_file(&file, TEST_FILE,
                                      RESONATE_FILE_READ | RESONATE_FILE_OVERLAPPED) ==
            RESONATE_PAL_OK);

    for (int round = 0; round < 2; ++round)
    {
        std::uint32_t received[4] = {};
        Dispatched dispatched;

        const ResonatePalStatus submitted = resonate_pal_event_submit_read(
            &loop, tokenFor(dispatched), &file, received, PAYLOAD_SIZE, 0);
        CAPTURE(round, static_cast<int>(submitted));
        REQUIRE(submitted == RESONATE_PAL_OK);

        std::uint32_t count = 0;
        REQUIRE(resonate_pal_event_dispatch(&loop, &capture, 2000000000ULL, &count) ==
                RESONATE_PAL_OK);
        REQUIRE(dispatched.status == RESONATE_PAL_OK);
        REQUIRE(received[1] == 2U);
    }

    resonate_pal_io_close_file(&file);
    resonate_pal_event_destroy_loop(&loop);
    removeFile();
}
