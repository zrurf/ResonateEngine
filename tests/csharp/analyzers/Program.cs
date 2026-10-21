using System;
using System.Collections.Immutable;
using System.Linq;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.Diagnostics;
using Resonate.Analyzers;

namespace Resonate.Analyzers.Tests;

/* Runs the analyzers over source strings: the laws are build-time contracts,
   so their own checks are a program with a non-zero exit, not a convention. */
internal static class Program
{
    /* Both attributes the analyzers look for, declared where the snippets can
       apply them. */
    private const string Stubs =
        "namespace Resonate.CSharp {" +
        " [System.AttributeUsage(System.AttributeTargets.Assembly)]" +
        " public sealed class ResonateBehaviourAttribute : System.Attribute {}" +
        " [System.AttributeUsage(System.AttributeTargets.Struct | System.AttributeTargets.Class)]" +
        " public sealed class ResonateViewAttribute : System.Attribute {}" +
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

    private static int Main()
    {
        int failures = 0;
        failures += Check("marked + mutable static", Marked + Mutable, "RESONATE0001", 1);
        failures += Check("marked + readonly static", Marked + ReadOnly, "RESONATE0001", 0);
        failures += Check("unmarked + mutable static", Mutable, "RESONATE0001", 0);
        failures += Check("marked + cached view field", Marked + CachedView, "RESONATE0006", 1);
        failures += Check("marked + view local", Marked + ViewLocal, "RESONATE0006", 0);
        failures += Check("marked + plain field", Marked + PlainField, "RESONATE0006", 0);
        Console.WriteLine(failures == 0 ? "analyzers: ok" : $"analyzers: {failures} failure(s)");
        return failures == 0 ? 0 : 1;
    }

    private static int Check(string name, string source, string rule, int expected)
    {
        CSharpCompilation compilation = CSharpCompilation.Create(
            "Behaviour",
            new[] { CSharpSyntaxTree.ParseText(Stubs), CSharpSyntaxTree.ParseText(source) },
            new[] { MetadataReference.CreateFromFile(typeof(object).Assembly.Location) },
            new CSharpCompilationOptions(OutputKind.DynamicallyLinkedLibrary));
        ImmutableArray<Diagnostic> diagnostics = compilation
            .WithAnalyzers(ImmutableArray.Create<DiagnosticAnalyzer>(new NoMutableStaticsAnalyzer(),
                                                                     new CachedViewAnalyzer()))
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
}
