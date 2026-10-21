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

local function publish_is_stale()
    if not os.isfile(ASSEMBLY) then
        return true
    end
    local built_at = os.mtime(ASSEMBLY)
    for _, file in ipairs(os.files(path.join(FIXTURE_DIR, "**"))) do
        if os.mtime(file) >= built_at then
            return true
        end
    end
    return false
end

if publish_is_stale() then
    local publish = {"publish", PROJECT, "-c", "Debug", "-o", OUT_DIR, "--nologo", "-v", "quiet"}
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
