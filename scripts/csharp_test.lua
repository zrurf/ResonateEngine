-- Publishes the managed assets the C# host tests load: the fixture, the patch
-- host bootstrap, and a patch assembly. Called from the test target's
-- before_build (see the resonate.csharp_test rule) and runnable by hand with
-- `xmake lua scripts/csharp_test.lua`.
--
-- The fixture is a component, not a tool: `EnableDynamicLoading` makes the
-- publish drop the runtimeconfig.json hostfxr reads. The bootstrap and the
-- patch are plain libraries with no dependencies outside the framework.

local PROJECT_DIR = os.projectdir()
local SCRIPT = path.join(PROJECT_DIR, "scripts", "csharp_test.lua")
local GENERATED_DIR = path.join(PROJECT_DIR, "build", "gen", "csharp")
-- The analyzers and the registration generator are compiled into every
-- behaviour assembly, so their sources are part of these publishes too.
local TOOLS_DIR = path.join(PROJECT_DIR, "packages", "tools", "analyzers")

-- The assets compile the generated ids and the schema's component views, so
-- both exist before the publishes. The schema script runs here too, whatever
-- order xmake runs the rule hooks in.
local schema = os.execv(os.programfile(),
                        {"lua", path.join(PROJECT_DIR, "scripts", "schema.lua")}, {try = true})
if schema ~= 0 then
    raise("csharp test: schema generation failed")
end
local generated = os.execv(os.programfile(), {"lua", path.join(PROJECT_DIR, "scripts", "csbind.lua")},
                           {try = true})
if generated ~= 0 then
    raise("csharp test: generating the binding ids failed")
end

-- The design's base is ReadyToRun: native code shipped in the assembly, the
-- full runtime present, so a patch can still JIT over it.
local function runtime_identifier()
    local arch = os.arch()
    if os.host() == "windows" then
        return "win-" .. arch
    end
    if os.host() == "macos" then
        return "osx-" .. arch
    end
    return "linux-" .. arch
end

local function generated_is_newer(built_at)
    for _, file in ipairs(os.files(path.join(GENERATED_DIR, "**", "*.g.cs"))) do
        if os.mtime(file) >= built_at then
            return true
        end
    end
    return false
end

-- The publish can legitimately leave the assembly untouched (MSBuild skips a
-- copy when the content is unchanged), so the assembly's mtime cannot record
-- that this exact input set was published. The stamp is written after every
-- successful publish and is what the comparisons use.
local function published_at(output)
    local stamp = output .. ".stamp"
    if os.isfile(stamp) then
        return os.mtime(stamp)
    end
    return os.mtime(output)
end

local function publish_is_stale(project_dir, output, uses_generated)
    if not os.isfile(output) then
        return true
    end
    local built_at = published_at(output)
    -- The command line is part of the output: a change to it (say, adding
    -- ReadyToRun) must republish even when the sources did not move.
    if os.mtime(SCRIPT) >= built_at then
        return true
    end
    if uses_generated and generated_is_newer(built_at) then
        return true
    end
    for _, file in ipairs(os.files(path.join(TOOLS_DIR, "**"))) do
        if os.mtime(file) >= built_at then
            return true
        end
    end
    for _, file in ipairs(os.files(path.join(project_dir, "**"))) do
        if os.mtime(file) >= built_at then
            return true
        end
    end
    return false
end

local function publish(name, project, project_dir, out_dir, output, ready_to_run, uses_generated)
    if not publish_is_stale(project_dir, output, uses_generated) then
        return
    end
    local args = {"publish", project, "-c", "Debug", "-o", out_dir, "--nologo", "-v", "quiet"}
    if ready_to_run then
        for _, extra in ipairs({"-r", runtime_identifier(), "--self-contained", "false",
                                "-p:PublishReadyToRun=true"}) do
            table.insert(args, extra)
        end
    end
    local code = os.execv("dotnet", args, {try = true})
    if code ~= 0 then
        -- Two builds can publish the same output at once; a retry finds the
        -- winner's assembly in place.
        os.sleep(1000)
        code = os.execv("dotnet", args, {try = true})
    end
    if code == 0 then
        os.touch(output .. ".stamp")
    end
    -- A failed publish must fail the build even when an older assembly is
    -- still on disk: the silent fallback once hid a compile error for a whole
    -- test run. A concurrent publish that won the race has a fresh stamp,
    -- so the staleness check is what decides.
    if publish_is_stale(project_dir, output, uses_generated) then
        raise("csharp test: publishing " .. name .. " failed (dotnet publish exited " ..
                  tostring(code) .. "); the .NET SDK is required for the C# host tests")
    end
end

publish("the fixture", path.join(PROJECT_DIR, "tests", "csharp", "managed",
                                 "Resonate.Tests.Managed.csproj"),
        path.join(PROJECT_DIR, "tests", "csharp", "managed"),
        path.join(PROJECT_DIR, "build", "tools", "csharp-test"),
        path.join(PROJECT_DIR, "build", "tools", "csharp-test", "Resonate.Tests.Managed.dll"),
        true, true)

publish("the patch host", path.join(PROJECT_DIR, "tests", "csharp", "bootstrap",
                                    "Resonate.Tests.Bootstrap.csproj"),
        path.join(PROJECT_DIR, "tests", "csharp", "bootstrap"),
        path.join(PROJECT_DIR, "build", "tools", "csharp-bootstrap"),
        path.join(PROJECT_DIR, "build", "tools", "csharp-bootstrap",
                  "Resonate.Tests.Bootstrap.dll"),
        false, false)

publish("the patch", path.join(PROJECT_DIR, "tests", "csharp", "patch",
                               "Resonate.Tests.Patch.csproj"),
        path.join(PROJECT_DIR, "tests", "csharp", "patch"),
        path.join(PROJECT_DIR, "build", "tools", "csharp-patch"),
        path.join(PROJECT_DIR, "build", "tools", "csharp-patch", "Resonate.Tests.Patch.dll"),
        false, true)

-- The laws are build-time contracts; their checks belong to the suite.
local analyzers = path.join(PROJECT_DIR, "scripts", "analyzers.lua")
local checked = os.execv(os.programfile(), {"lua", analyzers}, {try = true})
if checked ~= 0 then
    raise("csharp test: the behaviour analyzer self-check failed")
end
