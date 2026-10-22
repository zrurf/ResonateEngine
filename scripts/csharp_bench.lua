-- Publishes the managed FFI-bench assembly in Release and ReadyToRun: the
-- budget compares optimized native code, so a debug publish would measure the
-- wrong thing. Called by the bench target's before_build and runnable by hand
-- with `xmake lua scripts/csharp_bench.lua`.

local PROJECT_DIR = os.projectdir()
local SCRIPT = path.join(PROJECT_DIR, "scripts", "csharp_bench.lua")
local PROJECT = path.join(PROJECT_DIR, "tests", "csharp", "bench",
                          "Resonate.Tests.Bench.csproj")
local PROJECT_DIR_SRC = path.join(PROJECT_DIR, "tests", "csharp", "bench")
local OUT_DIR = path.join(PROJECT_DIR, "build", "tools", "csharp-bench")
local ASSEMBLY = path.join(OUT_DIR, "Resonate.Tests.Bench.dll")
local GENERATED_DIR = path.join(PROJECT_DIR, "build", "gen", "csharp")
local TOOLS_DIR = path.join(PROJECT_DIR, "packages", "tools", "analyzers")

-- The bench compiles the generated boundary and view code, so both exist
-- before the publish.
local schema = os.execv(os.programfile(),
                        {"lua", path.join(PROJECT_DIR, "scripts", "schema.lua")}, {try = true})
if schema ~= 0 then
    raise("csharp bench: schema generation failed")
end
local bindings = os.execv(os.programfile(),
                          {"lua", path.join(PROJECT_DIR, "scripts", "csbind.lua")}, {try = true})
if bindings ~= 0 then
    raise("csharp bench: binding id generation failed")
end

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

local function stale()
    if not os.isfile(ASSEMBLY) then
        return true
    end
    -- MSBuild skips copying an unchanged assembly, so the stamp records that
    -- this input set was published.
    local stamp = ASSEMBLY .. ".stamp"
    local built_at = os.isfile(stamp) and os.mtime(stamp) or os.mtime(ASSEMBLY)
    if os.mtime(SCRIPT) >= built_at then
        return true
    end
    for _, file in ipairs(os.files(path.join(PROJECT_DIR_SRC, "**"))) do
        if os.mtime(file) >= built_at then
            return true
        end
    end
    for _, file in ipairs(os.files(path.join(GENERATED_DIR, "**", "*.g.cs"))) do
        if os.mtime(file) >= built_at then
            return true
        end
    end
    for _, file in ipairs(os.files(path.join(TOOLS_DIR, "**"))) do
        if os.mtime(file) >= built_at then
            return true
        end
    end
    return false
end

if stale() then
    local publish = {"publish", PROJECT, "-c", "Release", "-o", OUT_DIR, "--nologo", "-v",
                     "quiet", "-r", runtime_identifier(), "--self-contained", "false",
                     "-p:PublishReadyToRun=true"}
    local code = os.execv("dotnet", publish, {try = true})
    if code ~= 0 then
        os.sleep(1000)
        code = os.execv("dotnet", publish, {try = true})
    end
    if code == 0 then
        io.writefile(ASSEMBLY .. ".stamp", tostring(os.time()))
    end
    if stale() then
        raise("csharp bench: publishing the bench assembly failed (dotnet publish exited " ..
                  tostring(code) .. "); the .NET SDK is required")
    end
end
