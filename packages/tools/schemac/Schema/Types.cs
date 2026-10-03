using System;
using System.Collections.Generic;
using System.Numerics;

namespace Resonate.Tools.Schemac.Schema;

internal enum TypeKind
{
    Enum,
    Bitmask,
    Struct,
    Component,
    Node,
}

/* The two families the design splits types into. The storage family lays out
   inside archetype chunks; the document family is the unified document model's
   node vocabulary. v0 checks both and lays out storage types only. */
internal static class TypeKinds
{
    internal static bool IsStorage(this TypeKind kind)
    {
        return kind is TypeKind.Enum or TypeKind.Bitmask or TypeKind.Struct or TypeKind.Component;
    }

    internal static string Describe(this TypeKind kind)
    {
        return kind switch
        {
            TypeKind.Enum => "an enum",
            TypeKind.Bitmask => "a bitmask",
            TypeKind.Struct => "a struct",
            TypeKind.Component => "a component",
            _ => "a node type",
        };
    }
}

internal enum PrimitiveKind
{
    Integer,
    Float,
    Boolean,
    Entity,
    Blob,
    Math,
}

/* A leaf type a field can be written in: the fixed integers, the two floats,
   bool, the entity and blob handles, and the core math types. Entity and blob
   are here because their sizes and alignments are part of every layout a
   component has. */
internal sealed record Primitive(string Name, PrimitiveKind Kind, int Bits, int Size, int Align,
                                 bool Signed, string Cpp)
{
    internal bool IsInteger => Kind == PrimitiveKind.Integer;
    internal bool IsFloat => Kind == PrimitiveKind.Float;
    internal bool IsNumeric => IsInteger || IsFloat;

    /* How many numbers a math value's default takes, one per component. A mat4
       is too large for a one-line literal to stay readable, so it takes none. */
    internal int Components => Kind == PrimitiveKind.Math && Size <= 16 ? Size / 4 : 0;
}

internal static class Primitives
{
    private static readonly Dictionary<string, Primitive> ByName = new(StringComparer.Ordinal)
    {
        ["i8"] = new("i8", PrimitiveKind.Integer, 8, 1, 1, true, "std::int8_t"),
        ["i16"] = new("i16", PrimitiveKind.Integer, 16, 2, 2, true, "std::int16_t"),
        ["i32"] = new("i32", PrimitiveKind.Integer, 32, 4, 4, true, "std::int32_t"),
        ["i64"] = new("i64", PrimitiveKind.Integer, 64, 8, 8, true, "std::int64_t"),
        ["u8"] = new("u8", PrimitiveKind.Integer, 8, 1, 1, false, "std::uint8_t"),
        ["u16"] = new("u16", PrimitiveKind.Integer, 16, 2, 2, false, "std::uint16_t"),
        ["u32"] = new("u32", PrimitiveKind.Integer, 32, 4, 4, false, "std::uint32_t"),
        ["u64"] = new("u64", PrimitiveKind.Integer, 64, 8, 8, false, "std::uint64_t"),
        ["f32"] = new("f32", PrimitiveKind.Float, 32, 4, 4, true, "float"),
        ["f64"] = new("f64", PrimitiveKind.Float, 64, 8, 8, true, "double"),
        ["bool"] = new("bool", PrimitiveKind.Boolean, 8, 1, 1, false, "bool"),
        /* Handles to state outside the chunk; the generated header asserts the
           two uint32 members these sizes stand for. */
        ["entity"] = new("entity", PrimitiveKind.Entity, 64, 8, 4, false,
                         "resonate::ecs::Entity"),
        ["blob"] = new("blob", PrimitiveKind.Blob, 64, 8, 4, false,
                       "resonate::ecs::BlobHandle"),
        /* The core math types, natural alignment; the generated header's layout
           assertions check the sizes against the C++ structs they name. */
        ["vec2"] = new("vec2", PrimitiveKind.Math, 0, 8, 4, true, "resonate::Vec2"),
        ["vec3"] = new("vec3", PrimitiveKind.Math, 0, 12, 4, true, "resonate::Vec3"),
        ["vec4"] = new("vec4", PrimitiveKind.Math, 0, 16, 4, true, "resonate::Vec4"),
        ["quat"] = new("quat", PrimitiveKind.Math, 0, 16, 4, true, "resonate::Quat"),
        ["mat4"] = new("mat4", PrimitiveKind.Math, 0, 64, 4, true, "resonate::Mat4"),
    };

    internal static Primitive? Find(string name)
    {
        return ByName.GetValueOrDefault(name);
    }

    internal static bool IsPrimitiveName(string name)
    {
        return ByName.ContainsKey(name);
    }

    /* The inclusive range an integer primitive's values live in; null for the
       kinds that hold no numbers. */
    internal static (BigInteger Min, BigInteger Max)? Range(Primitive primitive)
    {
        if (!primitive.IsInteger)
        {
            return null;
        }

        BigInteger max = (BigInteger.One << primitive.Bits) - 1;
        if (primitive.Signed)
        {
            BigInteger magnitude = BigInteger.One << (primitive.Bits - 1);
            return (-magnitude, magnitude - 1);
        }
        return (BigInteger.Zero, max);
    }
}

internal sealed class EnumValueDecl(string name, string literal, int line, int column)
{
    internal string Name { get; } = name;

    /* Canonical decimal text: the same declaration always writes the same
       bytes, and nothing downstream has to re-parse an arbitrary form. */
    internal string Literal { get; } = literal;
    internal int Line { get; } = line;
    internal int Column { get; } = column;
}

internal sealed class BitmaskFlagDecl(string name, int bit, int line, int column)
{
    internal string Name { get; } = name;
    internal int Bit { get; } = bit;
    internal int Line { get; } = line;
    internal int Column { get; } = column;
}

internal sealed class MigrationDecl(int from, int to, int line, int column)
{
    internal int From { get; } = from;
    internal int To { get; } = to;
    internal int Line { get; } = line;
    internal int Column { get; } = column;
}

/* An attribute as written, with its position: whoever reads it reports there
   rather than at the element that carries it. */
internal sealed record AttributeText(string Value, int Line, int Column);

internal sealed class FieldDecl(string name, string typeName, int line, int column)
{
    internal string Name { get; } = name;
    internal string TypeName { get; } = typeName;
    internal int Line { get; } = line;
    internal int Column { get; } = column;

    /* As written; canonicalised and checked once the type is resolved. */
    internal AttributeText? Min { get; set; }
    internal AttributeText? Max { get; set; }
    internal AttributeText? Default { get; set; }

    /* Canonical text, for the outputs; null when the attribute was not written. */
    internal string? MinText { get; set; }
    internal string? MaxText { get; set; }
    internal string? DefaultText { get; set; }

    internal Primitive? Primitive { get; set; }
    internal TypeDecl? Resolved { get; set; }

    /* Filled by the storage layout pass. */
    internal int Offset { get; set; }
    internal int Size { get; set; }
    internal int Align { get; set; }
}

internal sealed record Layout(int Size, int Align);

internal sealed class TypeDecl
{
    internal required string Name { get; init; }
    internal required string Module { get; init; }
    internal required TypeKind Kind { get; init; }
    internal required string File { get; init; }
    internal required int Line { get; init; }
    internal required int Column { get; init; }

    /* The name a type is addressed by outside C++: module-qualified, so two
       modules may own a "Health" each and they stay distinct components. */
    internal string QualifiedName => $"{Module}.{Name}";

    internal bool IsStorage => Kind.IsStorage();

    /* Enum and bitmask width ("u8", "u32"). */
    internal string Width { get; set; } = "";

    internal List<EnumValueDecl> Values { get; } = [];
    internal List<BitmaskFlagDecl> Flags { get; } = [];

    internal List<FieldDecl> Fields { get; } = [];

    /* Components and node types carry a version; the migrations between
       versions are declared on components. */
    internal int Version { get; set; }
    internal List<MigrationDecl> Migrations { get; } = [];

    /* Filled for the storage family once size, alignment and offsets are known. */
    internal Layout? Layout { get; set; }
}
