using System.Collections.Immutable;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.Diagnostics;

namespace Resonate.Analyzers;

/* Law 6: a view borrows the chunk it describes. Storing one (or a pointer or
   array of one) keeps a reference past the iteration that validated it, so a
   behaviour assembly gets a warning for every field that holds a view. */
[DiagnosticAnalyzer(LanguageNames.CSharp)]
public sealed class CachedViewAnalyzer : DiagnosticAnalyzer
{
    public static readonly DiagnosticDescriptor Rule = new(
        id: "RESONATE0006",
        title: "Component views are not cached",
        messageFormat: "field '{0}' stores a component view; read it where it is handed over",
        category: "Resonate",
        defaultSeverity: DiagnosticSeverity.Warning,
        isEnabledByDefault: true,
        description: "A view borrows its chunk; keeping it outlives the borrow.");

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
        if (field.IsImplicitlyDeclared || !IsView(field.Type))
        {
            return;
        }
        if (!IsBehaviourAssembly(field.ContainingAssembly))
        {
            return;
        }
        context.ReportDiagnostic(Diagnostic.Create(Rule, field.Locations[0], field.Name));
    }

    /* Pointers and arrays of a view are the same borrow. */
    private static bool IsView(ITypeSymbol type)
    {
        ITypeSymbol current = type;
        while (true)
        {
            if (current is IPointerTypeSymbol pointer)
            {
                current = pointer.PointedAtType;
                continue;
            }
            if (current is IArrayTypeSymbol array)
            {
                current = array.ElementType;
                continue;
            }
            break;
        }
        foreach (AttributeData attribute in current.GetAttributes())
        {
            if (attribute.AttributeClass?.ToDisplayString() ==
                "Resonate.CSharp.ResonateViewAttribute")
            {
                return true;
            }
        }
        return false;
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
