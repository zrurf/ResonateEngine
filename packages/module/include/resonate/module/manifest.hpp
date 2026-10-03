#ifndef RESONATE_MODULE_MANIFEST_HPP
#define RESONATE_MODULE_MANIFEST_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "resonate/module/capability_id.hpp"

namespace resonate
{

/*
 * A capability a manifest names. The id is what resolves; the name is carried
 * beside it because a diagnostic that reports only the hash tells a module author
 * nothing about which capability is missing.
 */
struct ModuleCapability
{
    std::string name;
    Id id;
};

/*
 * What one module declares about itself. resonate.module.toml states the same
 * thing for the build and for tooling, and scripts/module_check.lua fails the
 * build when the two disagree.
 *
 * Capabilities are listed as typed ids, so a renamed or deleted capability fails
 * to compile.
 */
struct ModuleManifest
{
    /* Reverse-DNS and globally unique: "resonate.physics". Identity everywhere. */
    std::string id;

    /* Display name for logs and editor lists: "Physics". Carries no identity. */
    std::string name;

    std::string version;

    /* Major ABI version the module was built against. */
    std::uint32_t abi = 1;

    /* Lowest host major ABI this module accepts. */
    std::uint32_t min_host_abi = 1;

    /* Capabilities this module publishes; a capability belongs to exactly one
       module. */
    std::vector<ModuleCapability> provides;

    /* Capabilities the host must resolve before on_attach runs. */
    std::vector<ModuleCapability> requirements;

    /* Capabilities that may be absent; the consumer checks the handle it gets back. */
    std::vector<ModuleCapability> optional;

    /* Modules that must be attached first, independent of capabilities. */
    std::vector<std::string> depends_on;
};

} // namespace resonate

#endif /* RESONATE_MODULE_MANIFEST_HPP */
