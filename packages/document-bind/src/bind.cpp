#include <resonate/document_bind/bind.h>

#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <resonate/core/math.h>

namespace resonate::dom_bind
{
namespace
{

std::vector<std::string_view> tokens(std::string_view text)
{
    std::vector<std::string_view> result;
    std::size_t index = 0;
    while (index < text.size())
    {
        while (index < text.size() && std::isspace(static_cast<unsigned char>(text[index])) != 0)
        {
            ++index;
        }
        const std::size_t begin = index;
        while (index < text.size() && std::isspace(static_cast<unsigned char>(text[index])) == 0)
        {
            ++index;
        }
        if (index > begin)
        {
            result.push_back(text.substr(begin, index - begin));
        }
    }
    return result;
}

bool parseFloat(std::string_view text, float& out)
{
    const std::string copy(text);
    char* end = nullptr;
    errno = 0;
    const float value = std::strtof(copy.c_str(), &end);
    if (end == copy.c_str() || *end != '\0' || errno == ERANGE || !std::isfinite(value))
    {
        return false;
    }
    out = value;
    return true;
}

bool parseI32(std::string_view text, std::int32_t& out)
{
    const std::string copy(text);
    char* end = nullptr;
    errno = 0;
    const long value = std::strtol(copy.c_str(), &end, 10);
    if (end == copy.c_str() || *end != '\0' || errno == ERANGE || value < INT32_MIN ||
        value > INT32_MAX)
    {
        return false;
    }
    out = static_cast<std::int32_t>(value);
    return true;
}

bool parseU32(std::string_view text, std::uint32_t& out)
{
    /* strtoul accepts a sign and skips leading space; a field value is exactly
       the digits, or it is not a number. */
    if (text.empty())
    {
        return false;
    }
    for (const char character : text)
    {
        if (character < '0' || character > '9')
        {
            return false;
        }
    }
    const std::string copy(text);
    char* end = nullptr;
    errno = 0;
    const unsigned long value = std::strtoul(copy.c_str(), &end, 10);
    if (end == copy.c_str() || *end != '\0' || errno == ERANGE || value > UINT32_MAX)
    {
        return false;
    }
    out = static_cast<std::uint32_t>(value);
    return true;
}

bool parseI64(std::string_view text, std::int64_t& out)
{
    if (text.empty())
    {
        return false;
    }
    const std::string copy(text);
    char* end = nullptr;
    errno = 0;
    const long long value = std::strtoll(copy.c_str(), &end, 10);
    if (end == copy.c_str() || *end != '\0' || errno == ERANGE)
    {
        return false;
    }
    out = static_cast<std::int64_t>(value);
    return true;
}

bool parseU64(std::string_view text, std::uint64_t& out)
{
    if (text.empty())
    {
        return false;
    }
    for (const char character : text)
    {
        if (character < '0' || character > '9')
        {
            return false;
        }
    }
    const std::string copy(text);
    char* end = nullptr;
    errno = 0;
    const unsigned long long value = std::strtoull(copy.c_str(), &end, 10);
    if (end == copy.c_str() || *end != '\0' || errno == ERANGE)
    {
        return false;
    }
    out = static_cast<std::uint64_t>(value);
    return true;
}

bool parseFloats(std::string_view text, float* out, std::size_t count, std::string& error)
{
    const auto parts = tokens(text);
    if (parts.size() != count)
    {
        error = "expected " + std::to_string(count) + " numbers";
        return false;
    }
    for (std::size_t index = 0; index < count; ++index)
    {
        if (!parseFloat(parts[index], out[index]))
        {
            error = "a value is not a number";
            return false;
        }
    }
    return true;
}

} // namespace

bool parseField(const dom::ComponentFieldDesc& field, std::string_view text, void* data,
                std::string& error)
{
    const std::size_t offset = field.offset;
    const std::string_view type = field.type != nullptr ? field.type : "";
    /* A named enum accepts one of its value names; the ordinal is what the
       component holds. */
    if (field.enum_values != nullptr && field.enum_value_count != 0U)
    {
        for (std::uint32_t index = 0; index < field.enum_value_count; ++index)
        {
            if (text != field.enum_values[index])
            {
                continue;
            }
            switch (field.size)
            {
            case 1U:
            {
                const auto value = static_cast<std::uint8_t>(index);
                std::memcpy(static_cast<std::byte*>(data) + offset, &value, sizeof(value));
                return true;
            }
            case 2U:
            {
                const auto value = static_cast<std::uint16_t>(index);
                std::memcpy(static_cast<std::byte*>(data) + offset, &value, sizeof(value));
                return true;
            }
            case 4U:
            {
                const auto value = static_cast<std::uint32_t>(index);
                std::memcpy(static_cast<std::byte*>(data) + offset, &value, sizeof(value));
                return true;
            }
            case 8U:
            {
                const auto value = static_cast<std::uint64_t>(index);
                std::memcpy(static_cast<std::byte*>(data) + offset, &value, sizeof(value));
                return true;
            }
            default:
                break;
            }
            error = "an enum field has an unsupported width";
            return false;
        }
        error = "not one of the enum's values";
        return false;
    }
    if (type == "f32" && field.size == sizeof(float))
    {
        float value = 0.0F;
        if (!parseFloat(text, value))
        {
            error = "not an f32";
            return false;
        }
        std::memcpy(static_cast<std::byte*>(data) + offset, &value, sizeof(value));
        return true;
    }
    if (type == "i32" && field.size == sizeof(std::int32_t))
    {
        std::int32_t value = 0;
        if (!parseI32(text, value))
        {
            error = "not an i32";
            return false;
        }
        std::memcpy(static_cast<std::byte*>(data) + offset, &value, sizeof(value));
        return true;
    }
    if (type == "u32" && field.size == sizeof(std::uint32_t))
    {
        std::uint32_t value = 0;
        if (!parseU32(text, value))
        {
            error = "not a u32";
            return false;
        }
        std::memcpy(static_cast<std::byte*>(data) + offset, &value, sizeof(value));
        return true;
    }
    if (type == "i8" || type == "i16" || type == "i64")
    {
        std::int64_t value = 0;
        if (!parseI64(text, value) ||
            (type == "i8" && (value < INT8_MIN || value > INT8_MAX)) ||
            (type == "i16" && (value < INT16_MIN || value > INT16_MAX)))
        {
            error = "not an " + std::string(type);
            return false;
        }
        if (type == "i8" && field.size == 1U)
        {
            const auto narrowed = static_cast<std::int8_t>(value);
            std::memcpy(static_cast<std::byte*>(data) + offset, &narrowed, sizeof(narrowed));
            return true;
        }
        if (type == "i16" && field.size == 2U)
        {
            const auto narrowed = static_cast<std::int16_t>(value);
            std::memcpy(static_cast<std::byte*>(data) + offset, &narrowed, sizeof(narrowed));
            return true;
        }
        if (type == "i64" && field.size == 8U)
        {
            std::memcpy(static_cast<std::byte*>(data) + offset, &value, sizeof(value));
            return true;
        }
        error = "the field's width does not match its type";
        return false;
    }
    if (type == "u8" || type == "u16" || type == "u64")
    {
        std::uint64_t value = 0;
        if (!parseU64(text, value) || (type == "u8" && value > UINT8_MAX) ||
            (type == "u16" && value > UINT16_MAX))
        {
            error = "not a " + std::string(type);
            return false;
        }
        if (type == "u8" && field.size == 1U)
        {
            const auto narrowed = static_cast<std::uint8_t>(value);
            std::memcpy(static_cast<std::byte*>(data) + offset, &narrowed, sizeof(narrowed));
            return true;
        }
        if (type == "u16" && field.size == 2U)
        {
            const auto narrowed = static_cast<std::uint16_t>(value);
            std::memcpy(static_cast<std::byte*>(data) + offset, &narrowed, sizeof(narrowed));
            return true;
        }
        if (type == "u64" && field.size == 8U)
        {
            std::memcpy(static_cast<std::byte*>(data) + offset, &value, sizeof(value));
            return true;
        }
        error = "the field's width does not match its type";
        return false;
    }
    if (type == "bool" && field.size == 1U)
    {
        if (text != "true" && text != "false")
        {
            error = "not a bool";
            return false;
        }
        static_cast<std::byte*>(data)[offset] = text == "true" ? std::byte{1} : std::byte{0};
        return true;
    }
    if (type == "vec2" && field.size == sizeof(Vec2))
    {
        const auto parts = tokens(text);
        Vec2 value;
        if (parts.size() != 2U || !parseFloat(parts[0], value.x) ||
            !parseFloat(parts[1], value.y))
        {
            error = "not a vec2";
            return false;
        }
        std::memcpy(static_cast<std::byte*>(data) + offset, &value, sizeof(value));
        return true;
    }
    if (type == "vec3" && field.size == sizeof(Vec3))
    {
        const auto parts = tokens(text);
        Vec3 value;
        if (parts.size() != 3U || !parseFloat(parts[0], value.x) ||
            !parseFloat(parts[1], value.y) || !parseFloat(parts[2], value.z))
        {
            error = "not a vec3";
            return false;
        }
        std::memcpy(static_cast<std::byte*>(data) + offset, &value, sizeof(value));
        return true;
    }
    if (type == "vec4" && field.size == sizeof(Vec4))
    {
        const auto parts = tokens(text);
        Vec4 value;
        if (parts.size() != 4U || !parseFloat(parts[0], value.x) ||
            !parseFloat(parts[1], value.y) || !parseFloat(parts[2], value.z) ||
            !parseFloat(parts[3], value.w))
        {
            error = "not a vec4";
            return false;
        }
        std::memcpy(static_cast<std::byte*>(data) + offset, &value, sizeof(value));
        return true;
    }
    if (type == "quat" && field.size == sizeof(Quat))
    {
        const auto parts = tokens(text);
        Quat value;
        if (parts.size() != 4U || !parseFloat(parts[0], value.x) ||
            !parseFloat(parts[1], value.y) || !parseFloat(parts[2], value.z) ||
            !parseFloat(parts[3], value.w))
        {
            error = "not a quat";
            return false;
        }
        std::memcpy(static_cast<std::byte*>(data) + offset, &value, sizeof(value));
        return true;
    }
    if (type == "mat4" && field.size == sizeof(Mat4))
    {
        float values[16] = {};
        if (!parseFloats(text, values, 16U, error))
        {
            return false;
        }
        std::memcpy(static_cast<std::byte*>(data) + offset, values, sizeof(values));
        return true;
    }
    error = "the field type '" + std::string(type) + "' is not bindable here";
    return false;
}

bool attachComponent(ecs::World& world, ecs::Entity entity, const dom::Document& document,
                     const dom::Component& component, const dom::ComponentDesc& descriptor,
                     std::string& error)
{
    if (descriptor.derived != 0U)
    {
        error = "component '" + std::string(descriptor.name) + "' is derived";
        return false;
    }
    const ecs::ComponentIndex index = world.findComponent(descriptor.qualified_name);
    if (index == ecs::kInvalidComponent)
    {
        error = "component '" + std::string(descriptor.qualified_name) + "' is not registered";
        return false;
    }

    /* The schema's defaults are the starting point; authored fields overwrite
       them. Without this a component lands zeroed (a colour's alpha included)
       whenever the document names only some of its fields. */
    std::vector<std::byte> data(descriptor.size, std::byte{0});
    if (descriptor.defaults != nullptr)
    {
        std::memcpy(data.data(), descriptor.defaults, descriptor.size);
    }
    for (const dom::Field& field : component.fields)
    {
        const std::string_view name = document.text(field.name);
        const dom::ComponentFieldDesc* layout = nullptr;
        for (const dom::ComponentFieldDesc& candidate : descriptor.fields)
        {
            if (name == candidate.name)
            {
                layout = &candidate;
                break;
            }
        }
        if (layout == nullptr)
        {
            error = "unknown field '" + std::string(name) + "' on " +
                    std::string(descriptor.name);
            return false;
        }
        if (!parseField(*layout, field.value, data.data(), error))
        {
            error = std::string(descriptor.name) + "." + std::string(name) + ": " + error;
            return false;
        }
    }
    if (world.add(entity, index, data.data()) == nullptr)
    {
        error = "component '" + std::string(descriptor.name) + "' could not be attached";
        return false;
    }
    return true;
}

} // namespace resonate::dom_bind
