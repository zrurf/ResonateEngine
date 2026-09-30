#ifndef RESONATE_TESTS_FAKE_HOST_H
#define RESONATE_TESTS_FAKE_HOST_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <resonate/module/abi.h>
#include <resonate/module/message.h>
#include <resonate/module/signal.h>

namespace resonate::test
{

/* Host services for the module package, plus a log sink the assertions read:
   what the package promises on failure is largely a log line. */
class FakeHost
{
  public:
    FakeHost()
    {
        api_.struct_size = sizeof(ResonateHostApi);
        api_.abi_version = RESONATE_ABI_VERSION;
        api_.user_data = this;
        api_.allocate = &allocateMemory;
        api_.deallocate = &deallocateMemory;
        api_.query_interface = &queryInterface;
        api_.read_config = &readConfig;
        api_.log = &logMessage;

        api_.signal_create = &signalCreate;
        api_.signal_destroy = &signalDestroy;
        api_.signal_connect = &signalConnect;
        api_.signal_disconnect = &signalDisconnect;
        api_.signal_emit = &signalEmit;
        api_.signal_emit_until = &signalEmitUntil;
        api_.signal_subscriber_count = &signalSubscriberCount;
        api_.message_writer_create = &messageWriterCreate;
        api_.message_writer_destroy = &messageWriterDestroy;
        api_.message_write = &messageWrite;
        api_.message_clear = &messageClear;
        api_.message_open_reader = &messageOpenReader;
        api_.message_close_reader = &messageCloseReader;
        api_.message_cursor_begin = &messageCursorBegin;
    }

    [[nodiscard]] const ResonateHostApi* api() const noexcept
    {
        return &api_;
    }

    /* What the host is still holding for modules: a test reads these to tell a
       module that destroyed its storage from one that left it to be reclaimed. */
    [[nodiscard]] std::size_t signalCount() const noexcept
    {
        return signals_.size();
    }

    [[nodiscard]] std::size_t writerCount() const noexcept
    {
        return writers_.size();
    }

    /* Whether a node's own teardown reached the host: a node whose signal is gone
       must not call through, and nothing else observes that. */
    [[nodiscard]] std::size_t signalDisconnectCount() const noexcept
    {
        return signal_disconnects_;
    }

    [[nodiscard]] const std::vector<std::string>& messages() const noexcept
    {
        return messages_;
    }

    [[nodiscard]] bool logged(std::string_view fragment) const
    {
        for (const std::string& message : messages_)
        {
            if (message.find(fragment) != std::string::npos)
            {
                return true;
            }
        }
        return false;
    }

    void clearMessages()
    {
        messages_.clear();
    }

    void publish(ResonateId id, std::uint32_t version, void* instance)
    {
        published_[key(id)] = {version, instance};
    }

  private:
    struct Entry
    {
        std::uint32_t version;
        void* instance;
    };

    static std::uint64_t key(ResonateId id)
    {
        return id.lo ^ (id.hi * 0x9E3779B97F4A7C15ULL);
    }

    static void* allocateMemory(void*, std::size_t size, std::size_t alignment)
    {
        const std::size_t effective = alignment > sizeof(void*) ? alignment : sizeof(void*);
        void* raw = std::malloc(size + effective);
        if (raw == nullptr)
        {
            return nullptr;
        }

        const auto aligned = (reinterpret_cast<std::uintptr_t>(raw) + effective) &
                             ~(static_cast<std::uintptr_t>(effective) - 1);
        reinterpret_cast<void**>(aligned)[-1] = raw;
        return reinterpret_cast<void*>(aligned);
    }

    static void deallocateMemory(void*, void* memory, std::size_t)
    {
        if (memory != nullptr)
        {
            std::free(reinterpret_cast<void**>(memory)[-1]);
        }
    }

    static void* queryInterface(void* user_data, ResonateId id, std::uint32_t min_version,
                                std::uint32_t* out_version)
    {
        auto* self = static_cast<FakeHost*>(user_data);
        const auto found = self->published_.find(key(id));
        if (found == self->published_.end() || found->second.version < min_version)
        {
            return nullptr;
        }
        if (out_version != nullptr)
        {
            *out_version = found->second.version;
        }
        return found->second.instance;
    }

    static ResonateStatus readConfig(void*, const char*, const char*, char*, std::size_t)
    {
        return RESONATE_E_MISSING;
    }

    static void logMessage(void* user_data, int32_t, const char*, int32_t, const char* message)
    {
        auto* self = static_cast<FakeHost*>(user_data);
        self->messages_.emplace_back(message != nullptr ? message : "");
    }

    /* The facility entries the real host installs per module. Only creation is
       resolved through the fake; the rest exist so a module reaches them without
       linking the host, exactly as the ABI documents. */
    static ResonateStatus signalCreate(void* user_data, ResonateSignalStorage** out_storage)
    {
        auto* self = static_cast<FakeHost*>(user_data);
        const ResonateStatus status = resonate_signal_create(out_storage, &self->api_);
        if (status == RESONATE_OK)
        {
            self->signals_.push_back(*out_storage);
        }
        return status;
    }

    static void signalDestroy(void* user_data, ResonateSignalStorage* storage)
    {
        auto* self = static_cast<FakeHost*>(user_data);
        if (storage == nullptr)
        {
            return;
        }

        /* The ABI makes this a request, not a handover: storage the host is not
           holding is ignored, which is what lets a module's own destructor run
           after the host has already reclaimed it at detach. */
        const auto found = std::find(self->signals_.begin(), self->signals_.end(), storage);
        if (found == self->signals_.end())
        {
            return;
        }

        self->signals_.erase(found);
        resonate_signal_destroy(storage);
    }

    static ResonateStatus signalConnect(void*, ResonateSignalStorage* storage,
                                        ResonateSignalNode* node, ResonateSignalInvoke invoke)
    {
        return resonate_signal_connect(storage, node, invoke);
    }

    static void signalDisconnect(void* user_data, ResonateSignalStorage* storage,
                                 ResonateSignalNode* node)
    {
        ++static_cast<FakeHost*>(user_data)->signal_disconnects_;
        resonate_signal_disconnect(storage, node);
    }

    static std::uint32_t signalEmit(void*, ResonateSignalStorage* storage, const void* payload)
    {
        return resonate_signal_emit(storage, payload);
    }

    static std::uint32_t signalEmitUntil(void*, ResonateSignalStorage* storage, const void* payload)
    {
        return resonate_signal_emit_until(storage, payload);
    }

    static std::uint32_t signalSubscriberCount(void*, const ResonateSignalStorage* storage)
    {
        return resonate_signal_subscriber_count(storage);
    }

    static ResonateStatus messageWriterCreate(void* user_data, ResonateMessageWriter** out_writer,
                                              std::uint32_t slot_bytes, std::uint32_t slot_count)
    {
        auto* self = static_cast<FakeHost*>(user_data);
        const ResonateStatus status =
            resonate_message_writer_create(out_writer, &self->api_, slot_bytes, slot_count);
        if (status == RESONATE_OK)
        {
            self->writers_.push_back(*out_writer);
        }
        return status;
    }

    static void messageWriterDestroy(void* user_data, ResonateMessageWriter* writer)
    {
        auto* self = static_cast<FakeHost*>(user_data);
        if (writer == nullptr)
        {
            return;
        }

        const auto found = std::find(self->writers_.begin(), self->writers_.end(), writer);
        if (found == self->writers_.end())
        {
            return;
        }

        self->writers_.erase(found);
        resonate_message_writer_destroy(writer);
    }

    static ResonateMessageSequence messageWrite(void*, ResonateMessageWriter* writer,
                                                std::uint32_t type_id, const void* payload,
                                                std::uint32_t payload_size)
    {
        return resonate_message_write(writer, type_id, payload, payload_size);
    }

    static void messageClear(void*, ResonateMessageWriter* writer)
    {
        resonate_message_clear(writer);
    }

    static ResonateStatus messageOpenReader(void*, ResonateMessageWriter* writer,
                                            ResonateMessageStream* out_stream)
    {
        return resonate_message_open_reader(writer, out_stream);
    }

    static void messageCloseReader(void*, ResonateMessageStream* stream)
    {
        resonate_message_close_reader(stream);
    }

    static ResonateMessageCursor messageCursorBegin(void*, const ResonateMessageStream* stream)
    {
        return resonate_message_cursor_begin(stream);
    }

    ResonateHostApi api_ = {};
    std::vector<std::string> messages_;
    std::unordered_map<std::uint64_t, Entry> published_;
    std::vector<ResonateSignalStorage*> signals_;
    std::vector<ResonateMessageWriter*> writers_;
    std::size_t signal_disconnects_ = 0;
};

} // namespace resonate::test

#endif /* RESONATE_TESTS_FAKE_HOST_H */
