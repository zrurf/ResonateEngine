using System.Collections.Immutable;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.Diagnostics;
using Microsoft.CodeAnalysis.Operations;

namespace Resonate.Analyzers;

/* Law 3: self is writable, others are read-only, and the types say which is
   which. The frame context still carries the raw chunk pointers for the
   generated views, but behaviour code reaching them directly gets a warning:
   SelfView<T> and ReadView<T> are the sanctioned shapes. */
[DiagnosticAnalyzer(LanguageNames.CSharp)]
public sealed class RawChunkAccessAnalyzer : DiagnosticAnalyzer
{
    public static readonly DiagnosticDescriptor Rule = new(
        id: "RESONATE0003",
        title: "Component memory is reached through a typed view",
        messageFormat: "'{0}' bypasses the typed view; use SelfView<T> or ReadView<T>",
        category: "Resonate",
        defaultSeverity: DiagnosticSeverity.Warning,
        isEnabledByDefault: true,
        description: "SelfView<T> is writable and ReadView<T> is read-only; raw chunk " +
                     "pointers carry neither guarantee.");

    private static readonly ImmutableHashSet<string> RawPointers = ImmutableHashSet.Create(
        "ChunkData", "ChunkCount", "EntityData");

    public override ImmutableArray<DiagnosticDescriptor> SupportedDiagnostics =>
        ImmutableArray.Create(Rule);

    public override void Initialize(AnalysisContext context)
    {
        context.ConfigureGeneratedCodeAnalysis(GeneratedCodeAnalysisFlags.None);
        context.EnableConcurrentExecution();
        context.RegisterOperationAction(AnalyzeField, OperationKind.FieldReference);
    }

    private static void AnalyzeField(OperationAnalysisContext context)
    {
        if (!IsBehaviourAssembly(context.Compilation.Assembly))
        {
            return;
        }
        var reference = (IFieldReferenceOperation)context.Operation;
        if (!RawPointers.Contains(reference.Field.Name) ||
            reference.Field.ContainingType.ToDisplayString() !=
                "Resonate.CSharp.Bindings.SystemContext")
        {
            return;
        }
        context.ReportDiagnostic(Diagnostic.Create(Rule, reference.Syntax.GetLocation(),
                                                   reference.Field.Name));
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
