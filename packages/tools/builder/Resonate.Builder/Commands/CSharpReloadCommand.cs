using System.Collections.Generic;
using System.IO;

namespace Resonate.Builder.Commands;

/* Prepares the hot-reload acceptance bench. The loop itself is native
   (tests/bench/csharp_reload.cpp): it rewrites Behaviour.cs, rebuilds the
   patch assembly, swaps it through the collectible ALC and invokes it, timing
   the whole path. This prepares the base fixture, the bootstrap and a warm
   incremental patch build. */
internal static class CSharpReloadCommand
{
    private const string PatchProject = """
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
            <Compile Include="__GENERATED__/**/*.g.cs" />
            <ProjectReference Include="__PROJECT__/packages/tools/analyzers/Resonate.CSharp.Attributes/Resonate.CSharp.Attributes.csproj" />
            <ProjectReference Include="__PROJECT__/packages/tools/analyzers/Resonate.Analyzers/Resonate.Analyzers.csproj"
                              OutputItemType="Analyzer" ReferenceOutputAssembly="false" />
          </ItemGroup>
        </Project>
        """;

    private const string Behaviour = """
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
        """;

    internal static int Run(BuildContext context, IReadOnlyList<string> args)
    {
        if (args.Count != 0)
        {
            context.Error.WriteLine($"resonate csharp-reload: unknown argument '{args[0]}'");
            return 2;
        }
        if (CSharpTestCommand.Run(context, []) != 0)
        {
            context.Error.WriteLine("csharp reload: publishing the base fixtures failed");
            return 1;
        }
        string output = context.BuildPath("tools", "csharp-reload");
        Directory.CreateDirectory(output);

        /* Absolute paths: the project lives in the build tree while the
           generated views and the analyzer live in the source tree. */
        string generated = Slash(context.BuildPath("gen", "csharp"));
        string project = Slash(context.ProjectDirectory);
        string csproj = PatchProject.Replace("__GENERATED__", generated, System.StringComparison.Ordinal)
            .Replace("__PROJECT__", project, System.StringComparison.Ordinal);
        File.WriteAllText(Path.Combine(output, "Reload.Patch.csproj"), csproj);

        /* Version 0 is the warm-up build; the bench rewrites this file for
           every measured iteration. */
        File.WriteAllText(Path.Combine(output, "Behaviour.cs"), Behaviour);

        int warm = ProcessRunner.Run("dotnet", ["build", Path.Combine(output, "Reload.Patch.csproj"),
                                                "-c", "Debug", "-o", Path.Combine(output, "out"),
                                                "--nologo", "-v", "quiet"]);
        if (warm != 0)
        {
            context.Error.WriteLine($"csharp reload: the warm-up build failed (exit {warm}); the "
                                    + ".NET SDK is required");
            return 1;
        }
        return 0;
    }

    private static string Slash(string path)
    {
        return path.Replace('\\', '/');
    }
}
