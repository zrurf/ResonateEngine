-- Hosts CoreCLR behind a small C++ facade. It links nothing from the .NET
-- SDK: hostfxr is located in the installed runtime at run time, so the engine's
-- build does not depend on a particular SDK layout or version.
target("Resonate.CSharp")
    set_kind("static")
    add_includedirs("include", {public = true})
    add_files("src/**.cpp")
    add_deps("Resonate.PAL")
