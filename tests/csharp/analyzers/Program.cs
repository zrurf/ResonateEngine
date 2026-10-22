using System;
using System.Collections.Generic;
using System.Collections.Immutable;
using System.Linq;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.Diagnostics;
using Resonate.Analyzers;

namespace Resonate.Analyzers.Tests;

/* Runs the analyzers and the registration generator over source strings: the
   laws are build-time contracts, so their own checks are a program with a
   non-zero exit, not a convention. */
internal static class Program
{
    /* The attributes and the generated boundary types the snippets and the
       generated registration compile against. */
    private const string Stubs =
        "namespace Resonate.CSharp {" +
        " [System.AttributeUsage(System.AttributeTargets.Assembly)]" +
        " public sealed class ResonateBehaviourAttribute : System.Attribute {}" +
        " [System.AttributeUsage(System.AttributeTargets.Struct | System.AttributeTargets.Class)]" +
        " public sealed class ResonateViewAttribute : System.Attribute {}" +
        " public enum ResonateStage : byte { EarlyUpdate = 1, Update = 2, Physics = 3," +
        " LateUpdate = 4, Render = 5, Present = 6 }" +
        " [System.AttributeUsage(System.AttributeTargets.Method)]" +
        " public sealed class UpdateInAttribute : System.Attribute {" +
        " public UpdateInAttribute(ResonateStage stage) { Stage = stage; }" +
        " public ResonateStage Stage { get; } }" +
        "}" +
        "namespace Resonate.CSharp.Bindings {" +
        " public unsafe struct Registration { public System.IntPtr Context;" +
        " public delegate* unmanaged<System.IntPtr, uint, uint, System.IntPtr, int> RegisterEntry; }" +
        " public static class Systems { public const uint EarlyUpdate = 11U;" +
        " public const uint Update = 7U; public const uint Physics = 13U;" +
        " public const uint LateUpdate = 17U; public const uint Render = 19U;" +
        " public const uint Present = 23U; }" +
        " public unsafe struct HostApi {" +
        " public delegate* unmanaged<System.IntPtr, ulong*, int> Spawn;" +
        " public delegate* unmanaged<System.IntPtr, ulong, byte*, System.IntPtr, int> AddComponent;" +
        " public delegate* unmanaged<System.IntPtr, ulong, byte*, int> RemoveComponent;" +
        " public delegate* unmanaged<System.IntPtr, ulong, int> Despawn;" +
        " public delegate* unmanaged<System.IntPtr, int> MarkChunkChanged; }" +        " public struct SystemContext { public float DeltaSeconds;" +
        " public System.IntPtr HostApi; public System.IntPtr Chunks;" +
        " public ulong ChunkCount; }" +
        "}";

    private const string Marked = "[assembly: Resonate.CSharp.ResonateBehaviour]\n";
    private const string Mutable =
        "namespace Game { public static class State { public static int Frames; } }";
    private const string ReadOnly =
        "namespace Game { public static class State { public static readonly int Frames = 0; } }";
    private const string ViewType =
        "[Resonate.CSharp.ResonateView] public struct LocalTransform { public float X; }";
    private const string CachedView =
        ViewType + " namespace Game { public class Behaviour { private LocalTransform cached; } }";
    private const string ViewLocal =
        ViewType + " namespace Game { public class Behaviour { public void Tick() {" +
        " LocalTransform local = default; local.X = 1.0F; } } }";
    private const string PlainField =
        "public struct Plain { public float X; }" +
        " namespace Game { public class Behaviour { private Plain value; } }";

    private const string UpdateEntry =
        "namespace Game { public static unsafe class Behaviour {" +
        " [Resonate.CSharp.UpdateIn(Resonate.CSharp.ResonateStage.Update)]" +
        " public static void Tick(Resonate.CSharp.Bindings.SystemContext* frame) {" +
        " frame->DeltaSeconds = 1.0F; } } }";
    private const string LateEntry =
        "namespace Game { public static unsafe class Camera {" +
        " [Resonate.CSharp.UpdateIn(Resonate.CSharp.ResonateStage.LateUpdate)]" +
        " public static void Follow(Resonate.CSharp.Bindings.SystemContext* frame) {" +
        " frame->DeltaSeconds = 2.0F; } } }";
    private const string UnmarkedEntry =
        "namespace Game { public static unsafe class Behaviour {" +
        " public static void Tick(Resonate.CSharp.Bindings.SystemContext* frame) { } } }";
    private const string BadEntry =
        "namespace Game { public static class Behaviour {" +
        " [Resonate.CSharp.UpdateIn(Resonate.CSharp.ResonateStage.Update)]" +
        " public static void Tick(int frame) { } } }";
    private const string RawStructural =
        "[assembly: Resonate.CSharp.ResonateBehaviour]\n" +
        "namespace Game { public static unsafe class Behaviour {" +
        " [Resonate.CSharp.UpdateIn(Resonate.CSharp.ResonateStage.Update)]" +
        " public static void Tick(Resonate.CSharp.Bindings.SystemContext* frame) {" +
        " var api = (Resonate.CSharp.Bindings.HostApi*)frame->HostApi;" +
        " ulong entity = 0; api->Spawn((System.IntPtr)api, &entity); } } }";
    private const string RawChunk =
        "[assembly: Resonate.CSharp.ResonateBehaviour]\n" +
        "namespace Game { public static unsafe class Behaviour {" +
        " [Resonate.CSharp.UpdateIn(Resonate.CSharp.ResonateStage.Update)]" +
        " public static void Tick(Resonate.CSharp.Bindings.SystemContext* frame) {" +
        " System.IntPtr data = frame->Chunks; } } }";
    private const string RawMarking =
        "[assembly: Resonate.CSharp.ResonateBehaviour]\n" +
        "namespace Game { public static unsafe class Behaviour {" +
        " [Resonate.CSharp.UpdateIn(Resonate.CSharp.ResonateStage.Update)]" +
        " public static void Tick(Resonate.CSharp.Bindings.SystemContext* frame) {" +
        " var api = (Resonate.CSharp.Bindings.HostApi*)frame->HostApi;" +
        " api->MarkChunkChanged((System.IntPtr)api); } } }";

    private static int Main()
    {
        int failures = 0;
        failures += Check("marked + mutable static", Marked + Mutable, "RESONATE0001", 1);
        failures += Check("marked + readonly static", Marked + ReadOnly, "RESONATE0001", 0);
        failures += Check("unmarked + mutable static", Mutable, "RESONATE0001", 0);
        failures += Check("marked + cached view field", Marked + CachedView, "RESONATE0006", 1);
        failures += Check("marked + view local", Marked + ViewLocal, "RESONATE0006", 0);
        failures += Check("marked + plain field", Marked + PlainField, "RESONATE0006", 0);
        failures += Check("raw structural call", RawStructural, "RESONATE0002", 1);
        failures += Check("raw chunk pointer", RawChunk, "RESONATE0003", 1);
        failures += Check("raw change marking", RawMarking, "RESONATE0007", 1);

        failures += CheckGenerator("declared update entry", Marked + UpdateEntry, 0, null,
                                   new[] { "Systems.Update", "Game.Behaviour.Tick" });
        failures += CheckGenerator("declared entries in two stages",
                                   Marked + UpdateEntry + LateEntry, 0, null,
                                   new[] { "Systems.Update", "Systems.LateUpdate" });
        failures += CheckGenerator("entry without a declaration", Marked + UnmarkedEntry, 1,
                                   "RESONATE0005", Array.Empty<string>());
        failures += CheckGenerator("declaration with a bad signature", Marked + BadEntry, 1,
                                   "RESONATE0004", Array.Empty<string>());

        Console.WriteLine(failures == 0 ? "analyzers: ok" : $"analyzers: {failures} failure(s)");
        return failures == 0 ? 0 : 1;
    }

    private static int Check(string name, string source, string rule, int expected)
    {
        CSharpParseOptions parse = new(LanguageVersion.Latest);
        CSharpCompilation compilation = CSharpCompilation.Create(
            "Behaviour",
            new[] { CSharpSyntaxTree.ParseText(Stubs, parse),
                    CSharpSyntaxTree.ParseText(source, parse) },
            new[] { MetadataReference.CreateFromFile(typeof(object).Assembly.Location) },
            new CSharpCompilationOptions(OutputKind.DynamicallyLinkedLibrary, allowUnsafe: true));
        ImmutableArray<Diagnostic> diagnostics = compilation
            .WithAnalyzers(ImmutableArray.Create<DiagnosticAnalyzer>(
                new NoMutableStaticsAnalyzer(), new CachedViewAnalyzer(),
                new StructuralBypassAnalyzer(), new RawChunkAccessAnalyzer(),
                new ChangeMarkingAnalyzer()))
            .GetAnalyzerDiagnosticsAsync()
            .GetAwaiter()
            .GetResult();
        int count = diagnostics.Count(diagnostic => diagnostic.Id == rule);
        if (count != expected)
        {
            Console.WriteLine($"{name}: expected {expected} {rule} diagnostic(s), got {count}");
            foreach (Diagnostic diagnostic in diagnostics)
            {
                Console.WriteLine("  " + diagnostic);
            }
            return 1;
        }
        return 0;
    }

    /* Runs the registration generator and checks its diagnostics, the generated
       registration, and — for a clean case — that the result compiles. */
    private static int CheckGenerator(string name, string source, int expectedDiagnostics,
                                      string? rule, IReadOnlyList<string> fragments)
    {
        CSharpParseOptions parse = new(LanguageVersion.Latest);
        CSharpCompilation compilation = CSharpCompilation.Create(
            "Behaviour",
            new[] { CSharpSyntaxTree.ParseText(Stubs, parse),
                    CSharpSyntaxTree.ParseText(source, parse) },
            new[]
            {
                MetadataReference.CreateFromFile(typeof(object).Assembly.Location),
                MetadataReference.CreateFromFile(
                    typeof(System.Runtime.InteropServices.UnmanagedCallersOnlyAttribute).Assembly
                        .Location),
            },
            new CSharpCompilationOptions(OutputKind.DynamicallyLinkedLibrary, allowUnsafe: true));

        GeneratorDriver driver = CSharpGeneratorDriver.Create(new RegistrationGenerator());
        driver = driver.RunGeneratorsAndUpdateCompilation(
            compilation, out Compilation output, out ImmutableArray<Diagnostic> diagnostics);

        int count = expectedDiagnostics == 0
                        ? 0
                        : diagnostics.Count(diagnostic => diagnostic.Id == rule);
        if (count != expectedDiagnostics)
        {
            Console.WriteLine(
                $"{name}: expected {expectedDiagnostics} {rule} diagnostic(s), got {count}");
            foreach (Diagnostic diagnostic in diagnostics)
            {
                Console.WriteLine("  " + diagnostic);
            }
            return 1;
        }

        GeneratorRunResult result = driver.GetRunResult().Results[0];
        string generated = string.Join(
            "\n", result.GeneratedSources.Select(generated_source =>
                                                   generated_source.SourceText.ToString()));
        if (expectedDiagnostics != 0)
        {
            return fragments.Count == 0 && result.GeneratedSources.Length == 0 ? 0 : Report(
                name, "a rejected declaration still produced a registration");
        }
        if (result.GeneratedSources.Length != 1)
        {
            Console.WriteLine($"{name}: expected one generated source, got {result.GeneratedSources.Length}");
            foreach (Diagnostic diagnostic in diagnostics)
            {
                Console.WriteLine("  " + diagnostic);
            }
            return 1;
        }
        foreach (string fragment in fragments)
        {
            if (!generated.Contains(fragment, StringComparison.Ordinal))
            {
                return Report(name, $"the registration lacks '{fragment}'");
            }
        }
        ImmutableArray<Diagnostic> errors = output.GetDiagnostics()
                                                .Where(diagnostic =>
                                                           diagnostic.Severity ==
                                                           DiagnosticSeverity.Error)
                                                .ToImmutableArray();
        if (errors.Length != 0)
        {
            Console.WriteLine($"{name}: the registration does not compile");
            foreach (Diagnostic error in errors)
            {
                Console.WriteLine("  " + error);
            }
            return 1;
        }
        return 0;
    }

    private static int Report(string name, string message)
    {
        Console.WriteLine($"{name}: {message}");
        return 1;
    }
}
