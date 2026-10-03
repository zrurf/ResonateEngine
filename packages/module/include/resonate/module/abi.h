#ifndef RESONATE_MODULE_ABI_H
#define RESONATE_MODULE_ABI_H

/*
 * The contract between the host and a plugin. C only: no C++ type, exception,
 * RTTI record or standard library object crosses the boundary, and a module may
 * be built by a different compiler or standard library than the host.
 *
 * Every struct that can grow leads with struct_size, which the writer sets to
 * the size it wrote. A reader compares that against its own sizeof before
 * touching anything past the leading fields; appending a field is a compatible
 * change.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define RESONATE_ABI_VERSION_MAJOR 1
#define RESONATE_ABI_VERSION_MINOR 0

/* Packed major/minor, as passed to resonateModuleInfo and compared by the
   loader; the struct fields below hold a major number only. */
#define RESONATE_ABI_VERSION                                                                       \
    (((uint32_t)RESONATE_ABI_VERSION_MAJOR << 16) | (uint32_t)RESONATE_ABI_VERSION_MINOR)

typedef uint32_t ResonateStatus;

#define RESONATE_OK 0u
#define RESONATE_E_UNSUPPORTED 1u /* the module cannot run here, and knows it */
#define RESONATE_E_INVALID 2u     /* a caller passed something malformed */
#define RESONATE_E_MISSING 3u     /* a declared dependency was not provided */
#define RESONATE_E_VERSION 4u     /* ABI or interface version mismatch */
#define RESONATE_E_STATE 5u       /* called out of lifecycle order */
#define RESONATE_E_UNAVAILABLE 6u /* a resource is absent, e.g. no audio device */
#define RESONATE_E_INTERNAL 7u    /* the module failed for its own reasons */

/* 128-bit hash of a capability or module id. */
typedef struct ResonateId
{
    uint64_t lo;
    uint64_t hi;
} ResonateId;

/*
 * Handles to the run's ECS world and to a system's command buffer. Both are
 * opaque: `instance` is the host's pointer, filled in at registration, and a
 * plugin never dereferences it. They let a world-aware system name what it is
 * handed.
 */
typedef struct ResonateWorld
{
    void* instance;
} ResonateWorld;

typedef struct ResonateCommands
{
    void* instance;
} ResonateCommands;

/* Stable across processes and builds. XXH3-128 of the name, and the same
   function resonate::Id calls, so an id from a manifest and one a capability
   header declares agree by construction rather than by two hashes matching. */
ResonateId resonate_id_make(const char* name);

typedef enum ResonateLogLevel : uint8_t
{
    RESONATE_LOG_TRACE = 0,
    RESONATE_LOG_INFO = 1,
    RESONATE_LOG_WARN = 2,
    RESONATE_LOG_ERROR = 3,
    RESONATE_LOG_FATAL = 4
} ResonateLogLevel;

/* The message is borrowed for the duration of the call. */
typedef void (*ResonateLogFn)(void* user_data, int32_t level, const char* file, int32_t line,
                              const char* message);

/* What a module hands to register_capability. The id and version are the ones in
   its CapabilityTraits, and struct_size is the size of the struct instance points
   at. */
typedef struct ResonateCapabilityRecord
{
    ResonateId id;
    uint32_t version;
    void* instance;

    /* Size of the struct instance points at. The host rejects a provider built
       against a shorter interface. */
    uint32_t struct_size;

    /* Diagnostics only: the host reports names, never hashes. */
    const char* name;
} ResonateCapabilityRecord;

/*
 * Types the host API hands back. The storage is opaque — it belongs to the host
 * — while the value types a caller writes or reads are here, where the entry
 * points that use them are.
 */

typedef struct ResonateSignalStorage ResonateSignalStorage;
typedef struct ResonateMessageWriter ResonateMessageWriter;
typedef struct ResonateMessageStreamVTable ResonateMessageStreamVTable;
typedef struct ResonateSystemDesc ResonateSystemDesc;

/* Subscriber-side connection node. Place one inside the subscriber object; it
   must outlive every connection made from it.
 *
 * Written by the host: connect fills storage in, and disconnect and destroy clear
 * it again. A node therefore reports whether it is still connected, and one whose
 * signal is already gone has nothing to call into. */
typedef struct ResonateSignalNode
{
    void* storage;
    void* subscriber;
} ResonateSignalNode;

/* Erased callback. A non-zero return stops an emit_until. */
typedef uint32_t (*ResonateSignalInvoke)(void* subscriber, const void* payload);

/* Monotonic and gap-free within a stream; also the trace handle for a message
   and the basis for detecting a consumer that has fallen behind. */
typedef uint64_t ResonateMessageSequence;

/* Consumer-owned read position; a plain value, comparable against a stream's
   oldest retained sequence. */
typedef struct ResonateMessageCursor
{
    ResonateMessageSequence sequence;
} ResonateMessageCursor;

/* A consumer's view onto a stream. The vtable belongs to the producer. */
typedef struct ResonateMessageStream
{
    const ResonateMessageStreamVTable* vtable;
    void* self;
} ResonateMessageStream;

/* A module's only channel to the outside world. Allocated by the host, valid
   from on_attach until on_detach. */
typedef struct ResonateHostApi
{
    uint32_t struct_size;
    uint32_t abi_version;
    void* user_data;

    /* The only legal source of memory that crosses the boundary. Reclaimed by the
       host; a module must not free host memory itself. */
    void* (*allocate)(void* user_data, size_t size, size_t alignment);
    void (*deallocate)(void* user_data, void* memory, size_t size);

    /* Resolves a capability published by another module. Returns NULL only for an
       id in the manifest's "optional" list; "requires" ids are guaranteed to
       resolve before on_attach, so a NULL there is a host bug and the module
       should return RESONATE_E_MISSING.
     *
     * The result is owned by the provider and stays valid until on_detach. */
    void* (*query_interface)(void* user_data, ResonateId id, uint32_t min_version,
                             uint32_t* out_version);

    /* Returns RESONATE_E_MISSING when the key is unset, distinguishing "use this
       default" from "explicitly empty". out_buffer is NUL-terminated on success. */
    ResonateStatus (*read_config)(void* user_data, const char* section, const char* key,
                                  char* out_buffer, size_t buffer_size);

    /* Publishes one of this module's capabilities for as long as it is attached.
       The id must appear in the module's manifest "provides", or the host refuses
       the registration: a capability nobody declared cannot have been required by
       anyone, so publishing it is a contract violation rather than a bonus.
       Returns RESONATE_E_INVALID if the id is already taken. */
    ResonateStatus (*register_capability)(void* user_data, const ResonateCapabilityRecord* record);

    /* Withdraws a capability registered by this module. Unregisters only if the
       instance matches. */
    void (*unregister_capability)(void* user_data, ResonateId id, void* instance);

    ResonateLogFn log;

    /*
     * Module facilities: storage a module creates for itself, such as a signal it
     * raises or a message stream it produces. It comes from the host allocator
     * like everything else that crosses the boundary.
     *
     * A module reaches them here rather than by calling into the host library, so
     * these entry points exist exactly once and the host knows every piece of
     * storage a module created. Anything still alive at on_detach is reclaimed
     * there, which is why destroying it is optional.
     */

    ResonateStatus (*signal_create)(void* user_data, ResonateSignalStorage** out_storage);

    /* Releases it early. The host owns the storage and destroys whatever is left
       at on_detach, so this is a request rather than a handover, and one naming
       storage the host is not holding is ignored rather than freed. */
    void (*signal_destroy)(void* user_data, ResonateSignalStorage* storage);

    /* The node is written to and must outlive the connection. Fails with
       RESONATE_E_INVALID if that node is already connected. */
    ResonateStatus (*signal_connect)(void* user_data, ResonateSignalStorage* storage,
                                     ResonateSignalNode* node, ResonateSignalInvoke invoke);
    void (*signal_disconnect)(void* user_data, ResonateSignalStorage* storage,
                              ResonateSignalNode* node);

    /* Returns 1 if at least one subscriber ran, 0 if there were none. */
    uint32_t (*signal_emit)(void* user_data, ResonateSignalStorage* storage, const void* payload);

    /* Stops at the first subscriber that returns non-zero, and returns that
       value. */
    uint32_t (*signal_emit_until)(void* user_data, ResonateSignalStorage* storage,
                                  const void* payload);

    uint32_t (*signal_subscriber_count)(void* user_data, const ResonateSignalStorage* storage);

    /* A ring of slot_count slots, each holding one record up to slot_bytes with
       the header included; a record that does not fit is rejected, not truncated.
       Writing into a full ring evicts the oldest record. */
    ResonateStatus (*message_writer_create)(void* user_data, ResonateMessageWriter** out_writer,
                                            uint32_t slot_bytes, uint32_t slot_count);
    void (*message_writer_destroy)(void* user_data, ResonateMessageWriter* writer);

    /* Copies the payload into stream-owned storage, so the producer's buffer is
       free on return. Returns the assigned sequence, or 0 on failure. */
    ResonateMessageSequence (*message_write)(void* user_data, ResonateMessageWriter* writer,
                                             uint32_t type_id, const void* payload,
                                             uint32_t payload_size);

    /* Discards everything retained; cursors are not rewound. */
    void (*message_clear)(void* user_data, ResonateMessageWriter* writer);

    ResonateStatus (*message_open_reader)(void* user_data, ResonateMessageWriter* writer,
                                          ResonateMessageStream* out_stream);

    /* The stream is a view onto its writer and must not be used once the writer
       is destroyed, which closes any reader still open on it. */
    void (*message_close_reader)(void* user_data, ResonateMessageStream* stream);

    /* Fresh cursor positioned before the oldest retained message. */
    ResonateMessageCursor (*message_cursor_begin)(void* user_data,
                                                  const ResonateMessageStream* stream);

    /*
     * Per-frame work: a system this module contributes to the host's schedule.
     * The host copies the descriptor's name, so the descriptor can be a
     * temporary; the run function and context stay the caller's and must
     * outlive the registration. A system runs every frame from registration
     * until removed or the module detaches, and one the module leaves behind is
     * withdrawn at on_detach.
     */

    ResonateStatus (*system_add)(void* user_data, const ResonateSystemDesc* desc);

    /* Removes every system this module registered under name. A name the module
       never registered is reported, not silently ignored. */
    void (*system_remove)(void* user_data, const char* name);

    /* The run's ECS world, or NULL when this run has none: what a module asks
       before registering a world-aware system, which cannot be registered
       without one. The handle lives as long as the host. */
    ResonateWorld* (*world)(void* user_data);
} ResonateHostApi;

typedef struct ResonateModuleInfo
{
    uint32_t struct_size;

    /* Reverse-DNS and globally unique: "resonate.physics". Identity everywhere,
       including the manifest and the dependency graph. */
    const char* id;

    /* Display name for logs and editor lists: "Physics". Carries no identity; two
       modules may share one. */
    const char* name;

    /* Semantic version of the module, not of the ABI. */
    const char* version;

    /* Major ABI version the module was built against. */
    uint32_t abi_version;

    /* Lowest host major ABI this module accepts. */
    uint32_t min_host_abi;
} ResonateModuleInfo;

/*
 * Lifecycle callbacks, in the order the host calls them:
 *
 *   on_attach   dependencies are attached and their capabilities resolvable.
 *               Take and hold what is needed; the frame path must not query.
 *   on_detach   release everything. Called in exact reverse topological order.
 *
 * Both run on the main thread and must not throw.
 */
typedef struct ResonateModuleVTable
{
    uint32_t struct_size;

    ResonateStatus (*on_attach)(const ResonateHostApi* host);
    void (*on_detach)(void);
} ResonateModuleVTable;

#if defined(_WIN32)
#    define RESONATE_MODULE_EXPORT __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#    define RESONATE_MODULE_EXPORT __attribute__((visibility("default")))
#else
#    define RESONATE_MODULE_EXPORT
#endif

/* The two symbols a module library exports, and the only two. Both must be
   present and outside any namespace. */
RESONATE_MODULE_EXPORT const ResonateModuleInfo* resonateModuleInfo(uint32_t host_abi);
RESONATE_MODULE_EXPORT const ResonateModuleVTable* resonateModuleVTable(void);

/* resonateModuleInfo receives RESONATE_ABI_VERSION; returning NULL refuses the
   load. The returned struct stays valid for the lifetime of the library. */

#ifdef __cplusplus
} /* extern "C" */

constexpr bool operator==(const ResonateId& a, const ResonateId& b) noexcept
{
    return a.lo == b.lo && a.hi == b.hi;
}
#endif

#endif /* RESONATE_MODULE_ABI_H */
