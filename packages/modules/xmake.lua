-- Module targets are declared from this table rather than read from the
-- manifests: a script pulled in by includes() runs in a sandbox with no file
-- reading, so it cannot open resonate.module.toml. scripts/module_check.lua
-- reconciles the table against the manifests, the C descriptors and the
-- capability headers, and fails the build on any disagreement.
--
-- Keep each entry on one line: the checker parses this block directly.
local modules = {
    {dir = "audio",   id = "resonate.audio",   deps = {}},
    {dir = "physics", id = "resonate.physics", deps = {}},
    {dir = "render",  id = "resonate.render",  deps = {}},
    {dir = "ui",      id = "resonate.ui",      deps = {"resonate.render"}},
}

-- Windows resolves a shared library from the executable's directory, macOS and
-- Linux from libdir, so both are forced flat. Left alone, plugins would land in
-- a different directory per platform and the loader would search for one of
-- them.
local plugin_dir = "$(builddir)/plugins"

local target_of = {}
for _, module in ipairs(modules) do
    target_of[module.id] = "Resonate.Module." .. module.id
end

for _, module in ipairs(modules) do
    -- Every domain's interface, header-only: the capability a module implements and
    -- the ones it may consume live in their own packages, so this list is what a
    -- plugin links instead of another plugin. Having all of them available costs
    -- nothing, and a module that starts requiring another capability needs no
    -- build change. `deps` in the table above stays module-to-module, which is
    -- what the checker compares against each manifest's depends_on.
    local deps = {"ResonateEngine.Module.Abi", "ResonateEngine.Render.Abi",
                  "ResonateEngine.Window.Abi", "ResonateEngine.Physics.Abi",
                  "ResonateEngine.Audio.Abi", "ResonateEngine.UI.Abi"}
    for _, dependency in ipairs(module.deps) do
        local dependency_target = target_of[dependency]
        if dependency_target == nil then
            raise("module '%s' depends on '%s', which is not a known module", module.id, dependency)
        end
        deps[#deps + 1] = dependency_target
    end

    -- Paths stay relative: they resolve against this script's directory, which
    -- is where the target is declared. $(scriptdir) expands to the project root
    -- here, not to this file.
    local dir = module.dir

    local module_id = module.id
    local module_dir = module.dir

    target(target_of[module_id])
        set_kind("shared")
        add_files(path.join(dir, "src/**.cpp"))
        add_deps(deps)
        set_targetdir(plugin_dir, {bindir = ".", libdir = "."})

        -- The loader pairs a library with the manifest of the same id beside it.
        -- The callback resolves its own paths: a relative one would be read from
        -- wherever the build happens to run.
        after_build(function (target)
            local source = path.join(os.projectdir(), "packages", "modules", module_dir, "resonate.module.toml")
            os.cp(source, path.join(target:targetdir(), module_id .. ".toml"))
        end)
end
