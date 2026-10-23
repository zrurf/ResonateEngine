-- The engine's own forward renderer: the extract system that reads the frame's
-- scene from the world, the graph it declares, and the executor that runs it
-- against whatever device capability the run carries.
--
-- It sits beside the hierarchy: an engine-side system package over the ECS and
-- the render domain's interfaces, registered through the startup composition —
-- not a plugin. The device itself stays a plugin behind the capability.

target("Resonate.Renderer")
    set_kind("static")
    add_includedirs("include", {public = true})
    add_files("src/**.cpp")

    -- The components are schema-declared: build/gen has to hold their generated
    -- header before this target compiles. The rule's hook fires only for the
    -- requested target, so the idempotent script runs here too.
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

    -- The cook tool runs itself after it links; the renderer needs the cooked
    -- textures at runtime, not to compile, so this is an order-only dep.
    add_deps("Resonate.Render", "Resonate.Render.Graph", "Resonate.ECS",
             "Resonate.Hierarchy", "Resonate.UI", "Resonate.Core.Math")
    if not get_config("engine_only") then
        add_deps("Resonate.Tools.TexCook", {links = false, inherit = false})
    end
    add_packages("basisu", {public = true})

    -- The UI text overlay is drawn by tgfx through the shared Vulkan device.
    -- tgfx is an external CMake build; Android has no tgfx target yet and keeps
    -- the atlas path.
    if is_plat("windows") then
        local tgfx_mode = is_mode("release") and "release" or "debug"
        add_includedirs(path.join(os.projectdir(), "build", "third_party", "tgfx", "include"))
        add_linkdirs(path.join(os.projectdir(), "build", "tgfx", tgfx_mode))
        add_links("tgfx")
        add_syslinks("bcrypt", "ws2_32", "dwrite", "windowscodecs", "ole32")
        add_defines("RESONATE_WITH_TGFX=1")
        before_build(function (target)
            local library = path.join(os.projectdir(), "build", "tgfx", tgfx_mode, "tgfx.lib")
            if not os.isfile(library) then
                local script = path.join(os.projectdir(), "scripts", "tgfx.lua")
                local output = os.iorunv(os.programfile(), {"lua", script},
                                         {envs = {RESONATE_TGFX_MODE = tgfx_mode}})
                if output ~= nil then
                    print(output)
                end
            end
        end)
    end
