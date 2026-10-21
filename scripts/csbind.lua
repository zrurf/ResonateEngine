-- Generates the stable binding ids for both sides from content/csharp/bindings.json:
-- build/gen/csharp/resonate/csharp/bindings.h (engine) and
-- build/gen/csharp/Resonate.CSharp.Bindings.g.cs (managed).
-- Called by scripts/csharp_test.lua before the managed fixture publishes;
-- runnable by hand with `xmake lua scripts/csbind.lua`.

local PROJECT_DIR = os.projectdir()
local TOOL_DIR = path.join(PROJECT_DIR, "packages", "tools", "csbind")
local TOOL_PROJECT = path.join(TOOL_DIR, "Resonate.Tools.CsBind.csproj")
local BUILD_DIR = path.join(PROJECT_DIR, "build")
local TOOL_OUT = path.join(BUILD_DIR, "tools", "csbind", "publish")
local TOOL_BIN = path.join(TOOL_OUT, is_host("windows") and "csbind.exe" or "csbind")
local INPUT = "content/csharp/bindings.json"
local GEN_DIR = path.join(BUILD_DIR, "gen", "csharp")

local function tool_is_stale()
    if not os.isfile(TOOL_BIN) then
        return true
    end
    local built_at = os.mtime(TOOL_BIN)
    for _, file in ipairs(os.files(path.join(TOOL_DIR, "**"))) do
        if os.mtime(file) >= built_at then
            return true
        end
    end
    return false
end

if tool_is_stale() then
    local publish = {"publish", TOOL_PROJECT, "-c", "Release", "-o", TOOL_OUT, "--nologo", "-v",
                     "quiet"}
    local code = os.execv("dotnet", publish, {try = true})
    if code ~= 0 then
        -- Two builds can publish the same output at once; a retry finds the
        -- winner's binary in place.
        os.sleep(1000)
        code = os.execv("dotnet", publish, {try = true})
    end
    if code ~= 0 and not os.isfile(TOOL_BIN) then
        raise("csbind: building the generator failed (dotnet publish exited " .. tostring(code) ..
                  "); the .NET SDK is required")
    end
end

os.mkdir(GEN_DIR)
-- The input is relative so the generated comment names the same file on every
-- machine; the tool runs from the project root for it.
local code = os.execv(TOOL_BIN, {"--input", INPUT, "--out", GEN_DIR},
                      {try = true, curdir = PROJECT_DIR})
if code ~= 0 then
    raise("csbind: generating the binding ids failed (exit " .. tostring(code) .. ")")
end
