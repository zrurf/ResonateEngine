-- Hosts CoreCLR behind a small C++ facade. It links nothing from the .NET
-- SDK: hostfxr is located in the installed runtime at run time, so the engine's
-- build does not depend on a particular SDK layout or version.
--
-- The managed ids and boundary structs come from content/csharp/bindings.json;
-- csbind generates them before this target compiles, because ManagedSystems
-- names the generated SystemContext and the tick id.

rule("resonate.csbind")
    before_build(function (target)
        if csbind_done then
            return
        end
        csbind_done = true
        local script = path.join(os.projectdir(), "scripts", "csbind.lua")
        local output = os.iorunv(os.programfile(), {"lua", script})
        if output ~= nil then
            print(output)
        end
    end)

target("Resonate.CSharp")
    set_kind("static")
    add_includedirs("include", {public = true})
    add_includedirs(path.join(os.projectdir(), "build", "gen", "csharp"), {public = true})
    add_files("src/**.cpp")
    add_deps("Resonate.ECS", "Resonate.PAL")
    add_rules("resonate.csbind")
    on_load(function (target)
        os.mkdir(path.join(os.projectdir(), "build", "gen", "csharp"))
    end)
