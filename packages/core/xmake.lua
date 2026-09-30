-- Split by capability rather than by file type, so a package that needs math
-- does not link a container allocator it never calls.

target("ResonateEngine.Core.Memory")
    set_kind("static")
    add_includedirs("include", {public = true})
    add_files("src/memory/**.cpp")
    add_deps("ResonateEngine.PAL")

target("ResonateEngine.Core.Container")
    set_kind("static")
    add_includedirs("include", {public = true})
    add_files("src/container/**.cpp")
    add_deps("ResonateEngine.Core.Memory")

target("ResonateEngine.Core.Math")
    set_kind("static")
    add_includedirs("include", {public = true})
    add_files("src/math/**.cpp")

target("ResonateEngine.Core.Job")
    set_kind("static")
    add_includedirs("include", {public = true})
    add_files("src/job/**.cpp")
    add_deps("ResonateEngine.PAL", "ResonateEngine.Core.Memory")
