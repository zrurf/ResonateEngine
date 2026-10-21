-- Publishes the sample managed game the launcher can run. The demo is content,
-- not engine code: `xmake csharp-demo` builds it into build/content/csharp/demo,
-- and a run points [resonate.csharp] at the assembly.

local PROJECT_DIR = os.projectdir()
local PROJECT = path.join(PROJECT_DIR, "content", "csharp", "demo", "Resonate.Demo.Game.csproj")
local OUT_DIR = path.join(PROJECT_DIR, "build", "content", "csharp", "demo")
local ASSEMBLY = path.join(OUT_DIR, "Resonate.Demo.Game.dll")
local GENERATED_DIR = path.join(PROJECT_DIR, "build", "gen", "csharp")

-- The generated bindings and views are compiled in, so they exist first.
local schema = os.execv(os.programfile(),
                        {"lua", path.join(PROJECT_DIR, "scripts", "schema.lua")}, {try = true})
if schema ~= 0 then
    raise("csharp demo: schema generation failed")
end
local bindings = os.execv(os.programfile(),
                          {"lua", path.join(PROJECT_DIR, "scripts", "csbind.lua")}, {try = true})
if bindings ~= 0 then
    raise("csharp demo: binding id generation failed")
end

local function stale()
    if not os.isfile(ASSEMBLY) then
        return true
    end
    local built_at = os.mtime(ASSEMBLY)
    for _, file in ipairs(os.files(path.join(PROJECT_DIR, "content", "csharp", "demo", "**"))) do
        if os.mtime(file) >= built_at then
            return true
        end
    end
    for _, file in ipairs(os.files(path.join(GENERATED_DIR, "**", "*.g.cs"))) do
        if os.mtime(file) >= built_at then
            return true
        end
    end
    return false
end

if stale() then
    local publish = {"publish", PROJECT, "-c", "Debug", "-o", OUT_DIR, "--nologo", "-v", "quiet"}
    local code = os.execv("dotnet", publish, {try = true})
    if code ~= 0 then
        os.sleep(1000)
        code = os.execv("dotnet", publish, {try = true})
    end
    if stale() then
        raise("csharp demo: publishing the sample game failed (dotnet publish exited " ..
                  tostring(code) .. ")")
    end
end
