#ifndef RESONATE_MODULE_MODULE_HPP
#define RESONATE_MODULE_MODULE_HPP

/*
 * C++ authoring layer over the C ABI; the only engine header a module
 * implementation includes.
 */

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#include "resonate/module/abi.h"
#include "resonate/module/capability.h"
#include "resonate/module/capability_id.hpp"
#include "resonate/module/message.h"
#include "resonate/module/signal.h"
#include "resonate/module/stage.h"

namespace resonate
{

class Module;

namespace detail
{

/* One slot per shared library; a module's instance cannot land in another's. */
inline Module*& moduleInstanceSlot()
{
    static Module* instance = nullptr;
    return instance;
}

} // namespace detail

/* Non-owning typed pointer to a published capability, valid until on_detach. */
template <typename Capability> class CapabilityRef
{
  public:
    using Traits = detail::CapabilityTraits<Capability>;

    CapabilityRef() = default;
    CapabilityRef(void* instance, std::uint32_t version)
        : instance_(static_cast<Capability*>(instance)), version_(version)
    {
    }

    [[nodiscard]] bool valid() const noexcept
    {
        return instance_ != nullptr;
    }
    explicit operator bool() const noexcept
    {
        return valid();
    }
    [[nodiscard]] std::uint32_t version() const noexcept
    {
        return version_;
    }

    Capability* operator->() const noexcept
    {
        return instance_;
    }
    Capability& operator*() const noexcept
    {
        return *instance_;
    }

  private:
    Capability* instance_ = nullptr;
    std::uint32_t version_ = 0;
};

class Host
{
  public:
    explicit Host(const ResonateHostApi* api) noexcept : api_(api)
    {
    }

    /* The host guarantees a "requires" capability resolves, so an invalid handle
       means the dependency set was built incorrectly; an "optional" one may not. */
    template <typename Capability> [[nodiscard]] CapabilityRef<Capability> query() const
    {
        using Traits = detail::CapabilityTraits<Capability>;
        std::uint32_t version = 0;
        void* instance =
            api_->query_interface(api_->user_data, Traits::id.value(),
                                  static_cast<std::uint32_t>(Traits::version), &version);
        return CapabilityRef<Capability>(instance, version);
    }

    /* Returns an owning string. The ABI takes NUL-terminated strings, so
       string_view arguments are copied first. */
    [[nodiscard]] std::string config(std::string_view section, std::string_view key,
                                     std::string_view fallback = {}) const
    {
        const std::string section_owned(section);
        const std::string key_owned(key);
        char buffer[256] = {};
        const ResonateStatus status = api_->read_config(api_->user_data, section_owned.c_str(),
                                                        key_owned.c_str(), buffer, sizeof(buffer));
        return status == RESONATE_OK ? std::string(buffer) : std::string(fallback);
    }

    [[nodiscard]] const ResonateHostApi* raw() const noexcept
    {
        return api_;
    }

    /* Whether the host wrote the whole struct this build expects. The ABI makes
       appending to ResonateHostApi a compatible change only because the reader can
       tell how much is there, and the facility entry points are the part a host
       older than these headers does not have. */
    [[nodiscard]] bool hasFacilities() const noexcept
    {
        return api_->struct_size >= sizeof(ResonateHostApi);
    }

    /* Module state must live in host memory so the host can reclaim it. Returns
       nullptr if the host could not satisfy the request. */
    template <typename T> [[nodiscard]] T* allocate() const
    {
        return static_cast<T*>(api_->allocate(api_->user_data, sizeof(T), alignof(T)));
    }

    template <typename T> void deallocate(T* memory) const
    {
        if (memory != nullptr)
        {
            api_->deallocate(api_->user_data, memory, sizeof(T));
        }
    }

    /* message is borrowed for the duration of the call only. */
    void log(ResonateLogLevel level, const char* file, int line, const char* message) const
    {
        api_->log(api_->user_data, static_cast<int32_t>(level), file, line, message);
    }

    /* Publishes one of this module's capabilities for as long as it is attached.
       The id has to be in the module's manifest "provides", or the host refuses
       the registration. */
    template <typename Capability> ResonateStatus publish(Capability& instance) const
    {
        ResonateCapabilityRecord record = {};
        record.id = detail::CapabilityTraits<Capability>::id.value();
        record.version = static_cast<uint32_t>(detail::CapabilityTraits<Capability>::version);
        record.instance = &instance;
        record.struct_size = instance.header.struct_size;
        record.name = detail::CapabilityTraits<Capability>::name;
        return api_->register_capability(api_->user_data, &record);
    }

    template <typename Capability> void unpublish(Capability& instance) const
    {
        api_->unregister_capability(api_->user_data,
                                    detail::CapabilityTraits<Capability>::id.value(), &instance);
    }

  private:
    const ResonateHostApi* api_ = nullptr;
};

/*
 * Typed synchronous signal. Create one as a private member of the service that
 * owns the event.
 */
template <typename... Args> class Signal
{
  public:
    Signal() = default;

    ~Signal()
    {
        destroy();
    }

    Signal(const Signal&) = delete;
    Signal& operator=(const Signal&) = delete;

    /* Once per signal: a second call would drop the storage already created. */
    ResonateStatus create(const Host& host)
    {
        if (storage_ != nullptr)
        {
            return RESONATE_E_STATE;
        }
        if (!host.hasFacilities())
        {
            return RESONATE_E_UNSUPPORTED;
        }

        api_ = host.raw();
        return api_->signal_create(api_->user_data, &storage_);
    }

    /* The host reclaims the storage at on_detach whether or not this runs, so a
       module calls it to stop an event early rather than to avoid a leak. */
    void destroy()
    {
        if (storage_ != nullptr)
        {
            api_->signal_destroy(api_->user_data, storage_);
            storage_ = nullptr;
        }
    }

    /* The node is written to, and must outlive the connection. */
    template <typename Callback>
    ResonateStatus connect(ResonateSignalNode& node, Callback& callback) const
    {
        if (storage_ == nullptr)
        {
            return RESONATE_E_STATE;
        }

        /* Only the subscriber is the module's to set; the host writes storage,
           which is how the node learns its connection ended. */
        node.subscriber = static_cast<void*>(std::addressof(callback));
        return api_->signal_connect(api_->user_data, storage_, &node,
                                    &Trampoline<Callback>::invoke);
    }

    /* The argument block crosses the erased boundary as a tuple. */
    void emit(Args... args) const
    {
        if (storage_ == nullptr)
        {
            return;
        }

        const std::tuple<Args...> arguments(args...);
        api_->signal_emit(api_->user_data, storage_, &arguments);
    }

    /* Returns the first non-zero subscriber result, or 0 if none returned one. */
    std::uint32_t emitUntil(Args... args) const
    {
        if (storage_ == nullptr)
        {
            return 0;
        }

        const std::tuple<Args...> arguments(args...);
        return api_->signal_emit_until(api_->user_data, storage_, &arguments);
    }

    [[nodiscard]] std::uint32_t subscriberCount() const
    {
        return storage_ != nullptr ? api_->signal_subscriber_count(api_->user_data, storage_) : 0U;
    }

    [[nodiscard]] ResonateSignalStorage* storage() const noexcept
    {
        return storage_;
    }

    /* What a node needs to disconnect after the signal is out of scope. */
    [[nodiscard]] const ResonateHostApi* api() const noexcept
    {
        return api_;
    }

  private:
    /* Recovers the subscriber's real signature from the erased
       ResonateSignalInvoke; a void-returning callback reports "keep going". */
    template <typename Callback> struct Trampoline
    {
        static std::uint32_t invoke(void* subscriber, const void* payload)
        {
            const auto& arguments = *static_cast<const std::tuple<Args...>*>(payload);
            auto* callback = static_cast<Callback*>(subscriber);

            if constexpr (std::is_void_v<std::invoke_result_t<Callback&, const Args&...>>)
            {
                std::apply([callback](const auto&... unpacked) { (*callback)(unpacked...); },
                           arguments);
                return 0U;
            }
            else
            {
                return static_cast<std::uint32_t>(std::apply([callback](const auto&... unpacked)
                                                             { return (*callback)(unpacked...); },
                                                             arguments));
            }
        }
    };

    ResonateSignalStorage* storage_ = nullptr;
    const ResonateHostApi* api_ = nullptr;
};

/* Subscriber-side connection with scoped lifetime. One per subscriber.
 *
 * The node names the storage it is connected to, and the storage clears that name
 * when it is destroyed or reclaimed, so a node torn down after its signal — a
 * member declared before the one holding the signal — is a no-op rather than a
 * call into freed storage. */
class SignalNode
{
  public:
    SignalNode() = default;

    ~SignalNode()
    {
        disconnect();
    }

    SignalNode(const SignalNode&) = delete;
    SignalNode& operator=(const SignalNode&) = delete;

    template <typename... Args, typename Callback>
    ResonateStatus connect(Signal<Args...>& signal, Callback& callback)
    {
        disconnect();
        const ResonateStatus status = signal.connect(node_, callback);
        if (status == RESONATE_OK)
        {
            signal_ = signal.storage();
            api_ = signal.api();
        }
        return status;
    }

    void disconnect()
    {
        if (signal_ == nullptr)
        {
            return;
        }

        /* Cleared by the storage on destroy, so a node whose signal is gone skips
           the call instead of naming freed storage. */
        if (node_.storage != nullptr)
        {
            api_->signal_disconnect(api_->user_data, signal_, &node_);
        }
        signal_ = nullptr;
    }

  private:
    ResonateSignalNode node_{};
    ResonateSignalStorage* signal_ = nullptr;
    const ResonateHostApi* api_ = nullptr;
};

/*
 * Typed producer side of a message stream. Create one as a private member of the
 * service that owns the notification; consumers open readers on it.
 */
template <typename Message> class MessageWriter
{
  public:
    MessageWriter() = default;

    ~MessageWriter()
    {
        destroy();
    }

    MessageWriter(const MessageWriter&) = delete;
    MessageWriter& operator=(const MessageWriter&) = delete;

    /* slot_count is how far a reader may fall behind before it starts losing
       messages. Once per writer, like Signal::create. */
    ResonateStatus create(const Host& host, std::uint32_t slot_count = 64)
    {
        if (writer_ != nullptr)
        {
            return RESONATE_E_STATE;
        }
        if (!host.hasFacilities())
        {
            return RESONATE_E_UNSUPPORTED;
        }

        api_ = host.raw();

        /* A slot holds the header and the record; the ABI sizes it in bytes, not
           in records. */
        const auto slot_bytes =
            static_cast<std::uint32_t>(sizeof(ResonateMessageHeader) + sizeof(Message));
        return api_->message_writer_create(api_->user_data, &writer_, slot_bytes, slot_count);
    }

    /* Copies the payload into the stream, so the argument is free on return.
       Returns the assigned sequence, or 0 if the record could not be written. */
    ResonateMessageSequence write(const Message& message) const
    {
        if (writer_ == nullptr)
        {
            return 0;
        }

        return api_->message_write(api_->user_data, writer_, Message::typeId(), &message,
                                   static_cast<std::uint32_t>(sizeof(Message)));
    }

    /* Discards everything retained. Readers' cursors are not rewound, so they
       resume at whatever is written next. */
    void clear() const
    {
        if (writer_ != nullptr)
        {
            api_->message_clear(api_->user_data, writer_);
        }
    }

    /* A reader must be closed before the stream goes out of scope; the host
       closes whatever is left when the writer is destroyed. */
    ResonateStatus open(ResonateMessageStream& out_stream) const
    {
        if (writer_ == nullptr)
        {
            return RESONATE_E_STATE;
        }

        return api_->message_open_reader(api_->user_data, writer_, &out_stream);
    }

    void destroy()
    {
        if (writer_ != nullptr)
        {
            api_->message_writer_destroy(api_->user_data, writer_);
            writer_ = nullptr;
        }
    }

    [[nodiscard]] ResonateMessageWriter* raw() const noexcept
    {
        return writer_;
    }

  private:
    ResonateMessageWriter* writer_ = nullptr;
    const ResonateHostApi* api_ = nullptr;
};

/*
 * Typed pull-side of a message stream. The cursor lives in the reader, so each
 * message reaches each reader exactly once.
 */
template <typename Message> class MessageReader
{
  public:
    /* The host is kept because closing needs it. */
    void open(const Host& host, ResonateMessageStream& stream)
    {
        api_ = host.raw();
        stream_ = &stream;
        cursor_ = api_->message_cursor_begin(api_->user_data, &stream);
    }

    void close()
    {
        if (stream_ != nullptr)
        {
            api_->message_close_reader(api_->user_data, stream_);
            stream_ = nullptr;
        }
    }

    /* Advances past messages of other types and returns the next one of this
       reader's type. Returns false when caught up, so `while (reader.read(m))`
       visits exactly the messages of type Message; other readers are
       unaffected. */
    [[nodiscard]] bool read(Message& out)
    {
        ResonateMessageHeader header{};
        const void* payload = nullptr;
        while (readRaw(header, payload))
        {
            if (header.type_id == Message::typeId() && header.payload_size == sizeof(Message))
            {
                out = *static_cast<const Message*>(payload);
                return true;
            }
        }
        return false;
    }

    /* Sequence of the last message consumed, for tracing. */
    [[nodiscard]] ResonateMessageSequence cursor() const noexcept
    {
        return cursor_.sequence;
    }

    [[nodiscard]] ResonateMessageSequence oldestSequence() const noexcept
    {
        return valid() ? stream_->vtable->oldest_sequence(stream_->self) : 0;
    }

    [[nodiscard]] ResonateMessageSequence latestSequence() const noexcept
    {
        return valid() ? stream_->vtable->latest_sequence(stream_->self) : 0;
    }

    /* True when the producer overwrote messages this reader had not reached;
       read() also returns false in that state, so the two are told apart here. */
    [[nodiscard]] bool fellBehind() const noexcept
    {
        return valid() && cursor_.sequence + 1U < oldestSequence();
    }

    /* False once the stream has been closed, which also clears its vtable. */
    [[nodiscard]] bool valid() const noexcept
    {
        return stream_ != nullptr && stream_->vtable != nullptr;
    }

  private:
    [[nodiscard]] bool readRaw(ResonateMessageHeader& header, const void*& payload)
    {
        return valid() && stream_->vtable->read(stream_->self, &cursor_, &header, &payload) != 0;
    }

    ResonateMessageStream* stream_ = nullptr;
    ResonateMessageCursor cursor_{};
    const ResonateHostApi* api_ = nullptr;
};

using Status = ResonateStatus;

class Module
{
  public:
    virtual ~Module() = default;

    virtual ResonateStatus onAttach(Host& host) = 0;
    virtual void onDetach() = 0;
};

namespace detail
{

inline ResonateStatus attachThunk(const ResonateHostApi* api)
{
    Host host(api);
    return moduleInstanceSlot()->onAttach(host);
}

inline void detachThunk()
{
    moduleInstanceSlot()->onDetach();
}

} // namespace detail

} // namespace resonate

/*
 * Defines a module: its identity and the two exported entry points.
 *
 * id_literal is the reverse-DNS id; display_name_literal is for logs only. One
 * instance per library, held in static storage so it is destroyed when the
 * library unloads.
 */
#define RESONATE_DEFINE_MODULE(class_name, id_literal, display_name_literal, version_literal)      \
    static_assert(sizeof(id_literal) > 1, "module id must not be empty");                          \
    static_assert(sizeof(version_literal) > 1, "module version must not be empty");                \
                                                                                                   \
    extern "C" RESONATE_MODULE_EXPORT const ::ResonateModuleInfo* resonateModuleInfo(              \
        std::uint32_t host_abi)                                                                    \
    {                                                                                              \
        if ((host_abi >> 16) != RESONATE_ABI_VERSION_MAJOR)                                        \
        {                                                                                          \
            return nullptr;                                                                        \
        }                                                                                          \
        static const ::ResonateModuleInfo info = {                                                 \
            sizeof(::ResonateModuleInfo), id_literal,                                              \
            display_name_literal,         version_literal,                                         \
            RESONATE_ABI_VERSION_MAJOR,   RESONATE_ABI_VERSION_MAJOR};                             \
        return &info;                                                                              \
    }                                                                                              \
                                                                                                   \
    extern "C" RESONATE_MODULE_EXPORT const ::ResonateModuleVTable* resonateModuleVTable(void)     \
    {                                                                                              \
        static class_name instance;                                                                \
        ::resonate::detail::moduleInstanceSlot() = &instance;                                      \
        static const ::ResonateModuleVTable table = {sizeof(::ResonateModuleVTable),               \
                                                     ::resonate::detail::attachThunk,              \
                                                     ::resonate::detail::detachThunk};             \
        return &table;                                                                             \
    }

#endif /* RESONATE_MODULE_MODULE_HPP */
