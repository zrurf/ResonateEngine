-- Publishes the managed fixture the C# host test loads. Called from the test
-- target's before_build (see the resonate.csharp_test rule) and runnable by
-- hand with `xmake lua scripts/csharp_test.lua`.
--
-- The fixture is a component, not a tool: `EnableDynamicLoading` makes the
-- publish drop the runtimeconfig.json hostfxr reads.

local PROJECT_DIR = os.projectdir()
local FIXTURE_DIR = path.join(PROJECT_DIR, "tests", "csharp", "managed")
local PROJECT = path.join(FIXTURE_DIR, "Resonate.Tests.Managed.csproj")
local OUT_DIR = path.join(PROJECT_DIR, "build", "tools", "csharp-test")
local ASSEMBLY = path.join(OUT_DIR, "Resonate.Tests.Managed.dll")
local GENERATED = path.join(PROJECT_DIR, "build", "gen", "csharp",
                            "Resonate.CSharp.Bindings.g.cs")

-- The fixture compiles the generated ids and the schema's component views, so
-- both exist before the publish. The schema script runs here too, whatever
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

local function publish_is_stale()
    if not os.isfile(ASSEMBLY) then
        return true
    end
    local built_at = os.mtime(ASSEMBLY)
    -- The command line below is part of the output: a change to it (say, adding
    -- ReadyToRun) must republish even when the fixture sources did not move.
    if os.mtime(path.join(PROJECT_DIR, "scripts", "csharp_test.lua")) >= built_at then
        return true
    end
    if os.isfile(GENERATED) and os.mtime(GENERATED) >= built_at then
        return true
    end
    for _, file in ipairs(os.files(path.join(FIXTURE_DIR, "**"))) do
        if os.mtime(file) >= built_at then
            return true
        end
    end
    return false
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

if publish_is_stale() then
    local publish = {"publish", PROJECT, "-c", "Debug", "-o", OUT_DIR,
                     "-r", runtime_identifier(), "--self-contained", "false",
                     "-p:PublishReadyToRun=true", "--nologo", "-v", "quiet"}
    local code = os.execv("dotnet", publish, {try = true})
    if code ~= 0 then
        -- Two builds can publish the same output at once; a retry finds the
        -- winner's assembly in place.
        os.sleep(1000)
        code = os.execv("dotnet", publish, {try = true})
    end
    if code ~= 0 and not os.isfile(ASSEMBLY) then
        raise("csharp test: publishing the managed fixture failed (dotnet publish exited " ..
                  tostring(code) .. "); the .NET SDK is required for the C# host tests")
    end
end
