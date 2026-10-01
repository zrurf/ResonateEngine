-- Tests link the same static libraries the engine ships, so a test that compiles
-- also tests the visibility rules. Capability headers from module targets are
-- included directly: their descriptors are exercised without linking the plugin,
-- which is a shared library.

-- A module built from the test tree rather than from packages/modules, so the
-- suite covers what a plugin can actually link. It depends on the ABI target
-- alone: a dependency on ResonateEngine.Module would hide the very mistake it
-- exists to catch, since the host library is where those symbols live.
--
-- It sits under plugins/ rather than module/ so the test target's own glob does
-- not compile it a second time, into the test binary.
target("ResonateEngine.Tests.ProbeModule")
    set_kind("shared")
    set_default(false)
    add_files("plugins/probe/probe_module.cpp")
    add_deps("ResonateEngine.Module.Abi")

    -- Its own directory, so discovering it does not change what the four shipped
    -- modules look like to the tests that count them.
    set_targetdir("$(builddir)/probe-plugins", {bindir = ".", libdir = "."})

    after_build(function (target)
        local manifest =
            path.join(os.projectdir(), "tests", "plugins", "probe", "resonate.probe.json")
        os.cp(manifest, path.join(target:targetdir(), "resonate.probe.json"))
    end)

-- A module that fails its attach with host resources held, so the withdrawal the
-- host promises for that case is something a test can observe.
target("ResonateEngine.Tests.FailingModule")
    set_kind("shared")
    set_default(false)
    add_files("plugins/failing/failing_module.cpp")
    add_deps("ResonateEngine.Module.Abi")

    set_targetdir("$(builddir)/failing-plugins", {bindir = ".", libdir = "."})

    after_build(function (target)
        local manifest =
            path.join(os.projectdir(), "tests", "plugins", "failing", "resonate.failing.json")
        os.cp(manifest, path.join(target:targetdir(), "resonate.failing.json"))
    end)

target("ResonateEngine.Tests")
    set_kind("binary")
    set_default(false)
    set_group("tests")

    add_files("main.cpp", "render/**.cpp", "core/**.cpp", "module/**.cpp", "pal/**.cpp",
              "runtime/**.cpp", "window/**.cpp")
    add_tests("default")

    -- For paths.h, which every test that needs a build artifact includes.
    add_includedirs(".")

    -- The libraries the tests link and call into, plus the two interfaces whose
    -- headers a test includes directly: a capability's declaration belongs to the
    -- package that owns the domain, not to the plugin that implements it.
    add_deps("ResonateEngine.Module", "ResonateEngine.Window", "ResonateEngine.Render",
             "ResonateEngine.UI.Abi", "ResonateEngine.PAL", "ResonateEngine.Runtime",
             "ResonateEngine.Core.Container", "ResonateEngine.Core.Math", "ResonateEngine.Core.Job")

    -- The plugins, order-only: their directory is what the end-to-end tests load,
    -- and a `xmake test` that did not build them once reported a green suite whose
    -- only end-to-end assertions ran nothing. Order-only, so the test binary does
    -- not carry imports of plugin libraries.
    add_deps("ResonateEngine.Tests.ProbeModule", {links = false, inherit = false})
    add_deps("ResonateEngine.Tests.FailingModule", {links = false, inherit = false})
    for _, module in ipairs({"audio", "physics", "render", "ui"}) do
        add_deps("Resonate.Module.resonate." .. module, {links = false, inherit = false})
    end

    -- SDL is private to the window target, so the test that drives the window
    -- capability through SDL's dummy video driver asks for it itself.
    add_packages("libsdl3")
    add_packages("catch2")
