-- Split by capability rather than by file type, so a package that needs math
-- does not link a container allocator it never calls.

target("Resonate.Core.Memory")
    set_kind("static")
    add_includedirs("include", {public = true})
    add_files("src/memory/**.cpp")
    add_deps("Resonate.PAL")

target("Resonate.Core.Container")
    set_kind("static")
    add_includedirs("include", {public = true})
    add_files("src/container/**.cpp")
    add_deps("Resonate.Core.Memory")

target("Resonate.Core.Math")
    set_kind("static")
    add_includedirs("include", {public = true})
    add_files("src/math/**.cpp")

target("Resonate.Core.Job")
    set_kind("static")
    add_includedirs("include", {public = true})
    add_files("src/job/**.cpp")
    add_deps("Resonate.PAL", "Resonate.Core.Memory")
