using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using System.Text.Json;

namespace Resonate.Tools.CsBind;

/* One stable id per registered system. The engine and the managed assemblies
   both read the generated output, so an id cannot drift between them. */
internal sealed class SystemEntry
{
    public string? Name { get; set; }
    public uint Id { get; set; }
}

/* A typed handle: an index in the low bits and a generation in the high bits.
   The wrapper keeps a raw integer from being mistaken for any other id. */
internal sealed class HandleEntry
{
    public string? Name { get; set; }
}

/* A boundary struct. Field types come from the closed vocabulary below, which
   is what lets both sides be generated with one layout. A field may instead
   name a function from the contract's `functions`. */
internal sealed class FieldEntry
{
    public string? Name { get; set; }
    public string? Type { get; set; }
    public string? Fn { get; set; }
}

/* A function pointer a boundary struct can carry; its return and parameter
   types come from the same vocabulary. */
internal sealed class FunctionEntry
{
    public string? Name { get; set; }
    public string? Ret { get; set; }
    public List<string>? Params { get; set; }
}

internal sealed class StructEntry
{
    public string? Name { get; set; }
    public List<FieldEntry>? Fields { get; set; }
}

internal sealed class Contract
{
    public List<SystemEntry>? Systems { get; set; }
    public List<HandleEntry>? Handles { get; set; }
    public List<FunctionEntry>? Functions { get; set; }
    public List<StructEntry>? Structs { get; set; }
}

internal readonly record struct FieldType(string Cpp, string CSharp, int Size, int Alignment);

internal sealed class FieldLayout
{
    public required FieldEntry Field { get; init; }
    public required FieldType Type { get; init; }
    public FunctionEntry? Function { get; init; }
    public int Offset { get; init; }
}

internal sealed class StructLayout
{
    public required string Name { get; init; }
    public required List<FieldLayout> Fields { get; init; }
    public required int Size { get; init; }
}

internal static class Program
{
    /* The closed vocabulary the generator knows how to lay out on both sides.
       A new type is added here and to the contract; nothing infers a layout
       from a native signature. Handle types join this map per contract. */
    private static readonly Dictionary<string, FieldType> Types = new(StringComparer.Ordinal)
    {
        ["u32"] = new("std::uint32_t", "uint", 4, 4),
        ["u64"] = new("std::uint64_t", "ulong", 8, 8),
        ["i32"] = new("std::int32_t", "int", 4, 4),
        ["f32"] = new("float", "float", 4, 4),
        ["f32_pointer"] = new("const float*", "float*", 8, 8),
        ["u8_pointer"] = new("const std::uint8_t*", "byte*", 8, 8),
        ["utf8_pointer"] = new("const char*", "byte*", 8, 8),
        ["u64_pointer"] = new("std::uint64_t*", "ulong*", 8, 8),
        ["void_pointer"] = new("void*", "IntPtr", 8, 8),
    };

    private static int Main(string[] args)
    {
        string? input = null;
        string? outDir = null;
        for (int index = 0; index < args.Length; ++index)
        {
            switch (args[index])
            {
                case "--input":
                    input = index + 1 < args.Length ? args[++index] : null;
                    break;
                case "--out":
                    outDir = index + 1 < args.Length ? args[++index] : null;
                    break;
                default:
                    Console.Error.WriteLine($"csbind: unknown argument '{args[index]}'");
                    return 2;
            }
        }
        if (input is null || outDir is null)
        {
            Console.Error.WriteLine("csbind: usage: csbind --input <bindings.json> --out <dir>");
            return 2;
        }

        Contract? contract;
        try
        {
            JsonSerializerOptions options = new() { PropertyNameCaseInsensitive = true };
            contract = JsonSerializer.Deserialize<Contract>(File.ReadAllText(input), options);
        }
        catch (Exception error) when (error is IOException or JsonException)
        {
            Console.Error.WriteLine($"csbind: reading {input} failed: {error.Message}");
            return 1;
        }
        if (contract?.Systems is null || contract.Systems.Count == 0)
        {
            Console.Error.WriteLine($"csbind: {input} declares no systems");
            return 1;
        }

        if (!ValidateSystems(contract.Systems, out List<(string Name, uint Id)> systems))
        {
            return 1;
        }
        if (!ValidateHandles(contract.Handles, out List<string> handles))
        {
            return 1;
        }

        Dictionary<string, FieldType> types = new(Types, StringComparer.Ordinal);
        HashSet<string> type_names = new(StringComparer.Ordinal);
        foreach (string handle in handles)
        {
            types[SnakeCase(handle) + "_handle"] =
                new($"{handle}Handle", $"{handle}Handle", 8, 8);
            type_names.Add($"{handle}Handle");
        }
        if (!ValidateFunctions(contract.Functions, types, out Dictionary<string, FunctionEntry> functions))
        {
            return 1;
        }
        if (!ValidateStructs(contract.Structs, types, functions, type_names, out List<StructLayout> layouts))
        {
            return 1;
        }

        Directory.CreateDirectory(Path.Combine(outDir, "resonate", "csharp"));
        WriteIfChanged(Path.Combine(outDir, "resonate", "csharp", "bindings.h"),
                       EmitHeader(input, systems, handles, layouts));
        WriteIfChanged(Path.Combine(outDir, "Resonate.CSharp.Bindings.g.cs"),
                       EmitCSharp(input, systems, handles, layouts));
        return 0;
    }

    private static bool ValidateSystems(List<SystemEntry> entries, out List<(string, uint)> systems)
    {
        systems = new List<(string, uint)>();
        HashSet<string> names = new(StringComparer.Ordinal);
        HashSet<uint> ids = new();
        foreach (SystemEntry entry in entries)
        {
            if (string.IsNullOrEmpty(entry.Name) || entry.Id == 0)
            {
                Console.Error.WriteLine("csbind: a system needs a name and a non-zero id");
                return false;
            }
            string name = PascalCase(entry.Name);
            if (!names.Add(name))
            {
                Console.Error.WriteLine($"csbind: duplicate system '{entry.Name}'");
                return false;
            }
            if (!ids.Add(entry.Id))
            {
                Console.Error.WriteLine($"csbind: duplicate id {entry.Id}");
                return false;
            }
            systems.Add((name, entry.Id));
        }
        return true;
    }

    private static bool ValidateHandles(List<HandleEntry>? entries, out List<string> handles)
    {
        handles = new List<string>();
        if (entries is null)
        {
            return true;
        }
        HashSet<string> names = new(StringComparer.Ordinal);
        foreach (HandleEntry entry in entries)
        {
            if (string.IsNullOrEmpty(entry.Name))
            {
                Console.Error.WriteLine("csbind: a handle needs a name");
                return false;
            }
            string name = PascalCase(entry.Name);
            if (!names.Add(name))
            {
                Console.Error.WriteLine($"csbind: duplicate handle '{entry.Name}'");
                return false;
            }
            handles.Add(name);
        }
        return true;
    }

    private static bool ValidateStructs(List<StructEntry>? entries,
                                        Dictionary<string, FieldType> types,
                                        Dictionary<string, FunctionEntry> functions,
                                        HashSet<string> reserved_type_names,
                                        out List<StructLayout> layouts)
    {
        layouts = new List<StructLayout>();
        if (entries is null)
        {
            return true;
        }

        HashSet<string> struct_names = new(StringComparer.Ordinal);
        foreach (StructEntry entry in entries)
        {
            if (string.IsNullOrEmpty(entry.Name) || entry.Fields is null || entry.Fields.Count == 0)
            {
                Console.Error.WriteLine("csbind: a struct needs a name and at least one field");
                return false;
            }
            if (!struct_names.Add(entry.Name) || !reserved_type_names.Add(entry.Name))
            {
                Console.Error.WriteLine($"csbind: duplicate type name '{entry.Name}'");
                return false;
            }

            List<FieldLayout> fields = new();
            HashSet<string> field_names = new(StringComparer.Ordinal);
            int offset = 0;
            int alignment = 1;
            foreach (FieldEntry field in entry.Fields)
            {
                if (string.IsNullOrEmpty(field.Name))
                {
                    Console.Error.WriteLine($"csbind: {entry.Name} has a field without a name");
                    return false;
                }
                if (!field_names.Add(field.Name))
                {
                    Console.Error.WriteLine($"csbind: {entry.Name} repeats field '{field.Name}'");
                    return false;
                }

                FieldType type;
                FunctionEntry? function = null;
                if (field.Fn is not null)
                {
                    if (!functions.TryGetValue(field.Fn, out function))
                    {
                        Console.Error.WriteLine(
                            $"csbind: {entry.Name}.{field.Name} names unknown function '{field.Fn}'");
                        return false;
                    }
                    /* A function pointer is pointer-sized on every target. */
                    type = new FieldType("", "", 8, 8);
                }
                else if (field.Type is not null && types.TryGetValue(field.Type, out type))
                {
                }
                else
                {
                    Console.Error.WriteLine(
                        $"csbind: {entry.Name} has a field with a missing name or unknown type " +
                        $"'{field.Type}'");
                    return false;
                }
                offset = AlignUp(offset, type.Alignment);
                fields.Add(new FieldLayout
                {
                    Field = field,
                    Type = type,
                    Function = function,
                    Offset = offset,
                });
                offset += type.Size;
                alignment = Math.Max(alignment, type.Alignment);
            }
            layouts.Add(new StructLayout
            {
                Name = entry.Name,
                Fields = fields,
                Size = AlignUp(offset, alignment),
            });
        }
        return true;
    }

    /* tick_rate and tick-rate both become TickRate; the C++ side keeps the
       contract's snake_case. */
    private static string PascalCase(string name)
    {
        StringBuilder builder = new(name.Length);
        bool upper = true;
        foreach (char character in name)
        {
            if (character == '_' || character == '-' || character == '.')
            {
                upper = true;
                continue;
            }
            builder.Append(upper ? char.ToUpperInvariant(character) : character);
            upper = false;
        }
        return builder.ToString();
    }

    /* EntityHandle -> entity_handle, the field type name the contract uses. */
    private static string SnakeCase(string name)
    {
        StringBuilder builder = new(name.Length + 4);
        for (int index = 0; index < name.Length; ++index)
        {
            char character = name[index];
            if (char.IsUpper(character) && index > 0)
            {
                builder.Append('_');
            }
            builder.Append(char.ToLowerInvariant(character));
        }
        return builder.ToString();
    }

    private static int AlignUp(int value, int alignment)
    {
        return (value + alignment - 1) / alignment * alignment;
    }

    private static bool ValidateFunctions(List<FunctionEntry>? entries,
                                          Dictionary<string, FieldType> types,
                                          out Dictionary<string, FunctionEntry> functions)
    {
        functions = new Dictionary<string, FunctionEntry>(StringComparer.Ordinal);
        if (entries is null)
        {
            return true;
        }
        foreach (FunctionEntry entry in entries)
        {
            if (string.IsNullOrEmpty(entry.Name) || entry.Ret is null || entry.Params is null)
            {
                Console.Error.WriteLine(
                    "csbind: a function needs a name, a return type and a parameter list");
                return false;
            }
            if (entry.Ret != "void" && !types.ContainsKey(entry.Ret))
            {
                Console.Error.WriteLine(
                    $"csbind: function '{entry.Name}' has unknown return '{entry.Ret}'");
                return false;
            }
            foreach (string parameter in entry.Params)
            {
                if (parameter == "void" || !types.ContainsKey(parameter))
                {
                    Console.Error.WriteLine(
                        $"csbind: function '{entry.Name}' has unknown parameter '{parameter}'");
                    return false;
                }
            }
            if (!functions.TryAdd(entry.Name, entry))
            {
                Console.Error.WriteLine($"csbind: duplicate function '{entry.Name}'");
                return false;
            }
        }
        return true;
    }

    private static string CppType(string type)
    {
        return type == "void" ? "void" : Types[type].Cpp;
    }

    private static string CppParameters(FunctionEntry function)
    {
        List<string> parameters = new();
        foreach (string parameter in function.Params!)
        {
            parameters.Add(CppType(parameter));
        }
        return string.Join(", ", parameters);
    }

    private static string CSharpType(string type)
    {
        return type == "void" ? "void" : Types[type].CSharp;
    }

    /* The return type is the last argument of a C# function pointer type. */
    private static string CSharpFunction(FunctionEntry function)
    {
        List<string> parts = new();
        foreach (string parameter in function.Params!)
        {
            parts.Add(CSharpType(parameter));
        }
        parts.Add(CSharpType(function.Ret!));
        return "delegate* unmanaged<" + string.Join(", ", parts) + ">";
    }

    private static string EmitHeader(string input, List<(string Name, uint Id)> systems,
                                     List<string> handles, List<StructLayout> layouts)
    {
        StringBuilder text = new();
        text.AppendLine($"// Generated by csbind from {input}. Do not edit.");
        text.AppendLine("#ifndef RESONATE_CSHARP_BINDINGS_H");
        text.AppendLine("#define RESONATE_CSHARP_BINDINGS_H");
        text.AppendLine();
        text.AppendLine("#include <cstddef>");
        text.AppendLine("#include <cstdint>");
        text.AppendLine();
        text.AppendLine("namespace resonate::csharp::bindings");
        text.AppendLine("{");
        text.AppendLine();
        foreach ((string name, uint id) in systems)
        {
            text.AppendLine($"inline constexpr std::uint32_t kSystem{name} = {id}U;");
        }
        text.AppendLine($"inline constexpr std::uint32_t kSystemCount = {systems.Count}U;");

        foreach (string handle in handles)
        {
            text.AppendLine();
            /* Two 32-bit halves, so the wrapper keeps the same 4-byte
               alignment as resonate::ecs::Entity and nests in a chunk without
               shifting later fields. */
            text.AppendLine("/* Index and generation, matching resonate::ecs::Entity. */");
            text.AppendLine($"struct {handle}Handle");
            text.AppendLine("{");
            text.AppendLine("    std::uint32_t index;");
            text.AppendLine("    std::uint32_t generation;");
            text.AppendLine("};");
            text.AppendLine($"static_assert(sizeof({handle}Handle) == 8U);");
            text.AppendLine($"static_assert(alignof({handle}Handle) == 4U);");
        }

        foreach (StructLayout layout in layouts)
        {
            text.AppendLine();
            text.AppendLine($"struct {layout.Name}");
            text.AppendLine("{");
            foreach (FieldLayout field in layout.Fields)
            {
                if (field.Function is not null)
                {
                    text.AppendLine($"    {CppType(field.Function.Ret!)} (*{field.Field.Name})(" +
                                    $"{CppParameters(field.Function)});");
                }
                else
                {
                    text.AppendLine($"    {field.Type.Cpp} {field.Field.Name};");
                }
            }
            text.AppendLine("};");
            text.AppendLine();
            foreach (FieldLayout field in layout.Fields)
            {
                text.AppendLine($"static_assert(offsetof({layout.Name}, {field.Field.Name}) == " +
                                $"{field.Offset}U);");
            }
            text.AppendLine($"static_assert(sizeof({layout.Name}) == {layout.Size}U);");
        }

        text.AppendLine();
        text.AppendLine("} // namespace resonate::csharp::bindings");
        text.AppendLine();
        text.AppendLine("#endif // RESONATE_CSHARP_BINDINGS_H");
        return text.ToString();
    }

    private static string EmitCSharp(string input, List<(string Name, uint Id)> systems,
                                     List<string> handles, List<StructLayout> layouts)
    {
        StringBuilder text = new();
        text.AppendLine($"// Generated by csbind from {input}. Do not edit.");
        text.AppendLine();
        text.AppendLine("using System;");
        text.AppendLine("using System.Runtime.InteropServices;");
        text.AppendLine();
        text.AppendLine("namespace Resonate.CSharp.Bindings;");
        text.AppendLine();
        text.AppendLine("public static class Systems");
        text.AppendLine("{");
        foreach ((string name, uint id) in systems)
        {
            text.AppendLine($"    public const uint {name} = {id}U;");
        }
        text.AppendLine($"    public const uint Count = {systems.Count}U;");
        text.AppendLine("}");

        foreach (string handle in handles)
        {
            text.AppendLine();
            text.AppendLine("/* Index and generation, matching resonate::ecs::Entity. */");
            text.AppendLine("[StructLayout(LayoutKind.Sequential)]");
            text.AppendLine($"public readonly struct {handle}Handle");
            text.AppendLine("{");
            text.AppendLine("    public readonly uint Index;");
            text.AppendLine("    public readonly uint Generation;");
            text.AppendLine();
            text.AppendLine($"    public {handle}Handle(uint index, uint generation)");
            text.AppendLine("    {");
            text.AppendLine("        Index = index;");
            text.AppendLine("        Generation = generation;");
            text.AppendLine("    }");
            text.AppendLine("}");
        }

        foreach (StructLayout layout in layouts)
        {
            text.AppendLine();
            text.AppendLine("[StructLayout(LayoutKind.Sequential)]");
            text.AppendLine($"public unsafe struct {layout.Name}");
            text.AppendLine("{");
            foreach (FieldLayout field in layout.Fields)
            {
                if (field.Function is not null)
                {
                    text.AppendLine($"    public {CSharpFunction(field.Function)} " +
                                    $"{PascalCase(field.Field.Name!)};");
                }
                else
                {
                    text.AppendLine($"    public {field.Type.CSharp} {PascalCase(field.Field.Name!)};");
                }
            }
            text.AppendLine("}");
            text.AppendLine();
            text.AppendLine($"public static class {layout.Name}Layout");
            text.AppendLine("{");
            text.AppendLine($"    public const int Size = {layout.Size};");
            foreach (FieldLayout field in layout.Fields)
            {
                text.AppendLine(
                    $"    public const int {PascalCase(field.Field.Name!)}Offset = {field.Offset};");
            }
            text.AppendLine("}");
        }
        return text.ToString();
    }

    /* Rewriting an identical file would move its mtime, and the fixture's
       publish decides staleness by mtime. */
    private static void WriteIfChanged(string path, string contents)
    {
        if (File.Exists(path) && File.ReadAllText(path) == contents)
        {
            return;
        }
        File.WriteAllText(path, contents);
    }
}
