-- Runs the schema compiler over the content tree, writing build/gen. Called by
-- the `resonate.schema` rule before a target that consumes generated types
-- builds; runnable by hand with `xmake schema`.
--
-- The compiler is a C# CLI (packages/tools/schemac), so this script is also
-- what keeps dotnet out of the rule bodies: the SDK is needed only where
-- generated types are actually consumed, and the publishing step below rebuilds
-- the tool only when one of its sources moved.

local PROJECT_DIR = os.projectdir()
local TOOL_DIR = path.join(PROJECT_DIR, "packages", "tools")
local TOOL_PROJECT = path.join(TOOL_DIR, "schemac", "cli", "Schemac.Cli.csproj")
-- xmake's default builddir; tests/paths.h finds the same directory by name.
local BUILD_DIR = path.join(PROJECT_DIR, "build")
local TOOL_OUT = path.join(BUILD_DIR, "tools", "schemac", "publish")
local TOOL_BIN = path.join(TOOL_OUT, is_host("windows") and "schemac.exe" or "schemac")
local GEN_DIR = path.join(BUILD_DIR, "gen")
local CONTENT_DIR = path.join(PROJECT_DIR, "content")
-- The shape contract an editor reads is checked here too, so a declaration the
-- compiler accepts and the contract does not know about fails the build instead
-- of drifting apart from what an editor shows.
local SHAPE_CONTRACT = path.join(PROJECT_DIR, "schema", "formats", "schema", "rschema.xsd")

-- An equal mtime counts as stale, so an edit never loses to a same-second
-- touch; publishing a C# project that did not change is cheap anyway.
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
    local code = os.execv("dotnet", {"publish", TOOL_PROJECT, "-c", "Release", "-o", TOOL_OUT,
                                     "--nologo", "-v", "quiet"}, {try = true})
    if code ~= 0 then
        raise("schemac: building the compiler failed (dotnet publish exited %s); the .NET SDK "
                  .. "is required wherever generated schema types are consumed", tostring(code))
    end
end

-- The mode comes from whichever task started this script: `xmake schema-lock`
-- sets RESONATE_SCHEMA_MODE=update, a build (and plain `xmake schema`) checks.
local MODE = os.getenv("RESONATE_SCHEMA_MODE") or "check"

local code = os.execv(TOOL_BIN, {"--root", CONTENT_DIR, "--out", GEN_DIR, "--xsd", SHAPE_CONTRACT,
                                 MODE == "update" and "--update-lock" or "--check-lock"}, {try = true})
if code ~= 0 then
    raise("schemac: schema compilation failed (exit %s)", tostring(code))
end
