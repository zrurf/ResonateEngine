#ifndef RESONATE_RUNTIME_TOML_TEXT_H
#define RESONATE_RUNTIME_TOML_TEXT_H

/* One place decides how a TOML scalar becomes the text form the store and the
   settings schema both read: the config store hands it to modules as text, and
   the schema validates that same text. Keeping the two in one function is what
   keeps "the value the module saw" and "the value validation checked" identical.

   A bad file is a diagnostic, not an exception unwinding the run: the macro has
   to precede toml++, and every TU that includes it must agree. */

#if !defined(TOML_EXCEPTIONS)
#    define TOML_EXCEPTIONS 0
#endif

#include <toml++/toml.hpp>

#include <string>
#include <type_traits>

namespace resonate::detail
{

/* The scalar's text form; false for a table, an array or a date-time. */
inline bool scalarText(const toml::node& node, std::string& out)
{
    bool found = false;
    node.visit(
        [&](auto&& value)
        {
            using Value = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Value, toml::value<std::string>>)
            {
                out = value.get();
                found = true;
            }
            else if constexpr (std::is_same_v<Value, toml::value<std::int64_t>>)
            {
                out = std::to_string(value.get());
                found = true;
            }
            else if constexpr (std::is_same_v<Value, toml::value<double>>)
            {
                out = std::to_string(value.get());
                found = true;
            }
            else if constexpr (std::is_same_v<Value, toml::value<bool>>)
            {
                out = value.get() ? std::string("true") : std::string("false");
                found = true;
            }
        });
    return found;
}

} // namespace resonate::detail

#endif /* RESONATE_RUNTIME_TOML_TEXT_H */
