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
    private const string AttributeStub =
        "namespace Resonate.CSharp { [System.AttributeUsage(System.AttributeTargets.Assembly)] " +
        "public sealed class ResonateBehaviourAttribute : System.Attribute { } }";

    private const string Marked = "[assembly: Resonate.CSharp.ResonateBehaviour]\n";
    private const string Mutable =
        "namespace Game { public static class State { public static int Frames; } }";
    private const string ReadOnly =
        "namespace Game { public static class State { public static readonly int Frames = 0; } }";

    private static int Main()
    {
        int failures = 0;
        failures += Check("marked + mutable static", Marked + Mutable, 1);
        failures += Check("marked + readonly static", Marked + ReadOnly, 0);
        failures += Check("unmarked + mutable static", Mutable, 0);
        Console.WriteLine(failures == 0 ? "analyzers: ok" : $"analyzers: {failures} failure(s)");
        return failures == 0 ? 0 : 1;
    }

    private static int Check(string name, string source, int expected)
    {
        CSharpCompilation compilation = CSharpCompilation.Create(
            "Behaviour",
            new[]
            {
                CSharpSyntaxTree.ParseText(AttributeStub),
                CSharpSyntaxTree.ParseText(source),
            },
            new[] { MetadataReference.CreateFromFile(typeof(object).Assembly.Location) },
            new CSharpCompilationOptions(OutputKind.DynamicallyLinkedLibrary));
        ImmutableArray<Diagnostic> diagnostics = compilation
            .WithAnalyzers(ImmutableArray.Create<DiagnosticAnalyzer>(new NoMutableStaticsAnalyzer()))
            .GetAnalyzerDiagnosticsAsync()
            .GetAwaiter()
            .GetResult();
        int count = diagnostics.Count(diagnostic => diagnostic.Id == "RESONATE0001");
        if (count != expected)
        {
            Console.WriteLine($"{name}: expected {expected} diagnostic(s), got {count}");
            foreach (Diagnostic diagnostic in diagnostics)
            {
                Console.WriteLine("  " + diagnostic);
            }
            return 1;
        }
        return 0;
    }
}
