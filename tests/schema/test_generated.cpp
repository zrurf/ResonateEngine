#include <catch2/catch_all.hpp>

#include <cstddef>
#include <cstdint>
#include <string>

#include <resonate/core/allocator.h>
#include <resonate/ecs/query.h>
#include <resonate/ecs/world.h>

#include <yyjson.h>

#include "paths.h"
#include "schema/cli.h"

/* The generated types, as the build's schema rule left them: including this
   header is the first half of the acceptance — its static asserts only compile
   when this compiler lays the declarations out the way the schema froze them. */
#include "resonate.gameplay/components.gen.h"

namespace
{

using resonate::Span;
using resonate::ecs::ChunkView;
using resonate::ecs::ComponentIndex;
using resonate::ecs::ComponentTraits;
using resonate::ecs::Entity;
using resonate::ecs::Query;
using resonate::ecs::QueryDesc;
using resonate::ecs::World;

/* A dense scan over the generated component, through the typed column the ECS
   hands out: what a System-type aspect does in the swarm lane. */
struct Counter
{
    std::uint64_t visited = 0;
    std::int64_t sum = 0;
    std::uint32_t columnCount = 0;

    static void run(void* context, ChunkView view)
    {
        auto* self = static_cast<Counter*>(context);
        const resonate::ecs::Column<resonate::gameplay::Health> health =
            resonate::ecs::column<resonate::gameplay::Health>(view);

        REQUIRE(health.data != nullptr);
        REQUIRE(health.ticks != nullptr);
        REQUIRE(health.count == view.count);

        self->columnCount = health.count;
        for (std::uint32_t row = 0; row < health.count; ++row)
        {
            self->sum += health.data[row].current;
            ++self->visited;
        }
    }
};

/* Reads a string member; a missing or mistyped one fails the case rather than
   handing a null pointer to std::string. */
std::string stringOf(yyjson_val* object, const char* key)
{
    yyjson_val* value = yyjson_obj_get(object, key);
    INFO(key);
    REQUIRE(value != nullptr);
    REQUIRE(yyjson_is_str(value));
    return std::string(yyjson_get_str(value));
}

std::int64_t integerOf(yyjson_val* object, const char* key)
{
    yyjson_val* value = yyjson_obj_get(object, key);
    INFO(key);
    REQUIRE(value != nullptr);
    REQUIRE(yyjson_is_int(value));
    return yyjson_get_sint(value);
}

yyjson_val* findType(yyjson_val* root, const char* name)
{
    yyjson_val* types = yyjson_obj_get(root, "types");
    if (types == nullptr)
    {
        return nullptr;
    }

    yyjson_arr_iter iterator = yyjson_arr_iter_with(types);
    yyjson_val* type = nullptr;
    while ((type = yyjson_arr_iter_next(&iterator)) != nullptr)
    {
        if (stringOf(type, "name") == name)
        {
            return type;
        }
    }
    return nullptr;
}

yyjson_val* findField(yyjson_val* type, const char* name)
{
    yyjson_val* fields = yyjson_obj_get(type, "fields");
    if (fields == nullptr)
    {
        return nullptr;
    }

    yyjson_val* field = nullptr;
    yyjson_arr_iter iterator = yyjson_arr_iter_with(fields);
    while ((field = yyjson_arr_iter_next(&iterator)) != nullptr)
    {
        if (stringOf(field, "name") == name)
        {
            return field;
        }
    }
    return nullptr;
}

/* Compares the metadata's field offsets with this compiler's, field by field:
   names come from the schema, numbers from offsetof. */
void checkFieldOffsets(yyjson_val* type, const char* const* names, const std::size_t* offsets,
                       std::size_t count)
{
    yyjson_val* fields = yyjson_obj_get(type, "fields");
    REQUIRE(fields != nullptr);
    REQUIRE(yyjson_arr_size(fields) == count);

    std::size_t seen = 0;
    yyjson_val* field = nullptr;
    yyjson_arr_iter iterator = yyjson_arr_iter_with(fields);
    while ((field = yyjson_arr_iter_next(&iterator)) != nullptr)
    {
        const std::string name = stringOf(field, "name");

        bool matched = false;
        for (std::size_t index = 0; index < count; ++index)
        {
            if (name == names[index])
            {
                INFO(name);
                REQUIRE(integerOf(field, "offset") == static_cast<std::int64_t>(offsets[index]));
                matched = true;
                break;
            }
        }
        REQUIRE(matched);
        ++seen;
    }
    REQUIRE(seen == count);
}

void checkLayout(yyjson_val* type, std::size_t size, std::size_t align)
{
    yyjson_val* layout = yyjson_obj_get(type, "layout");
    REQUIRE(layout != nullptr);
    REQUIRE(integerOf(layout, "size") == static_cast<std::int64_t>(size));
    REQUIRE(integerOf(layout, "align") == static_cast<std::int64_t>(align));
}

} // namespace

TEST_CASE("a schema component registers, stores and iterates in ECS memory", "[schema][ecs]")
{
    using resonate::gameplay::Health;
    using resonate::gameplay::LastDamage;
    using resonate::gameplay::Transform;

    World world(resonate::systemAllocator());
    const ComponentIndex health = world.registerComponent<Health>();
    const ComponentIndex transform = world.registerComponent<Transform>();
    const ComponentIndex lastDamage = world.registerComponent<LastDamage>();
    REQUIRE(health != resonate::ecs::kInvalidComponent);
    REQUIRE(transform != resonate::ecs::kInvalidComponent);
    REQUIRE(lastDamage != resonate::ecs::kInvalidComponent);

    /* What a component is addressed by outside C++ is the module-qualified name
       the schema gave it. */
    REQUIRE(std::string(ComponentTraits<Health>::name) == "resonate.gameplay.Health");
    REQUIRE(std::string(ComponentTraits<Transform>::name) == "resonate.gameplay.Transform");

    constexpr std::uint32_t kCount = 100;
    std::int64_t expected = 0;
    for (std::uint32_t index = 0; index < kCount; ++index)
    {
        const Entity entity = world.create();
        REQUIRE(world.alive(entity));

        Health value;
        value.current = static_cast<std::int32_t>(index);
        value.max = static_cast<std::int32_t>(kCount);
        value.regen = 0.25F;
        value.source = entity;
        REQUIRE(world.add(entity, value) != nullptr);

        Transform placement;
        placement.x = static_cast<float>(index);
        placement.scale = 2.0F;
        REQUIRE(world.add(entity, placement) != nullptr);

        expected += static_cast<std::int64_t>(index);
    }
    REQUIRE(world.entityCount() == kCount);

    const ComponentIndex all[] = {health, transform};
    QueryDesc desc;
    desc.all = Span<const ComponentIndex>(all, 2);
    Query query = world.createQuery(desc);
    REQUIRE(query.valid());
    REQUIRE(query.matchedChunkCount() > 0);

    Counter counter;
    query.forEachChunk(&Counter::run, &counter);
    REQUIRE(counter.visited == kCount);
    REQUIRE(counter.sum == expected);

    /* The values are the ones that were written, read back through the typed
       accessor the way a system would. */
    const Entity first = world.create();
    Health probe;
    probe.current = -1;
    probe.max = 7;
    probe.source = Entity{0, 0};
    REQUIRE(world.add(first, probe) != nullptr);

    Health* stored = world.get<Health>(first);
    REQUIRE(stored != nullptr);
    REQUIRE(stored->current == -1);
    REQUIRE(stored->max == 7);
    REQUIRE(stored->regen == 0.5F); /* the schema's default, from the initialiser */
    REQUIRE(stored->source == Entity{0, 0});
}

TEST_CASE("the generated metadata describes the compiled layout", "[schema]")
{
    const std::string directory = resonate::test::findBuildDirectory("gen");
    REQUIRE_FALSE(directory.empty());

    const std::string path = directory + "/resonate.gameplay.schema.json";
    const std::string text = resonate::test::readTextFile(path);
    INFO(path);
    REQUIRE_FALSE(text.empty());

    yyjson_doc* document = yyjson_read(text.data(), text.size(), 0);
    REQUIRE(document != nullptr);
    yyjson_val* root = yyjson_doc_get_root(document);

    REQUIRE(stringOf(root, "module") == "resonate.gameplay");
    REQUIRE(yyjson_arr_size(yyjson_obj_get(root, "types")) == 5);

    using resonate::gameplay::Damage;
    using resonate::gameplay::Health;
    using resonate::gameplay::HealthFlags;
    using resonate::gameplay::LastDamage;
    using resonate::gameplay::Transform;

    yyjson_val* health = findType(root, "Health");
    REQUIRE(health != nullptr);
    REQUIRE(stringOf(health, "kind") == "component");
    REQUIRE(stringOf(health, "qualifiedName") == "resonate.gameplay.Health");
    REQUIRE(integerOf(health, "version") ==
            static_cast<std::int64_t>(resonate::gameplay::kHealthVersion));
    checkLayout(health, sizeof(Health), alignof(Health));

    const char* const healthFields[] = {"current", "max", "flags", "regen", "source", "shield"};
    const std::size_t healthOffsets[] = {
        offsetof(Health, current), offsetof(Health, max),    offsetof(Health, flags),
        offsetof(Health, regen),   offsetof(Health, source), offsetof(Health, shield),
    };
    checkFieldOffsets(health, healthFields, healthOffsets, 6);

    /* Version 2 declares the path from version 1; the metadata carries the
       chain the future loader will drive. */
    yyjson_val* migrations = yyjson_obj_get(health, "migrations");
    REQUIRE(migrations != nullptr);
    REQUIRE(yyjson_arr_size(migrations) == 1);
    yyjson_val* migration = yyjson_arr_get(migrations, 0);
    REQUIRE(integerOf(migration, "from") == 1);
    REQUIRE(integerOf(migration, "to") == 2);

    yyjson_val* transform = findType(root, "Transform");
    REQUIRE(transform != nullptr);
    checkLayout(transform, sizeof(Transform), alignof(Transform));

    const char* const transformFields[] = {"x", "y", "z", "scale"};
    const std::size_t transformOffsets[] = {
        offsetof(Transform, x),
        offsetof(Transform, y),
        offsetof(Transform, z),
        offsetof(Transform, scale),
    };
    checkFieldOffsets(transform, transformFields, transformOffsets, 4);

    yyjson_val* damage = findType(root, "Damage");
    REQUIRE(damage != nullptr);
    REQUIRE(stringOf(damage, "kind") == "struct");
    checkLayout(damage, sizeof(Damage), alignof(Damage));

    yyjson_val* last = findType(root, "LastDamage");
    REQUIRE(last != nullptr);
    const char* const lastFields[] = {"hit", "attacker"};
    const std::size_t lastOffsets[] = {offsetof(LastDamage, hit), offsetof(LastDamage, attacker)};
    checkFieldOffsets(last, lastFields, lastOffsets, 2);

    yyjson_val* flags = findType(root, "HealthFlags");
    REQUIRE(flags != nullptr);
    REQUIRE(stringOf(flags, "width") == "u8");
    checkLayout(flags, sizeof(HealthFlags), alignof(HealthFlags));
    REQUIRE(yyjson_arr_size(yyjson_obj_get(flags, "values")) == 3);

    /* The declared constraints and defaults survive into the metadata verbatim,
       which is what the inspector and serialisation validation read. */
    yyjson_val* current = findField(health, "current");
    REQUIRE(current != nullptr);
    REQUIRE(stringOf(current, "min") == "0");
    REQUIRE(stringOf(current, "default") == "100");

    yyjson_val* stagger = findField(damage, "stagger");
    REQUIRE(stagger != nullptr);
    REQUIRE(stringOf(stagger, "min") == "0");
    REQUIRE(stringOf(stagger, "max") == "1");

    yyjson_doc_free(document);
}
