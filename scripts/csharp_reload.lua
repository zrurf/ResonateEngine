-- Prepares the hot-reload acceptance bench. The loop itself is native
-- (tests/bench/csharp_reload.cpp): it rewrites Behaviour.cs, rebuilds the
-- patch assembly, swaps it through the collectible ALC and invokes it, timing
-- the whole path. This script publishes the base fixture and bootstrap the
-- bench loads, and generates the patch project once so the first measured
-- build is a warm incremental one.

local PROJECT_DIR = os.projectdir()
local OUT = path.join(PROJECT_DIR, "build", "tools", "csharp-reload")
local GENERATED = path.join(PROJECT_DIR, "build", "gen", "csharp")

-- The base assembly and the patch bootstrap come from the C# test fixtures.
local code = os.execv(os.programfile(),
                      {"lua", path.join(PROJECT_DIR, "scripts", "csharp_test.lua")},
                      {try = true})
if code ~= 0 then
    raise("csharp reload: publishing the base fixtures failed")
end

os.mkdir(OUT)

local function slash(value)
    return value:gsub("\\", "/")
end

-- Absolute paths: the project lives in the build tree while the generated
-- views and the analyzer live in the source tree.
local csproj = string.format([[
<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <TargetFramework>net11.0</TargetFramework>
    <AssemblyName>Resonate.Tests.Reload.Patch</AssemblyName>
    <RootNamespace>Resonate.Tests.Reload</RootNamespace>
    <Nullable>enable</Nullable>
    <ImplicitUsings>disable</ImplicitUsings>
    <TreatWarningsAsErrors>true</TreatWarningsAsErrors>
    <AllowUnsafeBlocks>true</AllowUnsafeBlocks>
  </PropertyGroup>
  <ItemGroup>
    <Compile Include="%s/**/*.g.cs" />
    <ProjectReference Include="%s/packages/tools/analyzers/Resonate.CSharp.Attributes/Resonate.CSharp.Attributes.csproj" />
    <ProjectReference Include="%s/packages/tools/analyzers/Resonate.Analyzers/Resonate.Analyzers.csproj"
                      OutputItemType="Analyzer" ReferenceOutputAssembly="false" />
  </ItemGroup>
</Project>
]], slash(GENERATED), slash(PROJECT_DIR), slash(PROJECT_DIR))

io.writefile(path.join(OUT, "Reload.Patch.csproj"), csproj)

-- Version 0 is the warm-up build; the bench rewrites this file for every
-- measured iteration.
local source = [[
using Resonate.CSharp;
using Resonate.CSharp.Bindings;

[assembly: ResonateBehaviour]

namespace Resonate.Tests.Reload;

public static unsafe class PatchBehaviour
{
    [UpdateIn(ResonateStage.Update)]
    public static void Tick(SystemContext* frame)
    {
        int* counters = (int*)frame->User;
        counters[1] = 0;
    }
}
]]
io.writefile(path.join(OUT, "Behaviour.cs"), source)

local warm = os.execv("dotnet", {"build", path.join(OUT, "Reload.Patch.csproj"), "-c",
                                 "Debug", "-o", path.join(OUT, "out"), "--nologo", "-v",
                                 "quiet"}, {try = true})
if warm ~= 0 then
    raise("csharp reload: the warm-up build failed (exit " .. tostring(warm) ..
              "); the .NET SDK is required")
end
