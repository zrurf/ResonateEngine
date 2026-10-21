-- Builds and runs the Roslyn analyzers' self-checks. The behaviour laws are
-- build-time contracts, so the analyzers carry their own program with a
-- non-zero exit; the tests target's before_build calls this, and
-- `xmake analyzers` runs it by hand.

local PROJECT_DIR = os.projectdir()
local PROJECT = path.join(PROJECT_DIR, "tests", "csharp", "analyzers",
                          "Resonate.Analyzers.Tests.csproj")

local code = os.execv("dotnet", {"run", "--project", PROJECT, "-c", "Debug", "--nologo", "-v",
                                 "quiet"}, {try = true})
if code ~= 0 then
    raise("analyzers: the self-check failed (exit " .. tostring(code) ..
              "); the .NET SDK is required")
end
