using System.Collections.Immutable;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.Diagnostics;
using Microsoft.CodeAnalysis.Operations;

namespace Resonate.Analyzers;

/* Law 2: structural changes go through the command buffer. The ABI keeps the
   raw structural pointers, but behaviour code should hold the generated
   Commands facade; calling the pointers directly means the change is recorded
   without the frame's sync point in view, so it gets a warning. */
[DiagnosticAnalyzer(LanguageNames.CSharp)]
public sealed class StructuralBypassAnalyzer : DiagnosticAnalyzer
{
    public static readonly DiagnosticDescriptor Rule = new(
        id: "RESONATE0002",
        title: "Structural changes go through the command channel",
        messageFormat: "'{0}' bypasses the generated Commands facade",
        category: "Resonate",
        defaultSeverity: DiagnosticSeverity.Warning,
        isEnabledByDefault: true,
        description: "Spawn, add, remove and despawn are recorded through Commands so the " +
                     "frame's sync point plays them.");

    /* The contract's structural functions as csbind names them on the C# side. */
    private static readonly ImmutableHashSet<string> Structural = ImmutableHashSet.Create(
        "Spawn", "AddComponent", "RemoveComponent", "Despawn");

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
            !Structural.Contains(field.Field.Name) ||
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
