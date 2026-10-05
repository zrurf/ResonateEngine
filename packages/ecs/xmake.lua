-- The ECS: archetype storage, the authoritative game state. Depends on core
-- only, so the storage is usable (and testable) without the module host or the
-- frame loop.

target("Resonate.ECS")
    set_kind("static")
    add_includedirs("include", {public = true})
    add_files("src/**.cpp")

    add_deps("Resonate.Core.Memory", "Resonate.Core.Container",
             "Resonate.Core.Job")

    -- XXH3 derives the RNG streams' keys from their names, the way the module
    -- ABI hashes capability ids.
    add_packages("xxhash")
