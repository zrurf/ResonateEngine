#ifndef RESONATE_MODULE_DESCRIPTOR_HPP
#define RESONATE_MODULE_DESCRIPTOR_HPP

#include <string>
#include <vector>

#include "resonate/module/capability_id.hpp"
#include "resonate/module/manifest.hpp"
#include "resonate/module/module.hpp"

namespace resonate
{

/* Defined once per module by RESONATE_MODULE_DESCRIPTOR; read by the host after
   it loads the library. */
ModuleManifest& moduleManifest();

namespace detail
{

struct CapabilityList
{
    std::vector<ModuleCapability> capabilities;
};

struct ModuleNameList
{
    std::vector<std::string> names;
};

template <typename... Capabilities> CapabilityList capabilities()
{
    CapabilityList list;
    list.capabilities.reserve(sizeof...(Capabilities));
    (list.capabilities.push_back(ModuleCapability{CapabilityTraits<Capabilities>::name,
                                                  CapabilityTraits<Capabilities>::id}),
     ...);
    return list;
}

template <typename... Names> ModuleNameList moduleNames(Names... names)
{
    ModuleNameList list;
    list.names.reserve(sizeof...(names));
    (list.names.emplace_back(names), ...);
    return list;
}

} // namespace detail
} // namespace resonate

/* Parenthesised forms of the helpers above, for use inside the descriptor
   macro. */
#define RESONATE_CAPABILITIES(...) ::resonate::detail::capabilities<__VA_ARGS__>()
#define RESONATE_MODULE_NAMES(...) ::resonate::detail::moduleNames(__VA_ARGS__)

/*
 * The runtime half of a module's manifest, checked against resonate.module.json
 * on every build by scripts/module_check.lua.
 */
#define RESONATE_MODULE_DESCRIPTOR(module_id, module_name, module_version, module_abi, provides_,  \
                                   requirements_, optional_, dependencies_)                        \
    namespace resonate                                                                             \
    {                                                                                              \
    ModuleManifest makeModuleManifest()                                                            \
    {                                                                                              \
        ModuleManifest manifest;                                                                   \
        manifest.id = module_id;                                                                   \
        manifest.name = module_name;                                                               \
        manifest.version = module_version;                                                         \
        manifest.abi = module_abi;                                                                 \
        manifest.min_host_abi = module_abi;                                                        \
        manifest.provides = provides_.capabilities;                                                \
        manifest.requirements = requirements_.capabilities;                                        \
        manifest.optional = optional_.capabilities;                                                \
        manifest.depends_on = dependencies_.names;                                                 \
        return manifest;                                                                           \
    }                                                                                              \
                                                                                                   \
    ModuleManifest& moduleManifest()                                                               \
    {                                                                                              \
        static ModuleManifest manifest = makeModuleManifest();                                     \
        return manifest;                                                                           \
    }                                                                                              \
    }

#endif /* RESONATE_MODULE_DESCRIPTOR_HPP */
