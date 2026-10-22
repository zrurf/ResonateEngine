using System;
using System.Collections.Generic;
using System.Collections.Immutable;
using System.Linq;
using System.Text;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.Text;

namespace Resonate.Analyzers;

/* Law 4: an entry point runs only where it says. Every method carrying
   [UpdateIn] becomes part of the assembly's generated registration, grouped
   into one trampoline per stage; an entry-shaped method without the attribute
   is a warning, because the engine will never call it.

   The generated type holds the assembly's unmanaged entry (RegisterUnmanaged)
   and a plain Register the patch host can invoke through reflection. */
[Generator(LanguageNames.CSharp)]
public sealed class RegistrationGenerator : IIncrementalGenerator
{
    public static readonly DiagnosticDescriptor InvalidEntry = new(
        id: "RESONATE0004",
        title: "Update declaration has an unusable entry",
        messageFormat: "'{0}' cannot be registered: {1}",
        category: "Resonate",
        defaultSeverity: DiagnosticSeverity.Warning,
        isEnabledByDefault: true,
        description: "An [UpdateIn] method is static, returns void, takes one SystemContext* " +
                     "and names a stage the schedule runs.");

    public static readonly DiagnosticDescriptor MissingUpdateIn = new(
        id: "RESONATE0005",
        title: "Behaviour entry has no update declaration",
        messageFormat: "'{0}' looks like an entry but has no [UpdateIn]; the engine never runs it",
        category: "Resonate",
        defaultSeverity: DiagnosticSeverity.Warning,
        isEnabledByDefault: true,
        description: "A static void method taking one SystemContext* is an entry and needs " +
                     "[UpdateIn(stage)].");

    /* The stage values are the ABI's (see ResonateStage); the name is the
       contract's system entry the registration uses. */
    private static readonly (int Value, string Name)[] Stages =
    {
        (1, "EarlyUpdate"),
        (2, "Update"),
        (3, "Physics"),
        (4, "LateUpdate"),
        (5, "Render"),
        (6, "Present"),
    };

    public void Initialize(IncrementalGeneratorInitializationContext context)
    {
        /* The declaration surface is the whole compilation: entries can sit in
           any type, and the assembly attribute decides whether the laws apply. */
        context.RegisterSourceOutput(context.CompilationProvider,
                                     (production, compilation) => Execute(production, compilation));
    }

    private static void Execute(SourceProductionContext context, Compilation compilation)
    {
        if (!IsBehaviourAssembly(compilation.Assembly))
        {
            return;
        }

        List<Entry> entries = new();
        foreach (INamedTypeSymbol type in AllTypes(compilation.Assembly.GlobalNamespace))
        {
            foreach (IMethodSymbol method in type.GetMembers().OfType<IMethodSymbol>())
            {
                if (method.MethodKind != MethodKind.Ordinary || method.IsImplicitlyDeclared)
                {
                    continue;
                }
                bool declared = TryGetStage(method, out int stage);
                bool entry_shaped = IsEntryShaped(method);
                if (declared)
                {
                    string reason = EntryProblem(method, stage);
                    if (reason.Length != 0)
                    {
                        context.ReportDiagnostic(Diagnostic.Create(
                            InvalidEntry, method.Locations[0], method.Name, reason));
                        continue;
                    }
                    entries.Add(new Entry(method, stage, OrderOf(method, compilation)));
                }
                else if (entry_shaped)
                {
                    context.ReportDiagnostic(Diagnostic.Create(MissingUpdateIn, method.Locations[0],
                                                               method.Name));
                }
            }
        }
        if (entries.Count == 0)
        {
            return;
        }

        context.AddSource("BehaviourRegistration.g.cs",
                          SourceText.From(Emit(entries), Encoding.UTF8));
    }

    /* An entry is static, returns void and takes the generated frame context by
       pointer: the shape a stage trampoline hands over without marshalling. */
    private static bool IsEntryShaped(IMethodSymbol method)
    {
        return method.IsStatic && method.ReturnsVoid && method.Parameters.Length == 1 &&
               IsFrameContext(method.Parameters[0].Type);
    }

    private static bool IsFrameContext(ITypeSymbol type)
    {
        return type is IPointerTypeSymbol pointer &&
               pointer.PointedAtType.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat) ==
                   "global::Resonate.CSharp.Bindings.SystemContext";
    }

    private static bool TryGetStage(IMethodSymbol method, out int stage)
    {
        stage = 0;
        foreach (AttributeData attribute in method.GetAttributes())
        {
            if (attribute.AttributeClass?.ToDisplayString() != "Resonate.CSharp.UpdateInAttribute")
            {
                continue;
            }
            if (attribute.ConstructorArguments.Length == 1 &&
                attribute.ConstructorArguments[0].Value is not null)
            {
                /* The enum's underlying type is byte, so the argument does not
                   arrive boxed as int. */
                stage = Convert.ToInt32(attribute.ConstructorArguments[0].Value,
                                        System.Globalization.CultureInfo.InvariantCulture);
            }
            return true;
        }
        return false;
    }

    private static string EntryProblem(IMethodSymbol method, int stage)
    {
        if (!IsEntryShaped(method))
        {
            return "expected static void with a single SystemContext* parameter";
        }
        if (!Stages.Any(known => known.Value == stage))
        {
            return "the stage is not one the schedule runs";
        }
        return string.Empty;
    }

    private static int OrderOf(IMethodSymbol method, Compilation compilation)
    {
        Location location = method.Locations[0];
        int index = 0;
        foreach (SyntaxTree tree in compilation.SyntaxTrees)
        {
            if (tree == location.SourceTree)
            {
                return index * 1_000_000 + location.SourceSpan.Start;
            }
            ++index;
        }
        return int.MaxValue;
    }

    private static IEnumerable<INamedTypeSymbol> AllTypes(INamespaceOrTypeSymbol container)
    {
        foreach (ISymbol member in container.GetMembers())
        {
            if (member is INamespaceSymbol nested_namespace)
            {
                foreach (INamedTypeSymbol type in AllTypes(nested_namespace))
                {
                    yield return type;
                }
            }
            else if (member is INamedTypeSymbol type)
            {
                yield return type;
                foreach (INamedTypeSymbol nested in AllTypes(type))
                {
                    yield return nested;
                }
            }
        }
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

    private static string Emit(List<Entry> entries)
    {
        /* Calls within a stage keep source order: the declaration is the
           schedule, and a patch reload preserves it. */
        entries.Sort((left, right) => left.Order != right.Order
                                          ? left.Order.CompareTo(right.Order)
                                          : string.CompareOrdinal(left.Method.Name,
                                                                  right.Method.Name));

        StringBuilder text = new();
        text.AppendLine("// Generated from [UpdateIn] declarations. Do not edit.");
        text.AppendLine("#pragma warning disable");
        text.AppendLine("namespace Resonate.CSharp.Generated");
        text.AppendLine("{");
        text.AppendLine("    public static unsafe class BehaviourRegistration");
        text.AppendLine("    {");
        text.AppendLine("        public static int Register(global::System.IntPtr api)");
        text.AppendLine("        {");
        text.AppendLine("            global::Resonate.CSharp.Bindings.Registration* registration =");
        text.AppendLine("                (global::Resonate.CSharp.Bindings.Registration*)api;");
        text.AppendLine("            int status = 0;");
        foreach ((int stage, string name, List<Entry> group) in Group(entries))
        {
            text.AppendLine(
                $"            status |= registration->RegisterEntry(registration->Context, " +
                $"{(uint)stage}U, global::Resonate.CSharp.Bindings.Systems.{name}, " +
                $"(global::System.IntPtr)(delegate* unmanaged<global::System.IntPtr, void>)" +
                $"&{name});");
        }
        text.AppendLine("            return status;");
        text.AppendLine("        }");
        text.AppendLine();
        text.AppendLine(
            "        [global::System.Runtime.InteropServices.UnmanagedCallersOnly]");
        text.AppendLine(
            "        public static int RegisterUnmanaged(global::System.IntPtr api) => Register(api);");
        foreach ((int stage, string name, List<Entry> group) in Group(entries))
        {
            text.AppendLine();
            text.AppendLine(
                "        [global::System.Runtime.InteropServices.UnmanagedCallersOnly]");
            text.AppendLine($"        private static void {name}(global::System.IntPtr context)");
            text.AppendLine("        {");
            foreach (Entry entry in group)
            {
                string target = entry.Method.ContainingType.ToDisplayString(
                                    SymbolDisplayFormat.FullyQualifiedFormat) +
                                "." + entry.Method.Name;
                text.AppendLine(
                    $"            {target}((global::Resonate.CSharp.Bindings.SystemContext*)context);");
            }
            text.AppendLine("        }");
        }
        text.AppendLine("    }");
        text.AppendLine("}");
        return text.ToString();
    }

    private static IEnumerable<(int Stage, string Name, List<Entry> Group)> Group(
        List<Entry> entries)
    {
        foreach ((int value, string name) in Stages)
        {
            List<Entry> group = entries.Where(entry => entry.Stage == value).ToList();
            if (group.Count != 0)
            {
                yield return (value, name, group);
            }
        }
    }

    private sealed class Entry
    {
        public Entry(IMethodSymbol method, int stage, int order)
        {
            Method = method;
            Stage = stage;
            Order = order;
        }

        public IMethodSymbol Method { get; }

        public int Stage { get; }

        public int Order { get; }
    }
}
