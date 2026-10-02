-- The ECS: archetype storage, the authoritative game state. Depends on core
-- only, so the storage is usable (and testable) without the module host or the
-- frame loop.

target("ResonateEngine.ECS")
    set_kind("static")
    add_includedirs("include", {public = true})
    add_files("src/**.cpp")

    add_deps("ResonateEngine.Core.Memory", "ResonateEngine.Core.Container",
             "ResonateEngine.Core.Job")
