#include "component_bind.h"

#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <vector>

#include <resonate/core/math.h>

namespace resonate::ui::detail
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

bool parseField(const dom::ComponentFieldDesc& field, const std::string& text, std::byte* data,
                std::string& error)
{
    const std::size_t offset = field.offset;
    const std::string_view type = field.type != nullptr ? field.type : "";
    if (type == "f32" && field.size == sizeof(float))
    {
        float value = 0.0F;
        if (!parseFloat(text, value))
        {
            error = "not an f32";
            return false;
        }
        std::memcpy(data + offset, &value, sizeof(value));
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
        std::memcpy(data + offset, &value, sizeof(value));
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
        std::memcpy(data + offset, &value, sizeof(value));
        return true;
    }
    if (type == "bool" && field.size == 1U)
    {
        if (text != "true" && text != "false")
        {
            error = "not a bool";
            return false;
        }
        data[offset] = text == "true" ? std::byte{1} : std::byte{0};
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
        std::memcpy(data + offset, &value, sizeof(value));
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
        std::memcpy(data + offset, &value, sizeof(value));
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
        std::memcpy(data + offset, &value, sizeof(value));
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
        std::memcpy(data + offset, &value, sizeof(value));
        return true;
    }
    if (type == "mat4" && field.size == sizeof(Mat4))
    {
        float values[16] = {};
        if (!parseFloats(text, values, 16U, error))
        {
            return false;
        }
        std::memcpy(data + offset, values, sizeof(values));
        return true;
    }
    error = "the field type '" + std::string(type) + "' is not bindable here";
    return false;
}

} // namespace

bool attachGeneric(ecs::World& world, const dom::Document& document, dom::NodeHandle node,
                   ecs::Entity entity, const dom::Component& component,
                   const dom::ComponentDesc& descriptor, std::string& error)
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

    std::vector<std::byte> data(descriptor.size, std::byte{0});
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
    (void)node;
    return true;
}

} // namespace resonate::ui::detail
