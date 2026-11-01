#include <resonate/scene/machine_model.h>

#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace resonate::scene
{
namespace
{

/* The .rclip family's restricted reader: elements with attributes, comments
   and declarations skipped, anything unnamed refused with a line number. One
   reader per grammar keeps each format's refusals precise. */
struct MachineAttribute
{
    std::string name;
    std::string value;
};

struct MachineTag
{
    std::string name;
    std::vector<MachineAttribute> attributes;
    bool closing = false;
    bool self_closing = false;
};

class MachineReader
{
  public:
    explicit MachineReader(std::string_view text) : text_(text)
    {
        static const char kBom[] = "\xEF\xBB\xBF";
        if (text_.substr(0U, 3U) == kBom)
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

    bool readTag(MachineTag& out, std::string& error)
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
            MachineAttribute attribute;
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
            const char quote = peek();
            if (atEnd() || (quote != '"' && quote != '\''))
            {
                return fail("the value of '" + attribute.name + "' needs quotes", error);
            }
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
            for (const MachineAttribute& existing : out.attributes)
            {
                if (existing.name == attribute.name)
                {
                    return fail("duplicate attribute '" + attribute.name + "'", error);
                }
            }
            out.attributes.push_back(std::move(attribute));
        }
    }

    [[nodiscard]] const MachineAttribute* find(const MachineTag& tag,
                                               const char* name) const
    {
        for (const MachineAttribute& item : tag.attributes)
        {
            if (item.name == name)
            {
                return &item;
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
        error = "machine:" + std::to_string(line_) + ": " + message;
        return false;
    }

    std::string_view text_;
    std::size_t pos_ = 0U;
    int line_ = 1;
};

bool isNumber(std::string_view text)
{
    const std::string owned(text);
    char* stop = nullptr;
    std::strtof(owned.c_str(), &stop);
    return stop == owned.c_str() + owned.size() && !owned.empty();
}

std::string escape(std::string_view text)
{
    std::string out;
    for (const char character : text)
    {
        switch (character)
        {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '"':
            out += "&quot;";
            break;
        default:
            out.push_back(character);
        }
    }
    return out;
}

const MachineAttribute* attribute(const MachineTag& tag, const char* name)
{
    for (const MachineAttribute& item : tag.attributes)
    {
        if (item.name == name)
        {
            return &item;
        }
    }
    return nullptr;
}

} // namespace

bool parseMachineFragment(std::string_view xml, MachineModel& out, std::string& error)
{
    out = MachineModel{};
    MachineReader reader(xml);
    if (!reader.skipMisc(error))
    {
        return false;
    }
    MachineTag root;
    if (!reader.readTag(root, error))
    {
        return false;
    }
    if (root.closing || root.name != "Machine")
    {
        error = "machine:" + std::to_string(reader.line()) + ": the root is not a Machine";
        return false;
    }
    if (const MachineAttribute* entry = attribute(root, "entry"); entry != nullptr)
    {
        out.entry = entry->value;
    }
    if (const MachineAttribute* parameters = attribute(root, "parameters");
        parameters != nullptr)
    {
        out.parameters = parameters->value;
    }
    if (root.self_closing)
    {
        error = "machine:" + std::to_string(reader.line()) + ": the Machine has no states";
        return false;
    }
    std::map<std::string, std::uint32_t> names;
    for (;;)
    {
        if (!reader.skipMisc(error))
        {
            return false;
        }
        if (reader.atEnd())
        {
            error = "machine:" + std::to_string(reader.line()) + ": missing </Machine>";
            return false;
        }
        MachineTag tag;
        if (!reader.readTag(tag, error))
        {
            return false;
        }
        if (tag.closing)
        {
            if (tag.name != "Machine")
            {
                error = "machine:" + std::to_string(reader.line()) + ": expected </Machine>";
                return false;
            }
            break;
        }
        if (tag.name != "State" && tag.name != "Transition")
        {
            error = "machine:" + std::to_string(reader.line()) + ": unknown element '<" +
                    tag.name + ">'";
            return false;
        }
        if (tag.name == "State")
        {
            MachineModelState state;
            const MachineAttribute* name = attribute(tag, "name");
            const MachineAttribute* clip = attribute(tag, "clip");
            if (name == nullptr || clip == nullptr || clip->value.empty())
            {
                error = "machine:" + std::to_string(reader.line()) +
                        ": a state needs a name and a clip";
                return false;
            }
            state.name = name->value;
            state.clip = clip->value;
            if (const MachineAttribute* looping = attribute(tag, "looping");
                looping != nullptr)
            {
                if (looping->value != "true" && looping->value != "false")
                {
                    error = "machine:" + std::to_string(reader.line()) + ": the state '" +
                            state.name + "' has looping='" + looping->value +
                            "'; it is true or false";
                    return false;
                }
                state.looping = looping->value;
            }
            for (const char* position : {"x", "y"})
            {
                if (const MachineAttribute* authored = attribute(tag, position);
                    authored != nullptr && !isNumber(authored->value))
                {
                    error = "machine:" + std::to_string(reader.line()) + ": the state '" +
                            state.name + "' has " + position + "='" + authored->value +
                            "'; it must be a number";
                    return false;
                }
            }
            if (const MachineAttribute* authored = attribute(tag, "x"); authored != nullptr)
            {
                state.x = authored->value;
            }
            if (const MachineAttribute* authored = attribute(tag, "y"); authored != nullptr)
            {
                state.y = authored->value;
            }
            for (const MachineAttribute& item : tag.attributes)
            {
                if (item.name != "name" && item.name != "clip" && item.name != "looping" &&
                    item.name != "x" && item.name != "y")
                {
                    error = "machine:" + std::to_string(reader.line()) +
                            ": unknown attribute '" + item.name + "' on State";
                    return false;
                }
            }
            if (names.find(state.name) != names.end())
            {
                error = "machine:" + std::to_string(reader.line()) + ": the Machine names state '" +
                        state.name + "' twice";
                return false;
            }
            names.emplace(state.name, static_cast<std::uint32_t>(out.states.size()));
            out.states.push_back(std::move(state));
            if (!tag.self_closing)
            {
                error = "machine:" + std::to_string(reader.line()) +
                        ": a state takes no children";
                return false;
            }
            continue;
        }
        if (!tag.self_closing)
        {
            MachineModelTransition transition;
            if (const MachineAttribute* from = attribute(tag, "from"); from != nullptr)
            {
                transition.from = from->value;
            }
            if (const MachineAttribute* to = attribute(tag, "to"); to != nullptr)
            {
                transition.to = to->value;
            }
            if (const MachineAttribute* authored = attribute(tag, "at_end"); authored != nullptr)
            {
                if (authored->value != "true" && authored->value != "false")
                {
                    error = "machine:" + std::to_string(reader.line()) + ": at_end is true or false";
                    return false;
                }
                transition.at_end = authored->value;
            }
            if (const MachineAttribute* authored = attribute(tag, "fade"); authored != nullptr)
            {
                transition.fade = authored->value;
            }
            if (const MachineAttribute* authored = attribute(tag, "sync"); authored != nullptr)
            {
                if (authored->value != "true" && authored->value != "false")
                {
                    error = "machine:" + std::to_string(reader.line()) + ": sync is true or false";
                    return false;
                }
                transition.sync = authored->value;
            }
            for (const MachineAttribute& item : tag.attributes)
            {
                if (item.name != "from" && item.name != "to" && item.name != "at_end" &&
                    item.name != "fade" && item.name != "sync")
                {
                    error = "machine:" + std::to_string(reader.line()) +
                            ": unknown attribute '" + item.name + "' on Transition";
                    return false;
                }
            }
            for (;;)
            {
                if (!reader.skipMisc(error))
                {
                    return false;
                }
                if (reader.atEnd())
                {
                    error = "machine:" + std::to_string(reader.line()) + ": missing </Transition>";
                    return false;
                }
                MachineTag condition_tag;
                if (!reader.readTag(condition_tag, error))
                {
                    return false;
                }
                if (condition_tag.closing)
                {
                    if (condition_tag.name != "Transition")
                    {
                        error = "machine:" + std::to_string(reader.line()) +
                                ": expected </Transition>";
                        return false;
                    }
                    break;
                }
                if (condition_tag.name != "Condition")
                {
                    error = "machine:" + std::to_string(reader.line()) +
                            ": a transition holds only Condition elements";
                    return false;
                }
                MachineModelCondition condition;
                const MachineAttribute* parameter = attribute(condition_tag, "parameter");
                if (parameter == nullptr || parameter->value.empty())
                {
                    error = "machine:" + std::to_string(reader.line()) +
                            ": a condition needs a parameter";
                    return false;
                }
                condition.parameter = parameter->value;
                static const char* const kTests[] = {"greater", "less",           "equal",
                                                     "not_equal", "trigger",     "is"};
                for (const char* test_name : kTests)
                {
                    const std::string_view test = test_name;
                    if (const MachineAttribute* authored = attribute(condition_tag, test_name);
                        authored != nullptr)
                    {
                        if (!condition.test.empty())
                        {
                            error = "machine:" + std::to_string(reader.line()) +
                                    ": a condition carries two tests";
                            return false;
                        }
                        condition.test = test;
                        condition.value = authored->value;
                        if ((test == "greater" || test == "less" || test == "equal" ||
                             test == "not_equal") &&
                            !isNumber(condition.value))
                        {
                            error = "machine:" + std::to_string(reader.line()) +
                                    ": the condition's " + std::string(test) + " must be one number";
                            return false;
                        }
                        if ((test == "trigger" || test == "is") &&
                            condition.value != "true" && condition.value != "false")
                        {
                            error = "machine:" + std::to_string(reader.line()) + ": " + std::string(test) +
                                    " is true or false";
                            return false;
                        }
                    }
                }
                for (const MachineAttribute& item : condition_tag.attributes)
                {
                    bool known = item.name == "parameter";
                    for (const char* test_name : kTests)
                    {
                        known = known || item.name == std::string_view(test_name);
                    }
                    if (!known)
                    {
                        error = "machine:" + std::to_string(reader.line()) +
                                ": unknown attribute '" + item.name + "' on Condition";
                        return false;
                    }
                }
                if (condition.test.empty())
                {
                    error = "machine:" + std::to_string(reader.line()) +
                            ": a condition carries no test";
                    return false;
                }
                transition.conditions.push_back(std::move(condition));
                if (condition_tag.self_closing)
                {
                    continue;
                }
                error = "machine:" + std::to_string(reader.line()) +
                        ": a condition takes no children";
                return false;
            }
            out.transitions.push_back(std::move(transition));
            continue;
        }
        /* A self-closing transition: no conditions. */
        MachineModelTransition transition;
        if (const MachineAttribute* from = attribute(tag, "from"); from != nullptr)
        {
            transition.from = from->value;
        }
        if (const MachineAttribute* to = attribute(tag, "to"); to != nullptr)
        {
            transition.to = to->value;
        }
        out.transitions.push_back(std::move(transition));
    }
    if (out.states.empty())
    {
        error = "machine:" + std::to_string(reader.line()) + ": the Machine has no states";
        return false;
    }
    for (const MachineModelTransition& transition : out.transitions)
    {
        if (transition.from != "*" && names.find(transition.from) == names.end())
        {
            error = "machine:" + std::to_string(reader.line()) +
                    ": a transition starts at the unknown state '" + transition.from + "'";
            return false;
        }
        if (names.find(transition.to) == names.end())
        {
            error = "machine:" + std::to_string(reader.line()) +
                    ": a transition targets the unknown state '" + transition.to + "'";
            return false;
        }
    }
    if (!out.entry.empty() && names.find(out.entry) == names.end())
    {
        error = "machine:" + std::to_string(reader.line()) + ": the entry '" + out.entry +
                "' names no state";
        return false;
    }
    return true;
}

std::string serializeMachineFragment(const MachineModel& model)
{
    std::string out = "<Machine";
    if (!model.entry.empty())
    {
        out += " entry=\"" + escape(model.entry) + "\"";
    }
    if (!model.parameters.empty())
    {
        out += " parameters=\"" + escape(model.parameters) + "\"";
    }
    out += ">\n";
    for (const MachineModelState& state : model.states)
    {
        out += "  <State name=\"" + escape(state.name) + "\" clip=\"" +
               escape(state.clip) + "\"";
        if (!state.looping.empty())
        {
            out += " looping=\"" + state.looping + "\"";
        }
        if (!state.x.empty())
        {
            out += " x=\"" + escape(state.x) + "\"";
        }
        if (!state.y.empty())
        {
            out += " y=\"" + escape(state.y) + "\"";
        }
        out += "/>\n";
    }
    for (const MachineModelTransition& transition : model.transitions)
    {
        out += "  <Transition from=\"" + escape(transition.from) + "\" to=\"" +
               escape(transition.to) + "\"";
        if (!transition.at_end.empty())
        {
            out += " at_end=\"" + transition.at_end + "\"";
        }
        if (!transition.fade.empty())
        {
            out += " fade=\"" + escape(transition.fade) + "\"";
        }
        if (!transition.sync.empty())
        {
            out += " sync=\"" + transition.sync + "\"";
        }
        if (transition.conditions.empty())
        {
            out += "/>\n";
            continue;
        }
        out += ">\n";
        for (const MachineModelCondition& condition : transition.conditions)
        {
            out += "    <Condition parameter=\"" + escape(condition.parameter) + "\" " +
                   condition.test + "=\"";
            if (condition.test == "trigger" || condition.test == "is")
            {
                out += condition.value;
            }
            else
            {
                out += escape(condition.value);
            }
            out += "\"/>\n";
        }
        out += "  </Transition>\n";
    }
    out += "</Machine>";
    return out;
}

} // namespace resonate::scene
