#include <resonate/startup/cvar.h>

#include <algorithm>

#include <resonate/application/settings.h>

namespace resonate::startup
{

void CVarHost::bind(CVarBinding binding)
{
    for (CVarBinding& existing : bindings_)
    {
        if (existing.key == binding.key)
        {
            existing = std::move(binding);
            return;
        }
    }
    bindings_.push_back(std::move(binding));
}

bool CVarHost::apply(const std::string& key, const std::string& value, std::string& error)
{
    error.clear();
    CVarBinding* binding = nullptr;
    for (CVarBinding& existing : bindings_)
    {
        if (existing.key == key)
        {
            binding = &existing;
            break;
        }
    }
    if (binding == nullptr)
    {
        error = key + " is not a bound cvar";
        return false;
    }
    if (schema_ != nullptr && !schema_->checkValue(key.c_str(), value, error))
    {
        return false;
    }
    if (!binding->apply(binding->context, value, error))
    {
        if (error.empty())
        {
            error = key + " refused the value";
        }
        return false;
    }
    return true;
}

bool CVarHost::read(const std::string& key, std::string& out) const
{
    for (const CVarBinding& binding : bindings_)
    {
        if (binding.key == key)
        {
            out = binding.read != nullptr ? binding.read(binding.context) : std::string();
            return true;
        }
    }
    return false;
}

std::vector<std::string> CVarHost::find(const std::string& pattern) const
{
    std::vector<std::string> keys;
    for (const CVarBinding& binding : bindings_)
    {
        if (pattern.empty() || binding.key.find(pattern) != std::string::npos)
        {
            keys.push_back(binding.key);
        }
    }
    return keys;
}

} // namespace resonate::startup
