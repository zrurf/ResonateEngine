#include <resonate/renderer/renderer.h>

#include <algorithm>
#include <utility>

namespace resonate::renderer
{

namespace
{

void trimInPlace(std::string& text)
{
    const std::size_t first = text.find_first_not_of(" \t\r\n");
    const std::size_t last = text.find_last_not_of(" \t\r\n");
    if (first == std::string::npos)
    {
        text.clear();
        return;
    }
    text = text.substr(first, last - first + 1U);
}

} // namespace

bool FeatureStack::has(const std::string& name) const
{
    return std::find(chain.begin(), chain.end(), name) != chain.end();
}

bool FeatureStack::antialiases() const
{
    return antialiasing != "none";
}

FeatureStack FeatureStack::defaults()
{
    FeatureStack stack;
    stack.chain = {"bloom", "tonemap"};
    stack.antialiasing = "fxaa";
    stack.bloom_threshold = 1.0F;
    return stack;
}

bool validateFeatureStack(const FeatureStack& stack, std::string& out_error)
{
    bool bloom = false;
    bool tonemap = false;
    for (const std::string& name : stack.chain)
    {
        if (name == "bloom")
        {
            if (bloom)
            {
                out_error = "post feature 'bloom' appears twice";
                return false;
            }
            bloom = true;
            if (tonemap)
            {
                out_error = "post feature 'bloom' must precede 'tonemap'";
                return false;
            }
        }
        else if (name == "tonemap")
        {
            if (tonemap)
            {
                out_error = "post feature 'tonemap' appears twice";
                return false;
            }
            tonemap = true;
        }
        else
        {
            out_error = "unknown post feature '" + name + "'";
            return false;
        }
    }
    if (!tonemap)
    {
        out_error = "the post chain requires 'tonemap'";
        return false;
    }
    if (stack.antialiasing != "fxaa" && stack.antialiasing != "none")
    {
        out_error = "unknown antialiasing '" + stack.antialiasing + "' (fxaa or none)";
        return false;
    }
    out_error.clear();
    return true;
}

bool parseFeatureStack(const std::string& chain, const std::string& antialiasing,
                       FeatureStack& out_stack, std::string& out_error)
{
    FeatureStack parsed;
    std::size_t begin = 0;
    while (begin <= chain.size())
    {
        const std::size_t end = chain.find(',', begin);
        std::string name =
            chain.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
        trimInPlace(name);
        if (!name.empty())
        {
            parsed.chain.push_back(name);
        }
        if (end == std::string::npos)
        {
            break;
        }
        begin = end + 1U;
    }

    parsed.antialiasing = antialiasing;
    trimInPlace(parsed.antialiasing);
    if (parsed.antialiasing.empty())
    {
        parsed.antialiasing = "fxaa";
    }
    if (!validateFeatureStack(parsed, out_error))
    {
        return false;
    }
    out_stack = std::move(parsed);
    return true;
}

} // namespace resonate::renderer
