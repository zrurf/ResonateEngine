#include <resonate/ui/icon_registry.h>

#include <cstdlib>
#include <sstream>
#include <utility>

namespace resonate::ui
{

bool IconRegistry::load(std::string_view text, std::string& error)
{
    icons_.clear();
    std::istringstream lines{std::string(text)};
    std::string line;
    std::uint32_t number = 0U;
    while (std::getline(lines, line))
    {
        ++number;
        if (line.empty() || line.front() == '#')
        {
            continue;
        }
        std::istringstream fields(line);
        std::string name;
        Icon icon;
        if (!(fields >> name >> icon.uv_min.x >> icon.uv_min.y >> icon.uv_max.x >> icon.uv_max.y))
        {
            error = "icon map line " + std::to_string(number) + " needs a name and four fractions";
            return false;
        }
        if (icon.uv_min.x < 0.0F || icon.uv_min.y < 0.0F || icon.uv_max.x > 1.0F ||
            icon.uv_max.y > 1.0F || icon.uv_min.x >= icon.uv_max.x ||
            icon.uv_min.y >= icon.uv_max.y)
        {
            error = "icon map line " + std::to_string(number) + " names an invalid rect";
            return false;
        }
        icons_.insert_or_assign(std::move(name), icon);
    }
    return true;
}

const IconRegistry::Icon* IconRegistry::find(std::string_view name) const
{
    const auto found = icons_.find(std::string(name));
    return found == icons_.end() ? nullptr : &found->second;
}

} // namespace resonate::ui
