#include <resonate/ui/image_registry.h>

namespace resonate::ui
{

std::uint32_t ImageRegistry::registerImage(std::string_view path)
{
    auto found = ids_.find(std::string(path));
    if (found != ids_.end())
    {
        return found->second;
    }
    const auto id = static_cast<std::uint32_t>(ids_.size()) + kFirstId;
    ids_.emplace(std::string(path), id);
    return id;
}

std::uint32_t ImageRegistry::find(std::string_view path) const
{
    const auto found = ids_.find(std::string(path));
    return found != ids_.end() ? found->second : kUnregistered;
}

} // namespace resonate::ui
