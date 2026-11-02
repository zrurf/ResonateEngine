#include <resonate/animation/clip.h>

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>

namespace resonate::animation
{
namespace
{

/* The segment [begin, end) that brackets `time`: the keys are sorted, so a
   linear walk is fine for the key counts a clip carries. Answers the pair to
   interpolate and the 0..1 weight inside it; outside the keys the weight
   pins to the nearest end. */
template <typename Track>
bool bracket(const Track& track, float time, std::size_t& index, float& alpha)
{
    if (track.keys.size() < 2U || time <= track.keys.front().time)
    {
        return false;
    }
    if (time >= track.keys.back().time)
    {
        index = track.keys.size() - 2U;
        alpha = 1.0F;
        return true;
    }
    for (std::size_t i = 0U; i + 1U < track.keys.size(); ++i)
    {
        if (time < track.keys[i + 1U].time)
        {
            index = i;
            const float span = track.keys[i + 1U].time - track.keys[i].time;
            alpha = span > 0.0F ? (time - track.keys[i].time) / span : 0.0F;
            return true;
        }
    }
    return false;
}

Vec3 sampleVec3(const Vec3Track& track, float time)
{
    std::size_t index = 0U;
    float alpha = 0.0F;
    if (!bracket(track, time, index, alpha))
    {
        return track.keys.empty() ? Vec3{} : track.keys.front().value;
    }
    const Vec3Track::Key& from = track.keys[index];
    const Vec3Track::Key& to = track.keys[index + 1U];
    return from.value + (to.value - from.value) * alpha;
}

Quat sampleQuat(const QuatTrack& track, float time)
{
    std::size_t index = 0U;
    float alpha = 0.0F;
    if (!bracket(track, time, index, alpha))
    {
        return track.keys.empty() ? Quat{} : track.keys.front().value;
    }
    return slerp(track.keys[index].value, track.keys[index + 1U].value, alpha);
}

} // namespace

void Clip::sample(float time, bool looping, hierarchy::LocalTransform& transform) const
{
    float within = time;
    if (looping && duration > 0.0F)
    {
        within = std::fmod(time, duration);
        if (within < 0.0F)
        {
            within += duration;
        }
    }
    else if (time < 0.0F)
    {
        within = 0.0F;
    }
    else if (duration > 0.0F && time > duration)
    {
        within = duration;
    }

    if (!position.keys.empty() && !position_root_motion)
    {
        transform.position = sampleVec3(position, within);
    }
    if (!rotation.keys.empty())
    {
        transform.rotation = sampleQuat(rotation, within);
    }
    if (!scale.keys.empty())
    {
        transform.scale = sampleVec3(scale, within);
    }
}

Vec3 Clip::rootAt(float time) const
{
    if (!position_root_motion || position.keys.empty())
    {
        return Vec3{};
    }
    const float at =
        time < 0.0F ? 0.0F : (duration > 0.0F && time > duration ? duration : time);
    return sampleVec3(position, at);
}

void Clip::blend(const Clip& from, const Clip& to, float weight, float time, bool looping,
                 hierarchy::LocalTransform& transform)
{
    blend(from, to, weight, time, time, looping, looping, transform);
}

void Clip::blend(const Clip& from, const Clip& to, float weight, float from_time, float to_time,
                 bool from_looping, bool to_looping, hierarchy::LocalTransform& transform)
{
    hierarchy::LocalTransform from_sample = transform;
    hierarchy::LocalTransform to_sample = transform;
    from.sample(from_time, from_looping, from_sample);
    to.sample(to_time, to_looping, to_sample);

    const float mixed = weight < 0.0F ? 0.0F : (weight > 1.0F ? 1.0F : weight);
    /* A field both clips drive blends; a field only one drives comes from
       that clip whole. A root-motion position track drives no pose. */
    const bool from_pose = !from.position.keys.empty() && !from.position_root_motion;
    const bool to_pose = !to.position.keys.empty() && !to.position_root_motion;
    if (from_pose && to_pose)
    {
        transform.position =
            from_sample.position + (to_sample.position - from_sample.position) * mixed;
    }
    else if (from_pose)
    {
        transform.position = from_sample.position;
    }
    else if (to_pose)
    {
        transform.position = to_sample.position;
    }

    if (!from.rotation.keys.empty() && !to.rotation.keys.empty())
    {
        transform.rotation = slerp(from_sample.rotation, to_sample.rotation, mixed);
    }
    else if (!from.rotation.keys.empty())
    {
        transform.rotation = from_sample.rotation;
    }
    else if (!to.rotation.keys.empty())
    {
        transform.rotation = to_sample.rotation;
    }

    if (!from.scale.keys.empty() && !to.scale.keys.empty())
    {
        transform.scale =
            from_sample.scale + (to_sample.scale - from_sample.scale) * mixed;
    }
    else if (!from.scale.keys.empty())
    {
        transform.scale = from_sample.scale;
    }
    else if (!to.scale.keys.empty())
    {
        transform.scale = to_sample.scale;
    }
}

namespace
{

/* The .rclip reader: the same restricted XML family the string table uses --
   elements with attributes, comments and processing instructions skipped,
   everything else an error with a line number. Values are numeric text, so
   entities are not decoded. */
struct ClipAttribute
{
    std::string name;
    std::string value;
};

struct ClipTag
{
    std::string name;
    std::vector<ClipAttribute> attributes;
    bool closing = false;
    bool self_closing = false;
};

class ClipReader
{
  public:
    explicit ClipReader(std::string_view text) : text_(text)
    {
        if (text_.substr(0U, 3U) == "\xEF\xBB\xBF")
        {
            pos_ = 3U;
        }
    }

    [[nodiscard]] bool atEnd() const { return pos_ >= text_.size(); }
    [[nodiscard]] int line() const { return line_; }

    bool skipMisc(std::string& error)
    {
        for (;;)
        {
            while (!atEnd() && isSpace(text_[pos_]))
            {
                advance();
            }
            if (startsWith("<!--"))
            {
                advance(4U);
                while (!startsWith("-->"))
                {
                    if (atEnd())
                    {
                        return fail("an unterminated comment", error);
                    }
                    advance();
                }
                advance(3U);
                continue;
            }
            if (startsWith("<?"))
            {
                advance(2U);
                while (!startsWith("?>"))
                {
                    if (atEnd())
                    {
                        return fail("an unterminated processing instruction", error);
                    }
                    advance();
                }
                advance(2U);
                continue;
            }
            return true;
        }
    }

    bool readTag(ClipTag& out, std::string& error)
    {
        if (!startsWith("<"))
        {
            return fail("expected '<'", error);
        }
        advance();
        if (startsWith("/"))
        {
            out.closing = true;
            advance();
        }
        if (!readName(out.name, error))
        {
            return false;
        }
        if (out.closing)
        {
            while (!atEnd() && isSpace(peek()))
            {
                advance();
            }
            if (!startsWith(">"))
            {
                return fail("a closing tag takes no attributes", error);
            }
            advance();
            return true;
        }
        for (;;)
        {
            while (!atEnd() && isSpace(peek()))
            {
                advance();
            }
            if (startsWith("/>"))
            {
                out.self_closing = true;
                advance(2U);
                return true;
            }
            if (startsWith(">"))
            {
                advance();
                return true;
            }
            if (atEnd() || !isNameChar(peek()))
            {
                return fail("expected an attribute", error);
            }
            ClipAttribute attribute;
            if (!readName(attribute.name, error))
            {
                return false;
            }
            while (!atEnd() && isSpace(peek()))
            {
                advance();
            }
            if (!startsWith("="))
            {
                return fail("expected '=' after '" + attribute.name + "'", error);
            }
            advance();
            while (!atEnd() && isSpace(peek()))
            {
                advance();
            }
            if (atEnd() || (peek() != '"' && peek() != '\''))
            {
                return fail("the value of '" + attribute.name + "' needs quotes", error);
            }
            const char quote = peek();
            advance();
            const std::size_t begin = pos_;
            while (!atEnd() && peek() != quote)
            {
                advance();
            }
            if (atEnd())
            {
                return fail("an unterminated value", error);
            }
            attribute.value.assign(text_.substr(begin, pos_ - begin));
            advance();
            for (const ClipAttribute& existing : out.attributes)
            {
                if (existing.name == attribute.name)
                {
                    return fail("duplicate attribute '" + attribute.name + "'", error);
                }
            }
            out.attributes.push_back(std::move(attribute));
        }
    }

    [[nodiscard]] const ClipAttribute* find(const ClipTag& tag, const char* name) const
    {
        for (const ClipAttribute& attribute : tag.attributes)
        {
            if (attribute.name == name)
            {
                return &attribute;
            }
        }
        return nullptr;
    }

  private:
    [[nodiscard]] bool startsWith(std::string_view prefix) const
    {
        return text_.substr(pos_, prefix.size()) == prefix;
    }

    [[nodiscard]] char peek() const { return text_[pos_]; }

    static bool isSpace(char character)
    {
        return character == ' ' || character == '\t' || character == '\r' ||
               character == '\n';
    }

    static bool isNameStart(char character)
    {
        const auto value = static_cast<unsigned char>(character);
        return character == '_' || (value >= 'a' && value <= 'z') ||
               (value >= 'A' && value <= 'Z');
    }

    static bool isNameChar(char character)
    {
        return isNameStart(character) || character == '-' ||
               (character >= '0' && character <= '9');
    }

    void advance(std::size_t count = 1U)
    {
        for (std::size_t index = 0U; index < count && pos_ < text_.size(); ++index)
        {
            if (text_[pos_] == '\n')
            {
                ++line_;
            }
            ++pos_;
        }
    }

    bool readName(std::string& out, std::string& error)
    {
        if (atEnd() || !isNameStart(peek()))
        {
            return fail("expected a name", error);
        }
        const std::size_t begin = pos_;
        while (!atEnd() && isNameChar(peek()))
        {
            advance();
        }
        out.assign(text_.substr(begin, pos_ - begin));
        return true;
    }

    bool fail(const std::string& message, std::string& error)
    {
        error = "clip:" + std::to_string(line_) + ": " + message;
        return false;
    }

    std::string_view text_;
    std::size_t pos_ = 0U;
    int line_ = 1;
};

/* One number out of whitespace-separated text. strtod would read past a
   string_view's end, so the token lands in a bounded buffer first. */
bool parseNumber(std::string_view text, std::size_t& at, float& out)
{
    while (at < text.size() && (text[at] == ' ' || text[at] == '\t'))
    {
        ++at;
    }
    const std::size_t begin = at;
    while (at < text.size())
    {
        const char character = text[at];
        const bool numeric = (character >= '0' && character <= '9') || character == '+' ||
                             character == '-' || character == '.' || character == 'e' ||
                             character == 'E';
        if (!numeric)
        {
            break;
        }
        ++at;
    }
    if (at == begin || at - begin >= 64U)
    {
        return false;
    }
    char buffer[64] = {};
    std::memcpy(buffer, text.data() + begin, at - begin);
    char* stop = nullptr;
    const float value = std::strtof(buffer, &stop);
    if (stop != buffer + (at - begin))
    {
        return false;
    }
    out = value;
    return true;
}

bool parseNumbers(std::string_view text, float* out, std::size_t count, std::string& error)
{
    std::size_t at = 0U;
    for (std::size_t index = 0U; index < count; ++index)
    {
        if (!parseNumber(text, at, out[index]))
        {
            error = "expected " + std::to_string(count) + " numbers";
            return false;
        }
    }
    while (at < text.size())
    {
        if (text[at] != ' ' && text[at] != '\t')
        {
            error = "a value carries text past its numbers";
            return false;
        }
        ++at;
    }
    return true;
}

} // namespace

bool Clip::parse(std::string_view xml, Clip& out, std::string& error)
{
    out = Clip{};
    ClipReader reader(xml);
    if (!reader.skipMisc(error))
    {
        return false;
    }
    ClipTag root;
    if (!reader.readTag(root, error))
    {
        return false;
    }
    if (root.closing || root.name != "clip")
    {
        error = "clip:" + std::to_string(reader.line()) + ": expected <clip>";
        return false;
    }
    for (const ClipAttribute& attribute : root.attributes)
    {
        if (attribute.name != "duration")
        {
            error = "clip:" + std::to_string(reader.line()) + ": unknown attribute '" +
                    attribute.name + "' (playback flags belong to the Animation component)";
            return false;
        }
    }
    const ClipAttribute* duration = reader.find(root, "duration");
    float parsed_duration = 0.0F;
    std::size_t at = 0U;
    if (duration == nullptr || !parseNumber(duration->value, at, parsed_duration) ||
        at != duration->value.size())
    {
        error = "clip:" + std::to_string(reader.line()) + ": duration must be one number";
        return false;
    }
    if (!(parsed_duration > 0.0F))
    {
        error = "clip:" + std::to_string(reader.line()) + ": duration must be positive";
        return false;
    }
    out.duration = parsed_duration;

    if (root.self_closing)
    {
        return true;
    }
    for (;;)
    {
        if (!reader.skipMisc(error))
        {
            return false;
        }
        if (reader.atEnd())
        {
            error = "clip:" + std::to_string(reader.line()) + ": missing </clip>";
            return false;
        }
        ClipTag tag;
        if (!reader.readTag(tag, error))
        {
            return false;
        }
        if (tag.closing)
        {
            if (tag.name != "clip")
            {
                error = "clip:" + std::to_string(reader.line()) + ": expected </clip>";
                return false;
            }
            break;
        }
        if (tag.name != "track")
        {
            error = "clip:" + std::to_string(reader.line()) + ": unknown element '<" +
                    tag.name + ">'";
            return false;
        }
        const ClipAttribute* field = reader.find(tag, "field");
        if (field == nullptr)
        {
            error = "clip:" + std::to_string(reader.line()) + ": a track needs a field";
            return false;
        }
        for (const ClipAttribute& attribute : tag.attributes)
        {
            if (attribute.name != "field" && attribute.name != "root_motion")
            {
                error = "clip:" + std::to_string(reader.line()) + ": unknown attribute '" +
                        attribute.name + "' on track";
                return false;
            }
        }
        Vec3Track* vec_track = nullptr;
        QuatTrack* quat_track = nullptr;
        if (field->value == "position")
        {
            vec_track = &out.position;
        }
        else if (field->value == "scale")
        {
            vec_track = &out.scale;
        }
        else if (field->value == "rotation")
        {
            quat_track = &out.rotation;
        }
        else
        {
            error = "clip:" + std::to_string(reader.line()) + ": unknown field '" +
                    field->value + "'";
            return false;
        }
        if (const ClipAttribute* root = reader.find(tag, "root_motion"); root != nullptr)
        {
            if (field->value != "position")
            {
                error = "clip:" + std::to_string(reader.line()) +
                        ": root_motion applies to the position track only";
                return false;
            }
            if (root->value != "true" && root->value != "false")
            {
                error = "clip:" + std::to_string(reader.line()) +
                        ": root_motion must be true or false";
                return false;
            }
            out.position_root_motion = root->value == "true";
        }
        if ((vec_track != nullptr && !vec_track->keys.empty()) ||
            (quat_track != nullptr && !quat_track->keys.empty()))
        {
            error = "clip:" + std::to_string(reader.line()) + ": field '" + field->value +
                    "' carries two tracks";
            return false;
        }
        if (tag.self_closing)
        {
            continue;
        }
        for (;;)
        {
            if (!reader.skipMisc(error))
            {
                return false;
            }
            if (reader.atEnd())
            {
                error = "clip:" + std::to_string(reader.line()) + ": missing </track>";
                return false;
            }
            ClipTag key_tag;
            if (!reader.readTag(key_tag, error))
            {
                return false;
            }
            if (key_tag.closing)
            {
                if (key_tag.name != "track")
                {
                    error = "clip:" + std::to_string(reader.line()) + ": expected </track>";
                    return false;
                }
                break;
            }
            if (key_tag.name != "key")
            {
                error = "clip:" + std::to_string(reader.line()) + ": unknown element '<" +
                        key_tag.name + ">'";
                return false;
            }
            const ClipAttribute* t = reader.find(key_tag, "t");
            const ClipAttribute* value = reader.find(key_tag, "value");
            if (t == nullptr || value == nullptr)
            {
                error = "clip:" + std::to_string(reader.line()) + ": a key needs t and value";
                return false;
            }
            for (const ClipAttribute& attribute : key_tag.attributes)
            {
                if (attribute.name != "t" && attribute.name != "value")
                {
                    error = "clip:" + std::to_string(reader.line()) + ": unknown attribute '" +
                            attribute.name + "' on key";
                    return false;
                }
            }
            float time = 0.0F;
            std::size_t time_at = 0U;
            if (!parseNumber(t->value, time_at, time) || time_at != t->value.size())
            {
                error = "clip:" + std::to_string(reader.line()) + ": t must be one number";
                return false;
            }
            if (time < 0.0F || time > out.duration)
            {
                error = "clip:" + std::to_string(reader.line()) + ": t " + t->value +
                        " sits outside the clip";
                return false;
            }
            const float last = vec_track != nullptr && !vec_track->keys.empty()
                                   ? vec_track->keys.back().time
                                   : (quat_track != nullptr && !quat_track->keys.empty()
                                          ? quat_track->keys.back().time
                                          : -1.0F);
            if (time <= last)
            {
                error = "clip:" + std::to_string(reader.line()) + ": keys must ascend";
                return false;
            }
            float numbers[4] = {};
            const std::size_t count = quat_track != nullptr ? 4U : 3U;
            if (!parseNumbers(value->value, numbers, count, error))
            {
                error = "clip:" + std::to_string(reader.line()) + ": " + error;
                return false;
            }
            if (quat_track != nullptr)
            {
                quat_track->keys.push_back(
                    {time, Quat{numbers[0], numbers[1], numbers[2], numbers[3]}});
            }
            else
            {
                vec_track->keys.push_back({time, Vec3{numbers[0], numbers[1], numbers[2]}});
            }
        }
    }
    return true;
}

bool Clip::loadFile(const std::string& path, Clip& out, std::string& error)
{
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open())
    {
        error = path + ": cannot open the clip";
        return false;
    }
    const std::string text((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    const auto* const bytes = reinterpret_cast<const std::byte*>(text.data());
    if (!parseBytes(Span<const std::byte>(bytes, text.size()), out, error))
    {
        error = path + ": " + error;
        return false;
    }
    return true;
}

} // namespace resonate::animation
