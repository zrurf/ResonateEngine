-- The UI domain's interface: what an immediate-mode context exposes, so a plugin
-- can implement it and a consumer can reach it without linking an implementation.
-- The default implementation is packages/modules/ui.

target("Resonate.UI.Abi")
    set_kind("headeronly")
    add_includedirs("include", {public = true})

    add_deps("Resonate.Module.Abi")

-- The widget runtime behind the immediate-mode interface: the schema-declared
-- widget components, the retained layout pass and, later, the document and
-- hit-test layers. A module implements Resonate.UI.Context over this; a game
-- can link it directly.
target("Resonate.UI")
    set_kind("static")
    add_includedirs("include", {public = true})
    add_files("src/**.cpp")

    add_deps("Resonate.ECS", "Resonate.Hierarchy", "Resonate.Document", "Resonate.Text")

    -- The widget components are schema-declared in content/ui. This library is
    -- usually reached as a dependency, so its own hook runs the compiler; the
    -- script is idempotent and a second run in one build is a freshness check.
    add_includedirs(path.join(os.projectdir(), "build", "gen"))
    on_load(function (target)
        os.mkdir(path.join(os.projectdir(), "build", "gen"))
    end)

    before_build(function (target)
        local script = path.join(os.projectdir(), "scripts", "schema.lua")
        local output = os.iorunv(os.programfile(), {"lua", script})
        if output ~= nil then
            print((output:gsub("%\27%[[%d;]*m", ""):gsub("%s*%{%s*%}%s*$", ""):gsub("\n+$", "")))
        end
    end)
