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
        text.AppendLine();
        text.AppendLine(CommandsFacade());
        return text.ToString();
    }

    /* Law 2's API shape: structural changes exist only on the frame's command
       channel, and every one records for the sync point. The raw HostApi
       pointers stay reachable at the ABI, but the generated facade is what
       behaviour code is meant to hold; an analyzer flags the raw calls.

       The facade names the contract's HostApi members, so it assumes the
       engine's contract: spawn, add_component, remove_component and despawn. */
    private static string CommandsFacade()
    {
        return """
        /* The frame's command channel: spawn, add, remove and despawn record
           into the buffer the sync point plays, never into live storage. */
        public unsafe struct Commands
        {
            private HostApi* _api;

            public static Commands For(SystemContext* frame)
            {
                return new Commands { _api = (HostApi*)frame->HostApi };
            }

            public bool IsValid => _api != null;

            public bool Spawn(out EntityHandle entity)
            {
                entity = default;
                ulong packed = 0;
                if (_api == null || _api->Spawn((IntPtr)_api, &packed) != 0)
                {
                    return false;
                }
                entity = new EntityHandle((uint)packed, (uint)(packed >> 32));
                return true;
            }

            /* The component name comes from the generated view:
               T.SchemaNameUtf8. */
            public bool Add<T>(EntityHandle entity, ReadOnlySpan<byte> component, in T value)
                where T : unmanaged
            {
                if (_api == null)
                {
                    return false;
                }
                fixed (byte* name = component)
                fixed (T* data = &value)
                {
                    return _api->AddComponent((IntPtr)_api, Pack(entity), name, (IntPtr)data) == 0;
                }
            }

            public bool Remove(EntityHandle entity, ReadOnlySpan<byte> component)
            {
                if (_api == null)
                {
                    return false;
                }
                fixed (byte* name = component)
                {
                    return _api->RemoveComponent((IntPtr)_api, Pack(entity), name) == 0;
                }
            }

            public bool Despawn(EntityHandle entity)
            {
                return _api != null && _api->Despawn((IntPtr)_api, Pack(entity)) == 0;
            }

            /* Another entity's component, read-only by type. The pointer lives
               for the frame; the component name comes from the generated view. */
            public bool Read<T>(EntityHandle entity, ReadOnlySpan<byte> component,
                                out ReadView<T> view) where T : unmanaged
            {
                view = default;
                if (_api == null)
                {
                    return false;
                }
                void* data = null;
                fixed (byte* name = component)
                {
                    if (_api->ReadComponent((IntPtr)_api, Pack(entity), name,
                                            (IntPtr)(&data)) != 0 || data == null)
                    {
                        return false;
                    }
                }
                view = new ReadView<T>((T*)data);
                return true;
            }

            private static ulong Pack(EntityHandle entity)
            {
                return ((ulong)entity.Generation << 32) | entity.Index;
            }
        }

        /* Law 3's read side: the type exposes no writable path, so a view of
           another entity's component cannot be written by accident. */
        public readonly unsafe ref struct ReadView<T> where T : unmanaged
        {
            private readonly T* _data;

            internal ReadView(T* data)
            {
                _data = data;
            }

            public bool IsValid => _data != null;

            public ref readonly T Value => ref *_data;
        }

        /* The stage's chunks, validated once: the name check happens here, so a
           view per chunk costs no boundary work. The whole list arrives in one
           call (the FFI budget is per stage, not per chunk). */
        public readonly unsafe ref struct ChunkList<T> where T : unmanaged
        {
            private readonly ChunkSpan* _spans;
            private readonly int _count;
            private readonly HostApi* _api;

            private ChunkList(ChunkSpan* spans, int count, HostApi* api)
            {
                _spans = spans;
                _count = count;
                _api = api;
            }

            public static bool TryFrom(SystemContext* frame, ReadOnlySpan<byte> component,
                                       out ChunkList<T> list)
            {
                list = default;
                HostApi* api = (HostApi*)frame->HostApi;
                if (api == null || api->ComponentName == null || api->ComponentNameSize == 0)
                {
                    return false;
                }
                ReadOnlySpan<byte> name = new(api->ComponentName, (int)api->ComponentNameSize);
                if (!name.SequenceEqual(component))
                {
                    return false;
                }
                list = new ChunkList<T>((ChunkSpan*)frame->Chunks, (int)frame->ChunkCount, api);
                return true;
            }

            public int Count => _count;

            /* The writable view; it marks its chunk when disposed (law 7). */
            public SelfView<T> Self(int index) => new(_api, _spans[index]);

            /* The swarm lane's read view: no write path, no mark. */
            public ChunkView<T> Read(int index) => new(_spans[index]);
        }

        /* Law 3's self side: one chunk of the list, writable, with the mark on
           dispose. A self view denotes intent to write, so the chunk is marked
           even when the body only read: over-reporting is safe, a missed write
           is not. */
        public unsafe ref struct SelfView<T> where T : unmanaged
        {
            private HostApi* _api;
            private readonly ChunkSpan _span;

            internal SelfView(HostApi* api, in ChunkSpan span)
            {
                _api = api;
                _span = span;
            }

            public Span<T> Items => new((void*)_span.Data, (int)_span.Count);

            public ReadOnlySpan<EntityHandle> Entities =>
                new((void*)_span.Entities, (int)_span.Count);

            public void Dispose()
            {
                if (_api != null)
                {
                    _api->MarkChunkChanged((IntPtr)_api, _span.Entities, _span.Count);
                    _api = null;
                }
            }
        }

        /* The swarm lane's view: a dense read-only pass with no mark, because
           the body cannot write through it. */
        public readonly unsafe ref struct ChunkView<T> where T : unmanaged
        {
            private readonly ChunkSpan _span;

            internal ChunkView(in ChunkSpan span)
            {
                _span = span;
            }

            public ReadOnlySpan<T> Items => new((void*)_span.Data, (int)_span.Count);

            public ReadOnlySpan<EntityHandle> Entities =>
                new((void*)_span.Entities, (int)_span.Count);
        }

        /* The frame's input actions, sampled by the engine's input system. The
           managed side only reads the state; bindings stay an asset. */
        public static unsafe class Actions
        {
            public static bool TryRead(SystemContext* frame, ReadOnlySpan<byte> action,
                                       out ActionState state)
            {
                HostApi* api = (HostApi*)frame->HostApi;
                state = default;
                if (api == null || api->ReadAction == null)
                {
                    return false;
                }
                fixed (byte* name = action)
                fixed (ActionState* statePtr = &state)
                {
                    return api->ReadAction((IntPtr)api, name, (IntPtr)statePtr) == 0;
                }
            }
        }
        """;
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
