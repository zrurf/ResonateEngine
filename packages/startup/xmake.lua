-- The startup the launcher and the editor share. It sits above the runtime and the
-- window package because it is the only layer that knows about both: the runtime
-- keeps its frame loop free of displays, and the window package keeps the display
-- free of the frame loop.

target("ResonateEngine.Startup")
    set_kind("static")
    add_includedirs("include", {public = true})
    add_files("src/**.cpp")

    add_deps("ResonateEngine.Runtime", "ResonateEngine.Window", "ResonateEngine.Render",
             "ResonateEngine.Hierarchy")

    -- tree.h includes the generated hierarchy components, so the schema output
    -- has to exist before this target compiles.
    add_includedirs("$(builddir)/gen")
    on_load(function (target)
        os.mkdir(path.join(os.projectdir(), "build", "gen"))
    end)

    -- The generated header must exist before this target compiles. The
    -- resonate.schema rule's hook fires only for the requested target, so this
    -- library — usually built as somebody's dependency — runs the script
    -- itself; the script is idempotent, so a second run in one build is just a
    -- freshness check.
    before_build(function (target)
        local script = path.join(os.projectdir(), "scripts", "schema.lua")
        local output = os.iorunv(os.programfile(), {"lua", script})
        if output ~= nil then
            print((output:gsub("%\27%[[%d;]*m", ""):gsub("%s*%{%s*%}%s*$", ""):gsub("\n+$", "")))
        end
    end)
