#include <resonate/gameplay/action_map.h>

#include <cctype>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include <resonate/window/input.h>

namespace resonate::gameplay
{
namespace
{

/* The action map's XML subset: elements, attributes and comments. There is no
   text content and no entity expansion, and an unknown element or attribute is
   an error — a typo in an asset should fail at load, not silently unbind. */

struct Attribute
{
    std::string name;
    std::string value;
};

struct Element
{
    std::string name;
    std::vector<Attribute> attributes;
    int line = 1;
    bool closing = false;
    bool self_closing = false;
};

enum class Next
{
    Element,
    End,
    Error,
};

std::string trim(const std::string& text)
{
    std::size_t begin = 0;
    while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin])) != 0)
    {
        ++begin;
    }
    std::size_t end = text.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1U])) != 0)
    {
        --end;
    }
    return text.substr(begin, end - begin);
}

bool describe(const std::string& path, int line, const std::string& message, std::string& error)
{
    error = path + ":" + std::to_string(line) + ": " + message;
    return false;
}

class Reader
{
  public:
    explicit Reader(const std::string& text) : text_(text)
    {
    }

    Next next(Element& out, std::string& error)
    {
        if (!skipMisc(error))
        {
            return Next::Error;
        }
        if (pos_ >= text_.size())
        {
            return Next::End;
        }
        if (text_[pos_] != '<')
        {
            error = "expected '<'";
            return Next::Error;
        }

        const int line = line_;
        const std::size_t start = pos_ + 1U;
        std::size_t end = start;
        bool quoted = false;
        for (; end < text_.size(); ++end)
        {
            const char character = text_[end];
            if (character == '\n')
            {
                ++line_;
            }
            if (quoted)
            {
                if (character == '"')
                {
                    quoted = false;
                }
                continue;
            }
            if (character == '"')
            {
                quoted = true;
            }
            else if (character == '>')
            {
                break;
            }
        }
        if (end >= text_.size())
        {
            error = "unterminated tag";
            return Next::Error;
        }

        std::string tag = text_.substr(start, end - start);
        pos_ = end + 1U;

        out = Element{};
        out.line = line;
        if (tag.empty())
        {
            error = "empty tag";
            return Next::Error;
        }
        if (tag[0] == '/')
        {
            out.closing = true;
            out.name = trim(tag.substr(1U));
            if (out.name.empty())
            {
                error = "empty closing tag";
                return Next::Error;
            }
            return Next::Element;
        }
        if (tag.back() == '/')
        {
            out.self_closing = true;
            tag.pop_back();
        }

        std::size_t name_end = 0;
        while (name_end < tag.size() &&
               std::isspace(static_cast<unsigned char>(tag[name_end])) == 0)
        {
            ++name_end;
        }
        out.name = tag.substr(0U, name_end);
        if (out.name.empty())
        {
            error = "empty element name";
            return Next::Error;
        }
        return parseAttributes(tag, name_end, out, error) ? Next::Element : Next::Error;
    }

  private:
    bool skipMisc(std::string& error)
    {
        while (true)
        {
            while (pos_ < text_.size() &&
                   std::isspace(static_cast<unsigned char>(text_[pos_])) != 0)
            {
                if (text_[pos_] == '\n')
                {
                    ++line_;
                }
                ++pos_;
            }
            if (text_.compare(pos_, 4U, "<!--") == 0)
            {
                const std::size_t close = text_.find("-->", pos_ + 4U);
                if (close == std::string::npos)
                {
                    error = "unterminated comment";
                    return false;
                }
                for (std::size_t index = pos_; index < close; ++index)
                {
                    if (text_[index] == '\n')
                    {
                        ++line_;
                    }
                }
                pos_ = close + 3U;
                continue;
            }
            if (text_.compare(pos_, 2U, "<?") == 0)
            {
                const std::size_t close = text_.find("?>", pos_ + 2U);
                if (close == std::string::npos)
                {
                    error = "unterminated declaration";
                    return false;
                }
                pos_ = close + 2U;
                continue;
            }
            return true;
        }
    }

    bool parseAttributes(const std::string& tag, std::size_t begin, Element& out,
                         std::string& error)
    {
        std::size_t index = begin;
        while (index < tag.size())
        {
            while (index < tag.size() &&
                   std::isspace(static_cast<unsigned char>(tag[index])) != 0)
            {
                ++index;
            }
            if (index >= tag.size())
            {
                break;
            }

            const std::size_t name_begin = index;
            while (index < tag.size() &&
                   std::isspace(static_cast<unsigned char>(tag[index])) == 0 &&
                   tag[index] != '=')
            {
                ++index;
            }
            const std::string name = tag.substr(name_begin, index - name_begin);
            if (name.empty())
            {
                error = "attribute without a name";
                return false;
            }
            while (index < tag.size() &&
                   std::isspace(static_cast<unsigned char>(tag[index])) != 0)
            {
                ++index;
            }
            if (index >= tag.size() || tag[index] != '=')
            {
                error = "attribute '" + name + "' has no value";
                return false;
            }
            ++index;
            while (index < tag.size() &&
                   std::isspace(static_cast<unsigned char>(tag[index])) != 0)
            {
                ++index;
            }
            if (index >= tag.size() || tag[index] != '"')
            {
                error = "attribute '" + name + "' is not quoted";
                return false;
            }
            ++index;
            const std::size_t value_begin = index;
            while (index < tag.size() && tag[index] != '"')
            {
                ++index;
            }
            if (index >= tag.size())
            {
                error = "attribute '" + name + "' is not closed";
                return false;
            }
            for (const Attribute& attribute : out.attributes)
            {
                if (attribute.name == name)
                {
                    error = "attribute '" + name + "' is repeated";
                    return false;
                }
            }
            out.attributes.emplace_back(Attribute{name, tag.substr(value_begin, index - value_begin)});
            ++index;
        }
        return true;
    }

    const std::string& text_;
    std::size_t pos_ = 0;
    int line_ = 1;
};

const Attribute* findAttribute(const Element& element, const char* name)
{
    for (const Attribute& attribute : element.attributes)
    {
        if (attribute.name == name)
        {
            return &attribute;
        }
    }
    return nullptr;
}

bool checkAttributes(const Element& element, const std::string& path,
                     const std::vector<const char*>& allowed, std::string& error)
{
    for (const Attribute& attribute : element.attributes)
    {
        bool known = false;
        for (const char* candidate : allowed)
        {
            if (attribute.name == candidate)
            {
                known = true;
                break;
            }
        }
        if (!known)
        {
            return describe(path, element.line,
                            "unknown attribute '" + attribute.name + "' on <" + element.name +
                                ">",
                            error);
        }
    }
    return true;
}

bool keyFromName(const std::string& name, ResonateKey& out)
{
    if (name.size() == 1U)
    {
        const char character = name[0];
        if (character >= 'a' && character <= 'z')
        {
            out = static_cast<ResonateKey>(static_cast<int>(RESONATE_KEY_A) +
                                           (character - 'a'));
            return true;
        }
        switch (character)
        {
        case '0':
            out = RESONATE_KEY_0;
            return true;
        case '1':
            out = RESONATE_KEY_1;
            return true;
        case '2':
            out = RESONATE_KEY_2;
            return true;
        case '3':
            out = RESONATE_KEY_3;
            return true;
        case '4':
            out = RESONATE_KEY_4;
            return true;
        case '5':
            out = RESONATE_KEY_5;
            return true;
        case '6':
            out = RESONATE_KEY_6;
            return true;
        case '7':
            out = RESONATE_KEY_7;
            return true;
        case '8':
            out = RESONATE_KEY_8;
            return true;
        case '9':
            out = RESONATE_KEY_9;
            return true;
        default:
            return false;
        }
    }

    struct Named
    {
        const char* name;
        ResonateKey key;
    };
    static const Named kNamed[] = {
        {"space", RESONATE_KEY_SPACE},           {"escape", RESONATE_KEY_ESCAPE},
        {"return", RESONATE_KEY_RETURN},         {"enter", RESONATE_KEY_RETURN},
        {"tab", RESONATE_KEY_TAB},               {"backspace", RESONATE_KEY_BACKSPACE},
        {"minus", RESONATE_KEY_MINUS},           {"equals", RESONATE_KEY_EQUALS},
        {"up", RESONATE_KEY_UP},                 {"down", RESONATE_KEY_DOWN},
        {"left", RESONATE_KEY_LEFT},             {"right", RESONATE_KEY_RIGHT},
        {"delete", RESONATE_KEY_DELETE},         {"left-shift", RESONATE_KEY_LSHIFT},
        {"right-shift", RESONATE_KEY_RSHIFT},    {"left-ctrl", RESONATE_KEY_LCTRL},
        {"right-ctrl", RESONATE_KEY_RCTRL},      {"left-alt", RESONATE_KEY_LALT},
        {"right-alt", RESONATE_KEY_RALT},
    };
    for (const Named& named : kNamed)
    {
        if (name == named.name)
        {
            out = named.key;
            return true;
        }
    }
    return false;
}

bool mouseFromName(const std::string& name, ResonateMouseButton& out)
{
    if (name == "left-button")
    {
        out = RESONATE_MOUSE_LEFT;
        return true;
    }
    if (name == "right-button")
    {
        out = RESONATE_MOUSE_RIGHT;
        return true;
    }
    if (name == "middle-button")
    {
        out = RESONATE_MOUSE_MIDDLE;
        return true;
    }
    return false;
}

enum class GamepadKind
{
    Button,
    Axis,
};

struct GamepadBinding
{
    GamepadKind kind = GamepadKind::Button;
    ResonateGamepadButton button = RESONATE_GAMEPAD_SOUTH;
    ResonateGamepadAxis axis = RESONATE_GAMEPAD_AXIS_RIGHT_TRIGGER;
};

bool gamepadFromName(const std::string& name, GamepadKind& kind, ResonateGamepadButton& button,
                     ResonateGamepadAxis& axis)
{
    /* Triggers are analog, but an action is digital for now: a trigger past the
       halfway point counts as down. */
    if (name == "left-trigger")
    {
        kind = GamepadKind::Axis;
        axis = RESONATE_GAMEPAD_AXIS_LEFT_TRIGGER;
        return true;
    }
    if (name == "right-trigger")
    {
        kind = GamepadKind::Axis;
        axis = RESONATE_GAMEPAD_AXIS_RIGHT_TRIGGER;
        return true;
    }

    struct Named
    {
        const char* name;
        ResonateGamepadButton button;
    };
    static const Named kNamed[] = {
        {"south", RESONATE_GAMEPAD_SOUTH},
        {"east", RESONATE_GAMEPAD_EAST},
        {"west", RESONATE_GAMEPAD_WEST},
        {"north", RESONATE_GAMEPAD_NORTH},
        {"back", RESONATE_GAMEPAD_BACK},
        {"guide", RESONATE_GAMEPAD_GUIDE},
        {"start", RESONATE_GAMEPAD_START},
        {"left-stick", RESONATE_GAMEPAD_LEFT_STICK},
        {"right-stick", RESONATE_GAMEPAD_RIGHT_STICK},
        {"left-shoulder", RESONATE_GAMEPAD_LEFT_SHOULDER},
        {"right-shoulder", RESONATE_GAMEPAD_RIGHT_SHOULDER},
        {"dpad-up", RESONATE_GAMEPAD_DPAD_UP},
        {"dpad-down", RESONATE_GAMEPAD_DPAD_DOWN},
        {"dpad-left", RESONATE_GAMEPAD_DPAD_LEFT},
        {"dpad-right", RESONATE_GAMEPAD_DPAD_RIGHT},
    };
    for (const Named& named : kNamed)
    {
        if (name == named.name)
        {
            kind = GamepadKind::Button;
            button = named.button;
            return true;
        }
    }
    return false;
}

struct ActionData
{
    std::string name;
    std::vector<GamepadBinding> gamepad;
};

struct Binding
{
    std::string action;
    enum class Device
    {
        Keyboard,
        Mouse,
        Gamepad,
    } device = Device::Keyboard;
    ResonateKey key = RESONATE_KEY_UNKNOWN;
    ResonateMouseButton button = RESONATE_MOUSE_LEFT;
    GamepadBinding gamepad;
};

bool expectEmpty(Reader& reader, const Element& element, const std::string& path,
                 std::string& error)
{
    if (element.self_closing)
    {
        return true;
    }
    Element child;
    const Next next = reader.next(child, error);
    if (next == Next::Error)
    {
        error = path + ": " + error;
        return false;
    }
    if (next != Next::Element || !child.closing || child.name != element.name)
    {
        return describe(path, element.line, "<" + element.name + "> has no content", error);
    }
    return true;
}

bool parseAction(Reader& reader, const Element& element, std::vector<ActionData>& actions,
                 const std::string& path, std::string& error)
{
    if (!checkAttributes(element, path, {"name", "value"}, error))
    {
        return false;
    }
    const Attribute* name = findAttribute(element, "name");
    if (name == nullptr || name->value.empty())
    {
        return describe(path, element.line, "an action needs a name", error);
    }
    if (const Attribute* value = findAttribute(element, "value");
        value != nullptr && value->value != "button" && value->value != "axis")
    {
        return describe(path, element.line,
                        "action '" + name->value + "' has value '" + value->value +
                            "'; expected button or axis",
                        error);
    }
    for (const ActionData& action : actions)
    {
        if (action.name == name->value)
        {
            return describe(path, element.line, "action '" + name->value + "' is declared twice",
                            error);
        }
    }

    if (!element.self_closing)
    {
        while (true)
        {
            Element child;
            const Next next = reader.next(child, error);
            if (next == Next::Error)
            {
                error = path + ": " + error;
                return false;
            }
            if (next == Next::End)
            {
                return describe(path, element.line, "<action> is not closed", error);
            }
            if (child.closing)
            {
                if (child.name != "action")
                {
                    return describe(path, child.line, "unexpected </" + child.name + ">", error);
                }
                break;
            }
            if (child.name != "phase")
            {
                return describe(path, child.line,
                                "unknown element <" + child.name + "> inside <action>", error);
            }
            if (!checkAttributes(child, path, {"name"}, error))
            {
                return false;
            }
            const Attribute* phase = findAttribute(child, "name");
            if (phase == nullptr || (phase->value != "pressed" && phase->value != "released"))
            {
                return describe(path, child.line,
                                "a phase is pressed or released", error);
            }
            if (!expectEmpty(reader, child, path, error))
            {
                return false;
            }
        }
    }

    actions.push_back(ActionData{name->value, {}});
    return true;
}

bool parseBinding(Reader& reader, const Element& element, std::vector<Binding>& bindings,
                  const std::string& path, std::string& error)
{
    if (!checkAttributes(element, path, {"action", "device", "control"}, error))
    {
        return false;
    }
    const Attribute* action = findAttribute(element, "action");
    const Attribute* device = findAttribute(element, "device");
    const Attribute* control = findAttribute(element, "control");
    if (action == nullptr || action->value.empty() || device == nullptr ||
        control == nullptr || control->value.empty())
    {
        return describe(path, element.line, "a binding needs action, device and control", error);
    }

    Binding binding;
    binding.action = action->value;
    if (device->value == "keyboard")
    {
        binding.device = Binding::Device::Keyboard;
        if (!keyFromName(control->value, binding.key))
        {
            return describe(path, element.line,
                            "unknown keyboard control '" + control->value + "'", error);
        }
    }
    else if (device->value == "mouse")
    {
        binding.device = Binding::Device::Mouse;
        if (!mouseFromName(control->value, binding.button))
        {
            return describe(path, element.line,
                            "unknown mouse control '" + control->value + "'", error);
        }
    }
    else if (device->value == "gamepad")
    {
        binding.device = Binding::Device::Gamepad;
        if (!gamepadFromName(control->value, binding.gamepad.kind, binding.gamepad.button,
                             binding.gamepad.axis))
        {
            return describe(path, element.line,
                            "unknown gamepad control '" + control->value + "'", error);
        }
    }
    else
    {
        return describe(path, element.line,
                        "unknown device '" + device->value +
                            "'; expected keyboard, mouse or gamepad",
                        error);
    }

    if (!expectEmpty(reader, element, path, error))
    {
        return false;
    }
    bindings.push_back(std::move(binding));
    return true;
}

bool parseDocument(const std::string& path, const std::string& text,
                   std::vector<ActionData>& actions, std::vector<Binding>& bindings,
                   std::string& error)
{
    Reader reader(text);
    Element root;
    const Next root_next = reader.next(root, error);
    if (root_next == Next::Error)
    {
        error = path + ": " + error;
        return false;
    }
    if (root_next != Next::Element || root.closing || root.name != "action-map")
    {
        error = path + ": expected <action-map>";
        return false;
    }
    if (!root.attributes.empty())
    {
        return describe(path, root.line, "<action-map> takes no attributes", error);
    }

    if (!root.self_closing)
    {
        while (true)
        {
            Element element;
            const Next next = reader.next(element, error);
            if (next == Next::Error)
            {
                error = path + ": " + error;
                return false;
            }
            if (next == Next::End)
            {
                return describe(path, root.line, "<action-map> is not closed", error);
            }
            if (element.closing)
            {
                if (element.name != "action-map")
                {
                    return describe(path, element.line, "unexpected </" + element.name + ">",
                                    error);
                }
                break;
            }
            if (element.name == "action")
            {
                if (!parseAction(reader, element, actions, path, error))
                {
                    return false;
                }
            }
            else if (element.name == "binding")
            {
                if (!parseBinding(reader, element, bindings, path, error))
                {
                    return false;
                }
            }
            else
            {
                return describe(path, element.line,
                                "unknown element <" + element.name + ">", error);
            }
        }
    }

    Element trailing;
    const Next trailing_next = reader.next(trailing, error);
    if (trailing_next == Next::Error)
    {
        error = path + ": " + error;
        return false;
    }
    if (trailing_next != Next::End)
    {
        return describe(path, trailing.line, "content after </action-map>", error);
    }
    if (actions.empty())
    {
        error = path + ": no actions declared";
        return false;
    }

    for (const Binding& binding : bindings)
    {
        bool known = false;
        for (const ActionData& action : actions)
        {
            if (action.name == binding.action)
            {
                known = true;
                break;
            }
        }
        if (!known)
        {
            error = path + ": binding names undeclared action '" + binding.action + "'";
            return false;
        }
    }
    return true;
}

} // namespace

struct ActionMap::Impl
{
    struct Action
    {
        std::string name;
        std::vector<ResonateKey> keys;
        std::vector<ResonateMouseButton> buttons;
        std::vector<GamepadBinding> gamepad;
        ActionState state;
        bool was_held = false;
    };

    std::vector<Action> actions;
};

ActionMap::ActionMap() : impl_(std::make_unique<Impl>())
{
}

ActionMap::~ActionMap() = default;

ActionMap::ActionMap(ActionMap&&) noexcept = default;

ActionMap& ActionMap::operator=(ActionMap&&) noexcept = default;

bool ActionMap::load(const std::string& path, std::string& error)
{
    error.clear();

    std::ifstream in(path, std::ios::binary);
    if (!in.is_open())
    {
        error = "cannot read " + path;
        return false;
    }
    const std::string text((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());

    std::vector<ActionData> parsed_actions;
    std::vector<Binding> bindings;
    if (!parseDocument(path, text, parsed_actions, bindings, error))
    {
        return false;
    }

    /* The document is valid; replace the map wholesale so a failed load never
       leaves half a rebind applied. */
    impl_->actions.clear();
    impl_->actions.reserve(parsed_actions.size());
    for (ActionData& parsed : parsed_actions)
    {
        Impl::Action action;
        action.name = std::move(parsed.name);
        impl_->actions.push_back(std::move(action));
    }
    for (const Binding& binding : bindings)
    {
        for (Impl::Action& action : impl_->actions)
        {
            if (action.name != binding.action)
            {
                continue;
            }
            switch (binding.device)
            {
            case Binding::Device::Keyboard:
                action.keys.push_back(binding.key);
                break;
            case Binding::Device::Mouse:
                action.buttons.push_back(binding.button);
                break;
            case Binding::Device::Gamepad:
                action.gamepad.push_back(binding.gamepad);
                break;
            }
            break;
        }
    }
    return true;
}

void ActionMap::update(const ResonateInput* input)
{
    for (Impl::Action& action : impl_->actions)
    {
        bool down = false;
        if (input != nullptr && input->key_down != nullptr)
        {
            for (const ResonateKey key : action.keys)
            {
                if (input->key_down(input->self, key) != 0)
                {
                    down = true;
                    break;
                }
            }
        }
        if (!down && input != nullptr && input->mouse_down != nullptr)
        {
            for (const ResonateMouseButton button : action.buttons)
            {
                if (input->mouse_down(input->self, button) != 0)
                {
                    down = true;
                    break;
                }
            }
        }
        if (!down && input != nullptr && input->gamepad_count != nullptr &&
            input->gamepad_button_down != nullptr && input->gamepad_axis != nullptr)
        {
            const int32_t pads = input->gamepad_count(input->self);
            for (int32_t pad = 0; pad < pads && !down; ++pad)
            {
                for (const GamepadBinding& binding : action.gamepad)
                {
                    const bool pressed =
                        binding.kind == GamepadKind::Button
                            ? input->gamepad_button_down(input->self, pad, binding.button) != 0
                            : input->gamepad_axis(input->self, pad, binding.axis) >= 0.5F;
                    if (pressed)
                    {
                        down = true;
                        break;
                    }
                }
            }
        }

        action.state.pressed = down && !action.was_held;
        action.state.held = down;
        action.state.released = !down && action.was_held;
        action.was_held = down;
    }
}

std::size_t ActionMap::actionCount() const noexcept
{
    return impl_->actions.size();
}

const ActionState* ActionMap::state(const char* action) const noexcept
{
    if (action == nullptr)
    {
        return nullptr;
    }
    for (const Impl::Action& entry : impl_->actions)
    {
        if (entry.name == action)
        {
            return &entry.state;
        }
    }
    return nullptr;
}

} // namespace resonate::gameplay
