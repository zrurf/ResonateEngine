using System.Collections.Immutable;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.Diagnostics;
using Microsoft.CodeAnalysis.Operations;

namespace Resonate.Analyzers;

/* Law 7: a change is never invisible. SelfView<T> marks the chunk when it is
   disposed, so a behaviour that calls the raw marker is bypassing the wrapper
   and can forget its own mark. */
[DiagnosticAnalyzer(LanguageNames.CSharp)]
public sealed class ChangeMarkingAnalyzer : DiagnosticAnalyzer
{
    public static readonly DiagnosticDescriptor Rule = new(
        id: "RESONATE0007",
        title: "Change marking is automatic",
        messageFormat: "'{0}' is called by the typed view; write through SelfView<T>",
        category: "Resonate",
        defaultSeverity: DiagnosticSeverity.Warning,
        isEnabledByDefault: true,
        description: "SelfView<T> marks the chunk on dispose, so observers always see the " +
                     "write; the raw marker is not part of the behaviour surface.");

    public override ImmutableArray<DiagnosticDescriptor> SupportedDiagnostics =>
        ImmutableArray.Create(Rule);

    public override void Initialize(AnalysisContext context)
    {
        context.ConfigureGeneratedCodeAnalysis(GeneratedCodeAnalysisFlags.None);
        context.EnableConcurrentExecution();
        context.RegisterOperationAction(AnalyzeInvocation, OperationKind.FunctionPointerInvocation);
    }

    private static void AnalyzeInvocation(OperationAnalysisContext context)
    {
        if (!IsBehaviourAssembly(context.Compilation.Assembly))
        {
            return;
        }
        var invocation = (IFunctionPointerInvocationOperation)context.Operation;
        if (invocation.Target is not IFieldReferenceOperation field ||
            field.Field.Name != "MarkChunkChanged" ||
            field.Field.ContainingType.ToDisplayString() != "Resonate.CSharp.Bindings.HostApi")
        {
            return;
        }
        context.ReportDiagnostic(Diagnostic.Create(Rule, invocation.Syntax.GetLocation(),
                                                   field.Field.Name));
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
