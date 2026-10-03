using System;
using System.Collections.Generic;
using System.Numerics;

namespace Resonate.Tools.Schemac.Schema;

internal sealed class ModuleUnit
{
    internal required string Id { get; init; }
    internal List<SchemaFile> Files { get; } = [];
    internal List<TypeDecl> Types { get; } = [];
    internal Dictionary<string, TypeDecl> ByName { get; } = new(StringComparer.Ordinal);
}

/* The pass between parsing and emitting: groups files by owner module, resolves
   every field against the module's own type table, checks constraints and
   versioning, and freezes the storage layout. Everything it reports is an
   error; the build gate is "any error at all". */
internal static class SchemaCompiler
{
    internal static List<ModuleUnit> Compile(List<SchemaFile> files, Diagnostics diagnostics)
    {
        List<ModuleUnit> modules = [];
        Dictionary<string, ModuleUnit> byId = new(StringComparer.Ordinal);
        foreach (SchemaFile file in files)
        {
            if (!byId.TryGetValue(file.Module, out ModuleUnit? module))
            {
                module = new ModuleUnit { Id = file.Module };
                byId.Add(file.Module, module);
                modules.Add(module);
            }
            module.Files.Add(file);
            module.Types.AddRange(file.Types);
        }

        foreach (ModuleUnit module in modules)
        {
            IndexTypes(module, diagnostics);
            foreach (TypeDecl type in module.Types)
            {
                ResolveFields(module, type, diagnostics);
                CheckVersioning(type, diagnostics);
            }
            foreach (TypeDecl type in module.Types)
            {
                if (type.IsStorage)
                {
                    LayoutOf(type, diagnostics, []);
                }
            }
        }
        return modules;
    }

    /* A type is owned by the module that declares it, so a second declaration of
       one name — in another file of the module or in a second directory claiming
       the same id — is an ownership conflict and stops the build. */
    private static void IndexTypes(ModuleUnit module, Diagnostics diagnostics)
    {
        foreach (TypeDecl type in module.Types)
        {
            if (module.ByName.TryGetValue(type.Name, out TypeDecl? first))
            {
                diagnostics.Error(type.File, type.Line, type.Column,
                                  $"'{type.Name}' is declared twice in module '{module.Id}'; the "
                                      + $"first declaration is at {PathText.Relative(first.File)}:"
                                      + $"{first.Line}");
                continue;
            }
            module.ByName.Add(type.Name, type);
        }
    }

    private static void ResolveFields(ModuleUnit module, TypeDecl type, Diagnostics diagnostics)
    {
        foreach (FieldDecl field in type.Fields)
        {
            if (Primitives.Find(field.TypeName) is Primitive primitive)
            {
                field.Primitive = primitive;
                CheckConstraints(type, field, primitive, null, diagnostics);
                continue;
            }

            if (module.ByName.TryGetValue(field.TypeName, out TypeDecl? resolved))
            {
                /* A storage field inlines its composites; a node type may also
                   name a component (the record carries its payload) or another
                   node type (a reference inside the document). */
                bool composite = resolved.Kind is TypeKind.Component or TypeKind.Node;
                if (composite && type.Kind != TypeKind.Node)
                {
                    diagnostics.Error(type.File, field.Line, field.Column,
                                      $"'{field.TypeName}' is {resolved.Kind.Describe()}; a storage "
                                          + "field inlines a struct, an enum or a bitmask");
                    continue;
                }
                field.Resolved = resolved;
                CheckConstraints(type, field, null, resolved, diagnostics);
                continue;
            }

            ReportUnknownType(type, field, diagnostics);
        }
    }

    /* Names the design has and this compiler does not, so the failure says what
       is missing instead of "unknown type". */
    private static void ReportUnknownType(TypeDecl owner, FieldDecl field, Diagnostics diagnostics)
    {
        string name = field.TypeName;
        string? missing = name switch
        {
            "string" => "variable-length text belongs to the document family; a storage field is "
                        + "fixed-size",
            _ when name.StartsWith("asset:", StringComparison.Ordinal) => "asset references arrive "
                                                                          + "with the asset registry's GUID",
            _ when name.Contains('[') => "fixed arrays are not implemented yet",
            _ => null,
        };

        string message = missing is null
            ? $"'{name}' is not a declared type in module '{owner.Module}'"
            : $"'{name}' is not supported yet: {missing}";
        diagnostics.Error(owner.File, field.Line, field.Column, message);
    }

    private static void CheckConstraints(TypeDecl owner, FieldDecl field, Primitive? primitive,
                                         TypeDecl? resolved, Diagnostics diagnostics)
    {
        if (primitive is not null)
        {
            switch (primitive.Kind)
            {
                case PrimitiveKind.Integer:
                case PrimitiveKind.Float:
                    CheckNumeric(owner, field, primitive, diagnostics);
                    break;

                case PrimitiveKind.Boolean:
                    if (field.Min is not null || field.Max is not null)
                    {
                        diagnostics.Error(owner.File, field.Line, field.Column,
                                          "a bool field takes no min or max");
                    }
                    if (field.Default is not null)
                    {
                        string value = field.Default.Value.Trim();
                        if (value is not ("true" or "false"))
                        {
                            diagnostics.Error(owner.File, field.Default.Line, field.Default.Column,
                                              $"'{field.Default.Value}' is not a bool; the default is "
                                                  + "true or false");
                        }
                        else
                        {
                            field.DefaultText = value;
                        }
                    }
                    break;

                case PrimitiveKind.Blob:
                    if (field.Min is not null || field.Max is not null || field.Default is not null)
                    {
                        diagnostics.Error(owner.File, field.Line, field.Column,
                                          "a blob field takes no min, max or default; the blob is "
                                              + "the variable part");
                    }
                    break;

                case PrimitiveKind.Math:
                    CheckMath(owner, field, primitive, diagnostics);
                    break;

                default:
                    if (field.Min is not null || field.Max is not null || field.Default is not null)
                    {
                        diagnostics.Error(owner.File, field.Line, field.Column,
                                          "an entity field takes no min, max or default; its zero is "
                                              + "the invalid handle");
                    }
                    break;
            }
            return;
        }

        if (resolved is null)
        {
            return;
        }

        if (field.Min is not null || field.Max is not null)
        {
            diagnostics.Error(owner.File, field.Min?.Line ?? field.Line,
                              field.Min?.Column ?? field.Column,
                              $"'{field.TypeName}' is not numeric; min and max do not apply");
        }

        if (field.Default is null)
        {
            return;
        }

        string wanted = field.Default.Value.Trim();
        switch (resolved.Kind)
        {
            case TypeKind.Enum:
                if (ContainsValue(resolved, wanted))
                {
                    field.DefaultText = wanted;
                }
                else
                {
                    diagnostics.Error(owner.File, field.Default.Line, field.Default.Column,
                                      $"'{wanted}' is not a value of the enum '{resolved.Name}'");
                }
                break;

            case TypeKind.Bitmask:
                if (ContainsFlag(resolved, wanted))
                {
                    field.DefaultText = wanted;
                }
                else if (Literals.TryInteger(wanted, out BigInteger bits) && bits >= BigInteger.Zero
                         && bits <= Primitives.Range(Primitives.Find(resolved.Width)!)!.Value.Max)
                {
                    field.DefaultText = Literals.Integer(bits);
                }
                else
                {
                    diagnostics.Error(owner.File, field.Default.Line, field.Default.Column,
                                      $"'{wanted}' is neither a flag of '{resolved.Name}' nor a "
                                          + $"literal that fits '{resolved.Width}'");
                }
                break;

            default:
                diagnostics.Error(owner.File, field.Default.Line, field.Default.Column,
                                  $"'{field.TypeName}' is {resolved.Kind.Describe()}; a composite "
                                      + "field has no scalar default");
                break;
        }
    }

    /* A math field carries one number per component as its default ("0 0 0 1"),
       all canonicalised the way a float default is. min/max do not order
       vectors, and a mat4 literal would stop being readable. */
    private static void CheckMath(TypeDecl owner, FieldDecl field, Primitive primitive,
                                  Diagnostics diagnostics)
    {
        if (field.Min is not null || field.Max is not null)
        {
            diagnostics.Error(owner.File, field.Min?.Line ?? field.Line,
                              field.Min?.Column ?? field.Column,
                              $"a {primitive.Name} field takes no min or max");
        }
        if (field.Default is null)
        {
            return;
        }

        int components = primitive.Components;
        if (components == 0)
        {
            diagnostics.Error(owner.File, field.Default.Line, field.Default.Column,
                              $"a {primitive.Name} field takes no default");
            return;
        }

        string[] parts = field.Default.Value.Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries);
        if (parts.Length != components)
        {
            diagnostics.Error(owner.File, field.Default.Line, field.Default.Column,
                              $"a {primitive.Name} default is one number per component "
                                  + $"({components}), got {parts.Length}");
            return;
        }

        List<string> canonical = [];
        foreach (string part in parts)
        {
            if (!Literals.TryFloat(part, out double parsed))
            {
                diagnostics.Error(owner.File, field.Default.Line, field.Default.Column,
                                  $"'{part}' is not a number for the {primitive.Name} default");
                return;
            }
            if (Math.Abs(parsed) > float.MaxValue)
            {
                diagnostics.Error(owner.File, field.Default.Line, field.Default.Column,
                                  $"'{part}' is beyond the range of f32");
                return;
            }
            canonical.Add(Literals.Float(parsed));
        }
        field.DefaultText = string.Join(' ', canonical);
    }

    private static void CheckNumeric(TypeDecl owner, FieldDecl field, Primitive primitive,
                                     Diagnostics diagnostics)
    {
        bool isFloat = primitive.IsFloat;
        if (isFloat)
        {
            double min = double.NegativeInfinity;
            double max = double.PositiveInfinity;
            if (!ReadFloat(owner, field.Min, "min", primitive, ref min, diagnostics)
                || !ReadFloat(owner, field.Max, "max", primitive, ref max, diagnostics))
            {
                return;
            }
            if (min > max)
            {
                diagnostics.Error(owner.File, field.Max!.Line, field.Max.Column,
                                  $"the min {Literals.Float(min)} is above the max "
                                      + $"{Literals.Float(max)}");
            }
            if (field.Min is not null)
            {
                field.MinText = Literals.Float(min);
            }
            if (field.Max is not null)
            {
                field.MaxText = Literals.Float(max);
            }
            if (field.Default is not null)
            {
                double value = 0.0;
                if (!ReadFloat(owner, field.Default, "default", primitive, ref value, diagnostics))
                {
                    return;
                }
                if (value < min || value > max)
                {
                    diagnostics.Error(owner.File, field.Default.Line, field.Default.Column,
                                      $"the default {Literals.Float(value)} is outside "
                                          + "[{min}, {max}]");
                }
                else
                {
                    field.DefaultText = Literals.Float(value);
                }
            }
            return;
        }

        (BigInteger Min, BigInteger Max) range = Primitives.Range(primitive)!.Value;
        BigInteger minInteger = range.Min;
        BigInteger maxInteger = range.Max;
        if (!ReadInteger(owner, field.Min, "min", range, ref minInteger, diagnostics)
            || !ReadInteger(owner, field.Max, "max", range, ref maxInteger, diagnostics))
        {
            return;
        }
        if (minInteger > maxInteger)
        {
            diagnostics.Error(owner.File, field.Max!.Line, field.Max.Column,
                              $"the min {Literals.Integer(minInteger)} is above the max "
                                  + $"{Literals.Integer(maxInteger)}");
        }
        if (field.Min is not null)
        {
            field.MinText = Literals.Integer(minInteger);
        }
        if (field.Max is not null)
        {
            field.MaxText = Literals.Integer(maxInteger);
        }
        if (field.Default is not null)
        {
            BigInteger value = BigInteger.Zero;
            if (!ReadInteger(owner, field.Default, "default", range, ref value, diagnostics))
            {
                return;
            }
            if (value < minInteger || value > maxInteger)
            {
                diagnostics.Error(owner.File, field.Default.Line, field.Default.Column,
                                  $"the default {Literals.Integer(value)} is outside "
                                      + $"[{Literals.Integer(minInteger)}, "
                                      + $"{Literals.Integer(maxInteger)}]");
            }
            else
            {
                field.DefaultText = Literals.Integer(value);
            }
        }
    }

    /* `value` carries the bound in effect when the attribute is absent, so an
       unwritten min stays at the type's own minimum. */
    private static bool ReadInteger(TypeDecl owner, AttributeText? attribute, string what,
                                    (BigInteger Min, BigInteger Max) range, ref BigInteger value,
                                    Diagnostics diagnostics)
    {
        if (attribute is null)
        {
            return true;
        }
        if (!Literals.TryInteger(attribute.Value, out BigInteger parsed))
        {
            diagnostics.Error(owner.File, attribute.Line, attribute.Column,
                              $"'{attribute.Value}' is not an integer {what}");
            return false;
        }
        if (parsed < range.Min || parsed > range.Max)
        {
            diagnostics.Error(owner.File, attribute.Line, attribute.Column,
                              $"the {what} {Literals.Integer(parsed)} does not fit the field's type");
            return false;
        }
        value = parsed;
        return true;
    }

    private static bool ReadFloat(TypeDecl owner, AttributeText? attribute, string what,
                                  Primitive primitive, ref double value, Diagnostics diagnostics)
    {
        if (attribute is null)
        {
            return true;
        }
        if (!Literals.TryFloat(attribute.Value, out double parsed))
        {
            diagnostics.Error(owner.File, attribute.Line, attribute.Column,
                              $"'{attribute.Value}' is not a number for {what}");
            return false;
        }
        if (primitive.Bits == 32 && Math.Abs(parsed) > float.MaxValue)
        {
            diagnostics.Error(owner.File, attribute.Line, attribute.Column,
                              $"the {what} {Literals.Float(parsed)} is beyond the range of f32");
            return false;
        }
        value = parsed;
        return true;
    }

    /* A version above 1 is only meaningful with a migration path to it: the
       chain has to ascend, connect, and end at the current version, or old
       documents and saves would have no way in. */
    private static void CheckVersioning(TypeDecl type, Diagnostics diagnostics)
    {
        if (type.Version <= 1)
        {
            if (type.Migrations.Count > 0)
            {
                diagnostics.Error(type.File, type.Line, type.Column,
                                  $"'{type.Name}' is at version {type.Version} and declares "
                                      + "migrations; there is nothing below it to migrate from");
            }
            return;
        }

        if (type.Migrations.Count == 0)
        {
            diagnostics.Error(type.File, type.Line, type.Column,
                              $"'{type.Name}' is at version {type.Version} but declares no "
                                  + "migrations; a version above 1 is only meaningful with a path "
                                  + "from an older one");
            return;
        }

        List<MigrationDecl> ordered = [.. type.Migrations];
        ordered.Sort(static (left, right) => left.From.CompareTo(right.From));

        bool connected = true;
        foreach (MigrationDecl migration in ordered)
        {
            if (migration.From >= migration.To)
            {
                diagnostics.Error(type.File, migration.Line, migration.Column,
                                  $"a migration goes from {migration.From} to {migration.To}; "
                                      + "'to' is the later version");
                connected = false;
            }
        }
        for (int index = 1; index < ordered.Count; ++index)
        {
            if (ordered[index].From != ordered[index - 1].To)
            {
                diagnostics.Error(type.File, ordered[index].Line, ordered[index].Column,
                                  $"a migration starts at {ordered[index].From} but the previous one "
                                      + $"ends at {ordered[index - 1].To}; the chain has a gap");
                connected = false;
            }
        }
        if (connected && ordered[^1].To != type.Version)
        {
            diagnostics.Error(type.File, ordered[^1].Line, ordered[^1].Column,
                              $"the migration chain ends at version {ordered[^1].To} but "
                                  + $"'{type.Name}' is at version {type.Version}");
        }
    }

    private static bool ContainsValue(TypeDecl type, string name)
    {
        foreach (EnumValueDecl value in type.Values)
        {
            if (value.Name == name)
            {
                return true;
            }
        }
        return false;
    }

    private static bool ContainsFlag(TypeDecl type, string name)
    {
        foreach (BitmaskFlagDecl flag in type.Flags)
        {
            if (flag.Name == name)
            {
                return true;
            }
        }
        return false;
    }

    /* Natural alignment: a field starts at the next multiple of its own
       alignment, a record is aligned to its largest field, and its size is
       padded to that multiple. The generated header pins these numbers with
       static_asserts, so a compiler that lays the same declaration out
       differently fails the build instead of shifting bytes silently. */
    private static Layout? LayoutOf(TypeDecl type, Diagnostics diagnostics, List<TypeDecl> inProgress)
    {
        if (type.Layout is not null)
        {
            return type.Layout;
        }

        if (type.Kind == TypeKind.Enum || type.Kind == TypeKind.Bitmask)
        {
            Primitive width = Primitives.Find(type.Width)!;
            type.Layout = new Layout(width.Size, width.Align);
            return type.Layout;
        }

        if (inProgress.Contains(type))
        {
            List<string> chain = [];
            foreach (TypeDecl visited in inProgress)
            {
                chain.Add(visited.Name);
            }
            chain.Add(type.Name);
            diagnostics.Error(type.File, type.Line, type.Column,
                              $"the struct '{type.Name}' is inlined into itself: "
                                  + string.Join(" -> ", chain));
            return null;
        }

        inProgress.Add(type);
        int offset = 0;
        int align = 1;
        foreach (FieldDecl field in type.Fields)
        {
            (int size, int fieldAlign) = FieldLayout(type, field, diagnostics, inProgress);
            field.Size = size;
            field.Align = fieldAlign;
            field.Offset = RoundUp(offset, fieldAlign);
            offset = field.Offset + size;
            align = Math.Max(align, fieldAlign);
        }
        inProgress.RemoveAt(inProgress.Count - 1);

        type.Layout = new Layout(RoundUp(offset, align), align);
        return type.Layout;
    }

    private static (int Size, int Align) FieldLayout(TypeDecl owner, FieldDecl field,
                                                     Diagnostics diagnostics,
                                                     List<TypeDecl> inProgress)
    {
        if (field.Primitive is not null)
        {
            return (field.Primitive.Size, field.Primitive.Align);
        }

        TypeDecl? resolved = field.Resolved;
        if (resolved is null)
        {
            /* Unresolved because the field's own error was already reported. */
            return (0, 1);
        }

        Layout? layout = LayoutOf(resolved, diagnostics, inProgress);
        return layout is null ? (0, 1) : (layout.Size, layout.Align);
    }

    private static int RoundUp(int value, int alignment)
    {
        return alignment <= 1 ? value : (value + alignment - 1) / alignment * alignment;
    }
}
