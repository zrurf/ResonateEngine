#include "resonate/module/host.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <yyjson.h>

#include <resonate/module/message.h>
#include <resonate/module/signal.h>
#include <resonate/pal/io.h>

namespace resonate
{
namespace
{

bool sameId(ResonateId left, ResonateId right)
{
    return left.lo == right.lo && left.hi == right.hi;
}

std::string idText(ResonateId id)
{
    char text[40] = {};
    std::snprintf(text, sizeof(text), "%016llx%016llx", static_cast<unsigned long long>(id.hi),
                  static_cast<unsigned long long>(id.lo));
    return text;
}

std::string describe(const ResonateCapabilityRecord* record)
{
    return record->name != nullptr ? std::string(record->name) : idText(record->id);
}

/* A symbol address is a data pointer whatever it really is; copying it into the
   function pointer keeps the conversion defined. */
template <typename Fn> Fn symbolAs(const ResonateLibrary& library, const char* name)
{
    static_assert(sizeof(Fn) == sizeof(void*),
                  "a function pointer and a data pointer are the same size here");
    void* address = resonate_pal_library_symbol(&library, name);
    Fn function = nullptr;
    std::memcpy(&function, &address, sizeof(function));
    return function;
}

bool readFile(const std::string& path, std::string& out)
{
    ResonateFileHandle file = {};
    if (resonate_pal_io_open_file(&file, path.c_str(), RESONATE_FILE_READ) != RESONATE_PAL_OK)
    {
        return false;
    }

    uint64_t size = 0;
    if (resonate_pal_io_size(&file, &size) != RESONATE_PAL_OK)
    {
        resonate_pal_io_close_file(&file);
        return false;
    }

    out.assign(static_cast<std::size_t>(size), '\0');
    uint64_t read = 0;
    const ResonatePalStatus status =
        size == 0U ? RESONATE_PAL_OK : resonate_pal_io_read(&file, out.data(), size, &read);
    resonate_pal_io_close_file(&file);

    return status == RESONATE_PAL_OK && read == size;
}

yyjson_val* field(yyjson_val* object, const char* name)
{
    return yyjson_is_obj(object) ? yyjson_obj_get(object, name) : nullptr;
}

bool readString(yyjson_val* object, const char* name, std::string& out)
{
    yyjson_val* value = field(object, name);
    if (value == nullptr || !yyjson_is_str(value))
    {
        return false;
    }
    out = yyjson_get_str(value);
    return true;
}

bool readUint(yyjson_val* object, const char* name, std::uint32_t& out)
{
    yyjson_val* value = field(object, name);
    if (value == nullptr || !yyjson_is_uint(value) || yyjson_get_uint(value) > 0xFFFFFFFFULL)
    {
        return false;
    }
    out = static_cast<std::uint32_t>(yyjson_get_uint(value));
    return true;
}

/* Capability names arrive as strings and are hashed here, so a manifest and a
   descriptor agree on an id without sharing a table. The name is kept beside the
   hash for the diagnostics that would otherwise report only the hash. */
bool readCapabilities(yyjson_val* object, const char* name, std::vector<ModuleCapability>& out)
{
    yyjson_val* array = field(object, name);
    if (array == nullptr || !yyjson_is_arr(array))
    {
        return false;
    }

    yyjson_arr_iter iterator;
    yyjson_arr_iter_init(array, &iterator);
    while (yyjson_val* entry = yyjson_arr_iter_next(&iterator))
    {
        if (!yyjson_is_str(entry))
        {
            return false;
        }

        const char* const text = yyjson_get_str(entry);
        out.push_back(ModuleCapability{text, Id(resonate_id_make(text))});
    }
    return true;
}

bool readNames(yyjson_val* object, const char* name, std::vector<std::string>& out)
{
    yyjson_val* array = field(object, name);
    if (array == nullptr || !yyjson_is_arr(array))
    {
        return false;
    }

    yyjson_arr_iter iterator;
    yyjson_arr_iter_init(array, &iterator);
    while (yyjson_val* entry = yyjson_arr_iter_next(&iterator))
    {
        if (!yyjson_is_str(entry))
        {
            return false;
        }
        out.emplace_back(yyjson_get_str(entry));
    }
    return true;
}

} // namespace

struct ModuleHost::Impl
{
    /* One per module, so every callback the module gets knows who made it. */
    struct Context
    {
        Impl* impl = nullptr;
        ModuleRecord* record = nullptr;
        ResonateHostApi api = {};
        std::vector<std::pair<ResonateId, void*>> published;

        /* Facility storage this module created: reclaimed at detach whether or
           not the module destroyed it, so nothing it made outlives it. */
        std::vector<ResonateSignalStorage*> signals;
        std::vector<ResonateMessageWriter*> writers;

        /* Host memory this module holds: same treatment, and what lets a module
           treat giving it back as optional. */
        std::vector<std::pair<void*, std::size_t>> allocations;
    };

    Allocator* allocator = nullptr;
    ResonateCapabilityRegistry* registry = nullptr;
    ResonateScheduler* scheduler = nullptr;
    ModuleGraph graph;
    std::vector<ModuleRecord>* records = nullptr;
    std::vector<Context*> contexts;
    ResonateHostApi host_api = {};
    void* log_user_data = nullptr;
    ResonateLogFn log_sink = nullptr;

    void write(int32_t level, const std::string& message)
    {
        if (log_sink != nullptr)
        {
            log_sink(log_user_data, level, __FILE__, __LINE__, message.c_str());
            return;
        }
        std::fprintf(stderr, "resonate: %s\n", message.c_str());
    }

    void writeFrom(int32_t level, const std::string& module, const std::string& message)
    {
        write(level, module + ": " + message);
    }

    Context* contextOf(ModuleRecord& record)
    {
        for (Context* context : contexts)
        {
            if (context->record == &record)
            {
                return context;
            }
        }

        auto* context = new Context();
        context->impl = this;
        context->record = &record;
        context->api = host_api;
        context->api.user_data = context;

        /* The module-facing callbacks read the user_data as a Context, not as
           this Impl, so they have to be the ones installed here. Defined below,
           where the host-side ones are in scope. */
        context->api.allocate = &Impl::allocateForModule;
        context->api.deallocate = &Impl::deallocateForModule;
        context->api.query_interface = &Impl::queryForModule;
        context->api.register_capability = &Impl::registerForModule;
        context->api.unregister_capability = &Impl::unregisterForModule;
        context->api.log = &Impl::logForModule;
        context->api.read_config = &Impl::readConfigForModule;

        context->api.signal_create = &Impl::signalCreateForModule;
        context->api.signal_destroy = &Impl::signalDestroyForModule;
        context->api.signal_connect = &Impl::signalConnectForModule;
        context->api.signal_disconnect = &Impl::signalDisconnectForModule;
        context->api.signal_emit = &Impl::signalEmitForModule;
        context->api.signal_emit_until = &Impl::signalEmitUntilForModule;
        context->api.signal_subscriber_count = &Impl::signalSubscriberCountForModule;
        context->api.message_writer_create = &Impl::messageWriterCreateForModule;
        context->api.message_writer_destroy = &Impl::messageWriterDestroyForModule;
        context->api.message_write = &Impl::messageWriteForModule;
        context->api.message_clear = &Impl::messageClearForModule;
        context->api.message_open_reader = &Impl::messageOpenReaderForModule;
        context->api.message_close_reader = &Impl::messageCloseReaderForModule;
        context->api.message_cursor_begin = &Impl::messageCursorBeginForModule;

        contexts.push_back(context);
        return context;
    }

    static void* allocateForModule(void* user_data, std::size_t size, std::size_t alignment);
    static void deallocateForModule(void* user_data, void* memory, std::size_t size);
    static void* queryForModule(void* user_data, ResonateId id, uint32_t min_version,
                                uint32_t* out_version);
    static ResonateStatus registerForModule(void* user_data,
                                            const ResonateCapabilityRecord* record);
    static void unregisterForModule(void* user_data, ResonateId id, void* instance);
    static void logForModule(void* user_data, int32_t level, const char* file, int32_t line,
                             const char* message);
    static ResonateStatus readConfigForModule(void* user_data, const char* section, const char* key,
                                              char* out_buffer, std::size_t buffer_size);

    /* Facility entry points. Only the two that allocate have to be resolved
       through the context; the rest are here so a module reaches them without
       linking the host, and forward to the implementation unchanged. */
    static ResonateStatus signalCreateForModule(void* user_data,
                                                ResonateSignalStorage** out_storage);
    static void signalDestroyForModule(void* user_data, ResonateSignalStorage* storage);
    static ResonateStatus signalConnectForModule(void* user_data, ResonateSignalStorage* storage,
                                                 ResonateSignalNode* node,
                                                 ResonateSignalInvoke invoke);
    static void signalDisconnectForModule(void* user_data, ResonateSignalStorage* storage,
                                          ResonateSignalNode* node);
    static uint32_t signalEmitForModule(void* user_data, ResonateSignalStorage* storage,
                                        const void* payload);
    static uint32_t signalEmitUntilForModule(void* user_data, ResonateSignalStorage* storage,
                                             const void* payload);
    static uint32_t signalSubscriberCountForModule(void* user_data,
                                                   const ResonateSignalStorage* storage);

    static ResonateStatus messageWriterCreateForModule(void* user_data,
                                                       ResonateMessageWriter** out_writer,
                                                       uint32_t slot_bytes, uint32_t slot_count);
    static void messageWriterDestroyForModule(void* user_data, ResonateMessageWriter* writer);
    static ResonateMessageSequence messageWriteForModule(void* user_data,
                                                         ResonateMessageWriter* writer,
                                                         uint32_t type_id, const void* payload,
                                                         uint32_t payload_size);
    static void messageClearForModule(void* user_data, ResonateMessageWriter* writer);
    static ResonateStatus messageOpenReaderForModule(void* user_data, ResonateMessageWriter* writer,
                                                     ResonateMessageStream* out_stream);
    static void messageCloseReaderForModule(void* user_data, ResonateMessageStream* stream);
    static ResonateMessageCursor messageCursorBeginForModule(void* user_data,
                                                             const ResonateMessageStream* stream);

    /* Withdraws every host resource this module took: the capabilities it
       published and the facility storage it created. Runs for a module that
       attached and for one whose on_attach failed, because both can have taken
       resources before stopping. */
    void reclaim(ModuleRecord& record);
};

namespace
{

void* allocateMemory(void* user_data, std::size_t size, std::size_t alignment)
{
    return static_cast<ModuleHost::Impl*>(user_data)->allocator->allocate(size, alignment);
}

void deallocateMemory(void* user_data, void* memory, std::size_t size)
{
    static_cast<ModuleHost::Impl*>(user_data)->allocator->deallocate(memory, size);
}

void* queryInterface(void* user_data, ResonateId id, uint32_t min_version, uint32_t* out_version)
{
    return resonate_capability_find(static_cast<ModuleHost::Impl*>(user_data)->registry, id,
                                    min_version, out_version);
}

/* The host reads no configuration file yet, so every key is unset and a module
   falls back to its own default. */
ResonateStatus readConfig(void*, const char*, const char*, char*, std::size_t)
{
    return RESONATE_E_MISSING;
}

ResonateStatus registerCapability(void* user_data, const ResonateCapabilityRecord* record)
{
    return record != nullptr ? resonate_capability_register(
                                   static_cast<ModuleHost::Impl*>(user_data)->registry, record)
                             : RESONATE_E_INVALID;
}

void unregisterCapability(void* user_data, ResonateId id, void* instance)
{
    resonate_capability_unregister(static_cast<ModuleHost::Impl*>(user_data)->registry, id,
                                   instance);
}

void writeLog(void* user_data, int32_t level, const char*, int32_t, const char* message)
{
    static_cast<ModuleHost::Impl*>(user_data)->write(level, message != nullptr ? message : "");
}

} // namespace

/* The module-facing half of the same services: a module's Context is the
   user_data, and these must be the pointers installed in its api. */
void* ModuleHost::Impl::allocateForModule(void* user_data, std::size_t size, std::size_t alignment)
{
    auto* context = static_cast<ModuleHost::Impl::Context*>(user_data);
    void* memory = allocateMemory(context->impl, size, alignment);
    if (memory != nullptr)
    {
        context->allocations.emplace_back(memory, size);
    }
    return memory;
}

void ModuleHost::Impl::deallocateForModule(void* user_data, void* memory, std::size_t size)
{
    auto* context = static_cast<ModuleHost::Impl::Context*>(user_data);
    if (memory == nullptr)
    {
        return;
    }

    /* Only what the host is holding is freed, which is what makes a module's own
       release of memory the host already reclaimed at detach a reported no-op
       rather than a second free. */
    std::vector<std::pair<void*, std::size_t>>& allocations = context->allocations;
    const auto found = std::find_if(allocations.begin(), allocations.end(),
                                    [memory](const std::pair<void*, std::size_t>& entry)
                                    { return entry.first == memory; });
    if (found == allocations.end())
    {
        context->impl->writeFrom(RESONATE_LOG_WARN, context->record->manifest.id,
                                 "released memory it does not hold");
        return;
    }

    allocations.erase(found);
    deallocateMemory(context->impl, memory, size);
}

void* ModuleHost::Impl::queryForModule(void* user_data, ResonateId id, uint32_t min_version,
                                       uint32_t* out_version)
{
    auto* context = static_cast<ModuleHost::Impl::Context*>(user_data);
    void* instance = queryInterface(context->impl, id, min_version, out_version);
    if (instance != nullptr)
    {
        return instance;
    }

    /* A required capability that does not resolve is a host bug, and saying so is
       cheaper than the null dereference the module would reach. An optional one is
       expected to be absent, so it is not reported. */
    const std::vector<ModuleCapability>& requirements = context->record->manifest.requirements;
    for (const ModuleCapability& entry : requirements)
    {
        if (sameId(entry.id.value(), id))
        {
            context->impl->writeFrom(RESONATE_LOG_ERROR, context->record->manifest.id,
                                     "asked for required capability " + entry.name +
                                         ", which nothing provides");
            break;
        }
    }
    return nullptr;
}

ResonateStatus ModuleHost::Impl::registerForModule(void* user_data,
                                                   const ResonateCapabilityRecord* record)
{
    auto* context = static_cast<ModuleHost::Impl::Context*>(user_data);
    if (record == nullptr)
    {
        return RESONATE_E_INVALID;
    }

    /* A module may publish only what its manifest declares: nobody can have
       required a capability that was never announced. */
    const std::vector<ModuleCapability>& declared = context->record->manifest.provides;
    const bool listed =
        std::any_of(declared.begin(), declared.end(), [record](const ModuleCapability& entry)
                    { return sameId(entry.id.value(), record->id); });
    if (!listed)
    {
        context->impl->writeFrom(RESONATE_LOG_ERROR, context->record->manifest.id,
                                 "tried to publish " + describe(record) +
                                     ", which its manifest does not declare");
        return RESONATE_E_INVALID;
    }

    const ResonateStatus status = resonate_capability_register(context->impl->registry, record);
    if (status == RESONATE_OK)
    {
        context->published.emplace_back(record->id, record->instance);
    }
    return status;
}

void ModuleHost::Impl::unregisterForModule(void* user_data, ResonateId id, void* instance)
{
    auto* context = static_cast<ModuleHost::Impl::Context*>(user_data);
    resonate_capability_unregister(context->impl->registry, id, instance);

    std::vector<std::pair<ResonateId, void*>>& published = context->published;
    published.erase(std::remove_if(published.begin(), published.end(),
                                   [id, instance](const std::pair<ResonateId, void*>& entry)
                                   { return sameId(entry.first, id) && entry.second == instance; }),
                    published.end());
}

void ModuleHost::Impl::logForModule(void* user_data, int32_t level, const char*, int32_t,
                                    const char* message)
{
    auto* context = static_cast<Context*>(user_data);
    context->impl->writeFrom(level, context->record->manifest.id,
                             message != nullptr ? message : "");
}

ResonateStatus ModuleHost::Impl::readConfigForModule(void* user_data, const char* section,
                                                     const char* key, char* out_buffer,
                                                     std::size_t buffer_size)
{
    return readConfig(static_cast<Context*>(user_data)->impl, section, key, out_buffer,
                      buffer_size);
}

ResonateStatus ModuleHost::Impl::signalCreateForModule(void* user_data,
                                                       ResonateSignalStorage** out_storage)
{
    auto* context = static_cast<Context*>(user_data);
    const ResonateStatus status = resonate_signal_create(out_storage, &context->impl->host_api);
    if (status == RESONATE_OK)
    {
        context->signals.push_back(*out_storage);
    }
    return status;
}

void ModuleHost::Impl::signalDestroyForModule(void* user_data, ResonateSignalStorage* storage)
{
    auto* context = static_cast<Context*>(user_data);
    if (storage == nullptr)
    {
        return;
    }

    /* Tracking is what makes a second destroy a no-op rather than a second free,
       and a foreign pointer a reported mistake rather than corruption. */
    const auto found = std::find(context->signals.begin(), context->signals.end(), storage);
    if (found == context->signals.end())
    {
        context->impl->writeFrom(RESONATE_LOG_WARN, context->record->manifest.id,
                                 "destroyed a signal it does not hold");
        return;
    }

    context->signals.erase(found);
    resonate_signal_destroy(storage);
}

ResonateStatus ModuleHost::Impl::signalConnectForModule(void*, ResonateSignalStorage* storage,
                                                        ResonateSignalNode* node,
                                                        ResonateSignalInvoke invoke)
{
    return resonate_signal_connect(storage, node, invoke);
}

void ModuleHost::Impl::signalDisconnectForModule(void*, ResonateSignalStorage* storage,
                                                 ResonateSignalNode* node)
{
    resonate_signal_disconnect(storage, node);
}

uint32_t ModuleHost::Impl::signalEmitForModule(void*, ResonateSignalStorage* storage,
                                               const void* payload)
{
    return resonate_signal_emit(storage, payload);
}

uint32_t ModuleHost::Impl::signalEmitUntilForModule(void*, ResonateSignalStorage* storage,
                                                    const void* payload)
{
    return resonate_signal_emit_until(storage, payload);
}

uint32_t ModuleHost::Impl::signalSubscriberCountForModule(void*,
                                                          const ResonateSignalStorage* storage)
{
    return resonate_signal_subscriber_count(storage);
}

ResonateStatus ModuleHost::Impl::messageWriterCreateForModule(void* user_data,
                                                              ResonateMessageWriter** out_writer,
                                                              uint32_t slot_bytes,
                                                              uint32_t slot_count)
{
    auto* context = static_cast<Context*>(user_data);
    const ResonateStatus status = resonate_message_writer_create(
        out_writer, &context->impl->host_api, slot_bytes, slot_count);
    if (status == RESONATE_OK)
    {
        context->writers.push_back(*out_writer);
    }
    return status;
}

void ModuleHost::Impl::messageWriterDestroyForModule(void* user_data, ResonateMessageWriter* writer)
{
    auto* context = static_cast<Context*>(user_data);
    if (writer == nullptr)
    {
        return;
    }

    const auto found = std::find(context->writers.begin(), context->writers.end(), writer);
    if (found == context->writers.end())
    {
        context->impl->writeFrom(RESONATE_LOG_WARN, context->record->manifest.id,
                                 "destroyed a message writer it does not hold");
        return;
    }

    context->writers.erase(found);
    resonate_message_writer_destroy(writer);
}

ResonateMessageSequence
ModuleHost::Impl::messageWriteForModule(void*, ResonateMessageWriter* writer, uint32_t type_id,
                                        const void* payload, uint32_t payload_size)
{
    return resonate_message_write(writer, type_id, payload, payload_size);
}

void ModuleHost::Impl::messageClearForModule(void*, ResonateMessageWriter* writer)
{
    resonate_message_clear(writer);
}

ResonateStatus ModuleHost::Impl::messageOpenReaderForModule(void*, ResonateMessageWriter* writer,
                                                            ResonateMessageStream* out_stream)
{
    return resonate_message_open_reader(writer, out_stream);
}

void ModuleHost::Impl::messageCloseReaderForModule(void*, ResonateMessageStream* stream)
{
    resonate_message_close_reader(stream);
}

ResonateMessageCursor
ModuleHost::Impl::messageCursorBeginForModule(void*, const ResonateMessageStream* stream)
{
    return resonate_message_cursor_begin(stream);
}

ResonateStatus parseManifest(std::string_view json_text, ModuleManifest& out_manifest)
{
    out_manifest = ModuleManifest{};

    yyjson_doc* document = yyjson_read(json_text.data(), json_text.size(), 0);
    if (document == nullptr)
    {
        return RESONATE_E_INVALID;
    }

    yyjson_val* root = yyjson_doc_get_root(document);
    const bool complete = yyjson_is_obj(root) && readString(root, "id", out_manifest.id) &&
                          readString(root, "name", out_manifest.name) &&
                          readString(root, "version", out_manifest.version) &&
                          readUint(root, "abi", out_manifest.abi) &&
                          readUint(root, "min_host_abi", out_manifest.min_host_abi) &&
                          readCapabilities(root, "provides", out_manifest.provides) &&
                          readCapabilities(root, "requires", out_manifest.requirements) &&
                          readCapabilities(root, "optional", out_manifest.optional) &&
                          readNames(root, "depends_on", out_manifest.depends_on);

    yyjson_doc_free(document);
    return complete ? RESONATE_OK : RESONATE_E_INVALID;
}

std::unique_ptr<ModuleHost> ModuleHost::create(Allocator& allocator)
{
    auto host = std::unique_ptr<ModuleHost>(new ModuleHost());
    host->impl_ = std::make_unique<Impl>();

    Impl* impl = host->impl_.get();
    impl->allocator = &allocator;
    impl->records = &host->records_;

    impl->host_api.struct_size = sizeof(ResonateHostApi);
    impl->host_api.abi_version = RESONATE_ABI_VERSION;
    impl->host_api.user_data = impl;
    impl->host_api.allocate = &allocateMemory;
    impl->host_api.deallocate = &deallocateMemory;
    impl->host_api.query_interface = &queryInterface;
    impl->host_api.read_config = &readConfig;
    impl->host_api.register_capability = &registerCapability;
    impl->host_api.unregister_capability = &unregisterCapability;
    impl->host_api.log = &writeLog;

    /* The facility entry points are left null here on purpose: they are the
       module-facing half, installed per module in contextOf, and the host reaches
       the implementations directly. Signal and writer storage keeps this copy
       only to allocate from. */

    if (resonate_capability_registry_create(&impl->registry, &impl->host_api) != RESONATE_OK)
    {
        return nullptr;
    }
    if (resonate_scheduler_create(&impl->scheduler, &impl->host_api) != RESONATE_OK)
    {
        resonate_capability_registry_destroy(impl->registry);
        /* Cleared because returning nullptr destroys this host, and the destructor
           would otherwise destroy the registry a second time. */
        impl->registry = nullptr;
        return nullptr;
    }

    return host;
}

ModuleHost::~ModuleHost()
{
    detachAll();

    /* Libraries go first, contexts after: unloading one runs the static
       destructors inside it, and those may still call into the host — releasing a
       signal, say — so what they reach has to be alive at that point. */
    for (ModuleRecord& record : records_)
    {
        resonate_pal_library_unload(&record.library);
    }
    for (Impl::Context* context : impl_->contexts)
    {
        delete context;
    }

    resonate_scheduler_destroy(impl_->scheduler);
    resonate_capability_registry_destroy(impl_->registry);
}

ResonateStatus ModuleHost::discover(const std::string& plugin_directory)
{
    Impl* impl = impl_.get();
    const std::string suffix = std::string(".") + resonate_pal_library_extension();

    ResonateDirHandle directory = {};
    if (resonate_pal_io_enumerate(&directory, plugin_directory.c_str()) != RESONATE_PAL_OK)
    {
        impl->write(RESONATE_LOG_ERROR, "cannot read plugin directory " + plugin_directory);
        return RESONATE_E_MISSING;
    }

    const char* entry = nullptr;
    while (resonate_pal_io_enumerate_next(&directory, &entry) == RESONATE_PAL_OK)
    {
        const std::string name(entry);
        if (name.size() <= suffix.size() ||
            name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0)
        {
            continue;
        }

        const std::string path = plugin_directory + "/" + name;
        ResonateLibrary library = {};
        if (resonate_pal_library_load(&library, path.c_str()) != RESONATE_PAL_OK)
        {
            impl->write(RESONATE_LOG_ERROR, "cannot load " + path);
            continue;
        }

        auto info =
            symbolAs<const ResonateModuleInfo* (*)(uint32_t)>(library, "resonateModuleInfo");
        auto vtable =
            symbolAs<const ResonateModuleVTable* (*)(void)>(library, "resonateModuleVTable");
        if (info == nullptr || vtable == nullptr)
        {
            /* Anything else in the directory that happens to be a shared library
               is not a module: reported and skipped, not fatal. */
            impl->write(RESONATE_LOG_WARN, name + " does not export the two module entry points");
            resonate_pal_library_unload(&library);
            continue;
        }

        const ResonateModuleInfo* identity = info(RESONATE_ABI_VERSION);
        if (identity == nullptr || identity->id == nullptr ||
            identity->struct_size < sizeof(ResonateModuleInfo))
        {
            impl->write(RESONATE_LOG_WARN, name + " refused the host ABI");
            resonate_pal_library_unload(&library);
            continue;
        }

        const bool compatible = identity->abi_version <= RESONATE_ABI_VERSION_MAJOR &&
                                identity->min_host_abi <= RESONATE_ABI_VERSION_MAJOR;
        if (!compatible)
        {
            impl->write(RESONATE_LOG_WARN, std::string(identity->id) +
                                               " wants a newer host ABI than this host implements");
            resonate_pal_library_unload(&library);
            continue;
        }

        std::string text;
        if (!readFile(plugin_directory + "/" + identity->id + ".json", text))
        {
            impl->write(RESONATE_LOG_ERROR,
                        std::string(identity->id) + " has no manifest beside its library");
            resonate_pal_library_unload(&library);
            continue;
        }

        ModuleManifest manifest;
        if (parseManifest(text, manifest) != RESONATE_OK)
        {
            impl->write(RESONATE_LOG_ERROR,
                        std::string(identity->id) + "'s manifest is missing a required field");
            resonate_pal_library_unload(&library);
            continue;
        }
        if (manifest.id != identity->id)
        {
            impl->write(RESONATE_LOG_ERROR, "manifest id '" + manifest.id +
                                                "' does not match the library's '" + identity->id +
                                                "'");
            resonate_pal_library_unload(&library);
            continue;
        }
        if (identity->version != nullptr && manifest.version != identity->version)
        {
            impl->write(RESONATE_LOG_ERROR, manifest.id + "'s manifest says version " +
                                                manifest.version + " but its descriptor says " +
                                                identity->version);
            resonate_pal_library_unload(&library);
            continue;
        }

        /* The symbol existing does not mean it returns anything usable: a module
           may decline to hand out a lifecycle table, and one built against a
           shorter layout has to be read no further than it wrote. */
        const ResonateModuleVTable* const table = vtable();
        if (table == nullptr || table->struct_size < sizeof(ResonateModuleVTable) ||
            table->on_attach == nullptr || table->on_detach == nullptr)
        {
            impl->write(RESONATE_LOG_WARN, name + " has no usable module vtable");
            resonate_pal_library_unload(&library);
            continue;
        }

        ModuleRecord record;
        record.manifest = manifest;
        record.library_path = path;
        record.library = library;
        record.info = identity;
        record.vtable = table;
        records_.push_back(record);
    }

    resonate_pal_io_close_directory(&directory);
    return RESONATE_OK;
}

ResonateStatus ModuleHost::resolve()
{
    Impl* impl = impl_.get();

    const ResonateStatus built = impl->graph.build(records_);
    if (built != RESONATE_OK)
    {
        impl->write(RESONATE_LOG_ERROR, impl->graph.error());
        return built;
    }

    /* A required capability resolves if the host already provides it, or if some
       discovered module declares it; in the second case the graph has ordered the
       provider before its consumers. */
    std::vector<ResonateId> declared;
    for (const ModuleRecord& record : records_)
    {
        for (const ModuleCapability& provided : record.manifest.provides)
        {
            declared.push_back(provided.id.value());
        }
    }

    for (const ModuleRecord& record : records_)
    {
        for (const ModuleCapability& requirement : record.manifest.requirements)
        {
            const ResonateId id = requirement.id.value();
            if (resonate_capability_find(impl->registry, id, 0, nullptr) != nullptr)
            {
                continue;
            }

            const bool announced =
                std::any_of(declared.begin(), declared.end(),
                            [id](const ResonateId& entry) { return sameId(entry, id); });
            if (!announced)
            {
                impl->write(RESONATE_LOG_ERROR, record.manifest.id + " requires " +
                                                    requirement.name + ", which nothing provides");
                return RESONATE_E_MISSING;
            }
        }
    }

    return RESONATE_OK;
}

ResonateStatus ModuleHost::attachAll()
{
    Impl* impl = impl_.get();

    for (ModuleRecord* record : impl->graph.order())
    {
        Impl::Context* context = impl->contextOf(*record);
        const ResonateStatus status = record->vtable->on_attach(&context->api);
        if (status != RESONATE_OK)
        {
            impl->writeFrom(RESONATE_LOG_ERROR, record->manifest.id,
                            "failed to attach, status " + std::to_string(status));

            /* A module that fails partway has usually taken host resources first,
               and detachAll() below skips it because it never attached. Its own
               state is its own to unwind — it knows why it failed — but what the
               host handed it is withdrawn here, or a retry would find its own ids
               taken. */
            impl->reclaim(*record);
            detachAll();
            return status;
        }
        record->attached = true;
        impl->writeFrom(RESONATE_LOG_INFO, record->manifest.id, "attached");
    }

    return RESONATE_OK;
}

void ModuleHost::detachAll()
{
    Impl* impl = impl_.get();

    /* Reverse attach order, so a module is torn down before anything it used. */
    const std::vector<ModuleRecord*>& order = impl->graph.order();
    for (auto entry = order.rbegin(); entry != order.rend(); ++entry)
    {
        ModuleRecord* record = *entry;
        if (!record->attached)
        {
            continue;
        }

        record->vtable->on_detach();
        record->attached = false;
        impl->writeFrom(RESONATE_LOG_INFO, record->manifest.id, "detached");

        /* Whatever the module left behind is withdrawn here, or the next load of
           it would find its own ids taken. */
        impl->reclaim(*record);
    }
}

void ModuleHost::Impl::reclaim(ModuleRecord& record)
{
    Context* context = contextOf(record);

    while (!context->published.empty())
    {
        const std::pair<ResonateId, void*> leftover = context->published.back();
        context->published.pop_back();
        writeFrom(RESONATE_LOG_WARN, record.manifest.id,
                  "left capability " + idText(leftover.first) + " registered through detach");
        resonate_capability_unregister(registry, leftover.first, leftover.second);
    }

    /* Facility storage gets the same treatment: a signal or a stream the module
       did not destroy is destroyed here, so a module's leak stays inside its own
       lifetime instead of becoming the host's. */
    while (!context->signals.empty())
    {
        ResonateSignalStorage* const storage = context->signals.back();
        context->signals.pop_back();
        writeFrom(RESONATE_LOG_WARN, record.manifest.id, "left a signal alive through detach");
        resonate_signal_destroy(storage);
    }
    while (!context->writers.empty())
    {
        ResonateMessageWriter* const writer = context->writers.back();
        context->writers.pop_back();
        writeFrom(RESONATE_LOG_WARN, record.manifest.id,
                  "left a message writer alive through detach");
        resonate_message_writer_destroy(writer);
    }

    /* And the memory: everything the module took from the allocator and did not
       give back, which is what makes holding it until detach legal. */
    while (!context->allocations.empty())
    {
        const std::pair<void*, std::size_t> leftover = context->allocations.back();
        context->allocations.pop_back();
        writeFrom(RESONATE_LOG_WARN, record.manifest.id,
                  "left " + std::to_string(leftover.second) + " byte(s) allocated through detach");
        allocator->deallocate(leftover.first, leftover.second);
    }
}

void ModuleHost::runFrame(float delta_seconds)
{
    resonate_scheduler_run_frame(impl_->scheduler, delta_seconds);
}

const std::vector<ModuleRecord*>& ModuleHost::order() const noexcept
{
    return impl_->graph.order();
}

ResonateCapabilityRegistry* ModuleHost::capabilities() const noexcept
{
    return impl_->registry;
}

ResonateScheduler* ModuleHost::scheduler() const noexcept
{
    return impl_->scheduler;
}

void ModuleHost::setLogSink(void* user_data, ResonateLogFn sink)
{
    impl_->log_user_data = user_data;
    impl_->log_sink = sink;
}

} // namespace resonate
