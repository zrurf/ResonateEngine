target("ResonateEngine.Module.Abi")
    set_kind("headeronly")
    add_includedirs("include", {public = true})

    add_packages("xxhash", {public = true})

-- The host implementation is a separate target so that host.hpp stays off a
-- plugin's include path: a module gets the ABI and the authoring layer, never
-- the loader.
target("ResonateEngine.Module")
    set_kind("static")
    add_includedirs("host", {public = true})
    add_files("src/**.cpp")

    add_deps("ResonateEngine.Module.Abi", "ResonateEngine.PAL", "ResonateEngine.Core.Memory",
             "ResonateEngine.Core.Container", "ResonateEngine.Core.Job", "ResonateEngine.ECS")

    -- Manifest parsing, host side only: a plugin never sees this.
    add_packages("yyjson")
