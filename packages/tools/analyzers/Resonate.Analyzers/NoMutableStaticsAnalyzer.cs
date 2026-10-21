using System.Collections.Immutable;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.Diagnostics;

namespace Resonate.Analyzers;

/* Law 1: game state lives in native components. A mutable managed static
   outlives a patch swap and is invisible to save/rollback, so a behaviour
   assembly gets a warning for every one. Constants and readonly values are
   configuration, not state, and pass. */
[DiagnosticAnalyzer(LanguageNames.CSharp)]
public sealed class NoMutableStaticsAnalyzer : DiagnosticAnalyzer
{
    public static readonly DiagnosticDescriptor Rule = new(
        id: "RESONATE0001",
        title: "Behaviour state belongs to the engine",
        messageFormat: "mutable static field '{0}' in a behaviour assembly",
        category: "Resonate",
        defaultSeverity: DiagnosticSeverity.Warning,
        isEnabledByDefault: true,
        description: "Game state lives in native components; a managed static outlives a patch swap.");

    public override ImmutableArray<DiagnosticDescriptor> SupportedDiagnostics =>
        ImmutableArray.Create(Rule);

    public override void Initialize(AnalysisContext context)
    {
        context.ConfigureGeneratedCodeAnalysis(GeneratedCodeAnalysisFlags.None);
        context.EnableConcurrentExecution();
        context.RegisterSymbolAction(AnalyzeField, SymbolKind.Field);
    }

    private static void AnalyzeField(SymbolAnalysisContext context)
    {
        IFieldSymbol field = (IFieldSymbol)context.Symbol;
        if (!field.IsStatic || field.IsConst || field.IsReadOnly || field.IsImplicitlyDeclared)
        {
            return;
        }
        if (!IsBehaviourAssembly(field.ContainingAssembly))
        {
            return;
        }
        context.ReportDiagnostic(Diagnostic.Create(Rule, field.Locations[0], field.Name));
    }

    private static bool IsBehaviourAssembly(IAssemblySymbol assembly)
    {
        foreach (AttributeData attribute in assembly.GetAttributes())
        {
            if (attribute.AttributeClass?.ToDisplayString() ==
                "Resonate.CSharp.ResonateBehaviourAttribute")
            {
                return true;
            }
        }
        return false;
    }
}
