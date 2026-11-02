/* The cooked clip codec: keyframe tracks as a packed table instead of XML.
 *
 * Layout (little-endian):
 *
 *   "RCLP" | u32 codec | f32 duration | u32 flags (bit 0: position root motion)
 *   | u32 position key count, each: f32 time, three f32
 *   | u32 rotation key count, each: f32 time, four f32
 *   | u32 scale key count, each: f32 time, three f32
 *
 * The decoder applies the XML parser's rules rather than trusting the writer:
 * positive duration, every key inside [0, duration], keys strictly ascending.
 */

#include <resonate/animation/clip.h>

#include <cstring>
#include <string>
#include <vector>

namespace resonate::animation
{
namespace
{

constexpr char kMagic[4] = {'R', 'C', 'L', 'P'};
constexpr std::uint32_t kCodecVersion = 1U;
constexpr std::uint32_t kRootMotion = 1U;

struct Writer
{
    std::vector<std::byte> bytes;

    void u32(std::uint32_t value)
    {
        for (int byte = 0; byte < 4; ++byte)
        {
            bytes.push_back(std::byte{static_cast<std::uint8_t>((value >> (8 * byte)) & 0xFFU)});
        }
    }

    void f32(float value)
    {
        std::uint32_t bits = 0U;
        std::memcpy(&bits, &value, sizeof(bits));
        u32(bits);
    }
};

struct Reader
{
    Span<const std::byte> data;
    std::size_t at = 0;

    [[nodiscard]] bool u32(std::uint32_t& out)
    {
        if (4U > data.size() - at)
        {
            return false;
        }
        out = 0U;
        for (std::size_t byte = 0U; byte < 4U; ++byte)
        {
            out |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(data[at + byte]))
                   << (8U * static_cast<unsigned>(byte));
        }
        at += 4U;
        return true;
    }

    [[nodiscard]] bool f32(float& out)
    {
        std::uint32_t bits = 0U;
        if (!u32(bits))
        {
            return false;
        }
        std::memcpy(&out, &bits, sizeof(out));
        return true;
    }
};

template <typename Key> bool validTrack(const std::vector<Key>& keys, float duration,
                                       const char* name, std::string& error)
{
    float last = -1.0F;
    for (const Key& key : keys)
    {
        if (!(key.time >= 0.0F) || key.time > duration)
        {
            error = std::string(name) + " has a key outside the clip";
            return false;
        }
        if (key.time <= last)
        {
            error = std::string(name) + " keys do not ascend";
            return false;
        }
        last = key.time;
    }
    return true;
}

template <typename Key, typename Value, std::size_t N>
void writeTrack(Writer& writer, const std::vector<Key>& keys)
{
    static_assert(sizeof(Value) == N * sizeof(float), "a track value is N floats");
    writer.u32(static_cast<std::uint32_t>(keys.size()));
    for (const Key& key : keys)
    {
        writer.f32(key.time);
        float values[N] = {};
        std::memcpy(values, &key.value, sizeof(values));
        for (const float value : values)
        {
            writer.f32(value);
        }
    }
}

template <typename Key, typename Value, std::size_t N>
bool readTrack(Reader& reader, std::vector<Key>& keys)
{
    static_assert(sizeof(Value) == N * sizeof(float), "a track value is N floats");
    std::uint32_t count = 0U;
    if (!reader.u32(count) || count > (reader.data.size() - reader.at) / (4U * (N + 1U)))
    {
        return false;
    }
    keys.resize(count);
    for (Key& key : keys)
    {
        float values[N] = {};
        if (!reader.f32(key.time))
        {
            return false;
        }
        for (float& value : values)
        {
            if (!reader.f32(value))
            {
                return false;
            }
        }
        std::memcpy(&key.value, values, sizeof(values));
    }
    return true;
}

} // namespace

bool Clip::isBinary(Span<const std::byte> bytes) noexcept
{
    if (bytes.size() < sizeof(kMagic))
    {
        return false;
    }
    return std::memcmp(bytes.data(), kMagic, sizeof(kMagic)) == 0;
}

std::vector<std::byte> Clip::toBinary() const
{
    Writer writer;
    writer.bytes.insert(writer.bytes.end(),
                        reinterpret_cast<const std::byte*>(kMagic),
                        reinterpret_cast<const std::byte*>(kMagic) + sizeof(kMagic));
    writer.u32(kCodecVersion);
    writer.f32(duration);
    writer.u32(position_root_motion ? kRootMotion : 0U);

    writeTrack<Vec3Track::Key, Vec3, 3>(writer, position.keys);
    writeTrack<QuatTrack::Key, Quat, 4>(writer, rotation.keys);
    writeTrack<Vec3Track::Key, Vec3, 3>(writer, scale.keys);
    return std::move(writer.bytes);
}

bool Clip::parseBinary(Span<const std::byte> bytes, Clip& out, std::string& error)
{
    if (bytes.size() < sizeof(kMagic) + 12U || !isBinary(bytes))
    {
        error = "not a cooked clip";
        return false;
    }
    Reader reader{bytes, sizeof(kMagic)};
    std::uint32_t codec = 0U;
    std::uint32_t flags = 0U;
    float duration = 0.0F;
    if (!reader.u32(codec) || !reader.f32(duration) || !reader.u32(flags))
    {
        error = "the cooked clip header is truncated";
        return false;
    }
    if (codec != kCodecVersion)
    {
        error = "the cooked clip codec " + std::to_string(codec) +
                " is not the one this build reads";
        return false;
    }
    if ((flags & ~kRootMotion) != 0U)
    {
        error = "the cooked clip sets unknown flags";
        return false;
    }
    if (!(duration > 0.0F))
    {
        error = "the cooked clip's duration is not positive";
        return false;
    }

    Clip clip;
    clip.duration = duration;
    clip.position_root_motion = (flags & kRootMotion) != 0U;
    if (!readTrack<Vec3Track::Key, Vec3, 3>(reader, clip.position.keys) ||
        !readTrack<QuatTrack::Key, Quat, 4>(reader, clip.rotation.keys) ||
        !readTrack<Vec3Track::Key, Vec3, 3>(reader, clip.scale.keys))
    {
        error = "the cooked clip's tracks are truncated";
        return false;
    }
    if (reader.at != bytes.size())
    {
        error = "the cooked clip has trailing bytes";
        return false;
    }
    if (!validTrack<Vec3Track::Key>(clip.position.keys, duration, "position", error) ||
        !validTrack<QuatTrack::Key>(clip.rotation.keys, duration, "rotation", error) ||
        !validTrack<Vec3Track::Key>(clip.scale.keys, duration, "scale", error))
    {
        return false;
    }
    out = std::move(clip);
    return true;
}

bool Clip::parseBytes(Span<const std::byte> bytes, Clip& out, std::string& error)
{
    if (isBinary(bytes))
    {
        return parseBinary(bytes, out, error);
    }
    return parse(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()),
                 out, error);
}

} // namespace resonate::animation
