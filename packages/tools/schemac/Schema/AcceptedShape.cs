using System;
using System.Collections.Generic;
using System.Text;

namespace Resonate.Tools.Schemac.Schema;

/* The canonical description of a versioned type and everything inlined into it,
   as the version lock records it: a flat list of lines. Two runs therefore
   compare by string equality, a git diff of the lock reads as "what changed",
   and a mismatch can name the first line that differs.

   Helper types (enum, bitmask, struct) have no version of their own; they are
   part of the components that inline them, so a change anywhere inside one is a
   change to the versioned type's accepted shape. */
internal static class AcceptedShape
{
    internal static List<string> Lines(TypeDecl type)
    {
        List<string> lines = [$"{Word(type.Kind)} {type.Name} version {type.Version}"];
        if (type.Layout is Layout layout)
        {
            lines.Add($"  size {layout.Size} align {layout.Align}");
        }
        foreach (FieldDecl field in type.Fields)
        {
            lines.Add("  " + FieldLine(field, type.Kind));
        }

        SortedDictionary<string, TypeDecl> inlined = new(StringComparer.Ordinal);
        foreach (FieldDecl field in type.Fields)
        {
            Collect(field.Resolved, inlined);
        }
        foreach (KeyValuePair<string, TypeDecl> entry in inlined)
        {
            lines.AddRange(DefinitionLines(entry.Value));
        }
        return lines;
    }

    private static void Collect(TypeDecl? declared, SortedDictionary<string, TypeDecl> into)
    {
        /* A component or node field holds a reference, not a payload: its own
           version covers it. */
        if (declared is null || declared.Kind is TypeKind.Component or TypeKind.Node
            || into.ContainsKey(declared.Name))
        {
            return;
        }

        into.Add(declared.Name, declared);
        foreach (FieldDecl field in declared.Fields)
        {
            Collect(field.Resolved, into);
        }
    }

    private static IEnumerable<string> DefinitionLines(TypeDecl type)
    {
        switch (type.Kind)
        {
            case TypeKind.Enum:
            {
                StringBuilder values = new();
                foreach (EnumValueDecl value in type.Values)
                {
                    values.Append(values.Length > 0 ? ", " : "").Append(value.Name).Append('=')
                        .Append(value.Literal);
                }
                yield return $"  enum {type.Name} {type.Width} values {values}";
                break;
            }

            case TypeKind.Bitmask:
            {
                StringBuilder flags = new();
                foreach (BitmaskFlagDecl flag in type.Flags)
                {
                    flags.Append(flags.Length > 0 ? ", " : "").Append(flag.Name)
                        .Append("=1<<").Append(flag.Bit);
                }
                yield return $"  bitmask {type.Name} {type.Width} flags {flags}";
                break;
            }

            default:
            {
                Layout layout = type.Layout!;
                yield return $"  struct {type.Name} size {layout.Size} align {layout.Align}";
                foreach (FieldDecl field in type.Fields)
                {
                    yield return "    " + FieldLine(field, TypeKind.Struct);
                }
                break;
            }
        }
    }

    private static string FieldLine(FieldDecl field, TypeKind owner)
    {
        StringBuilder text = new();
        text.Append("field ").Append(field.Name).Append(' ');
        if (field.Resolved is TypeDecl declared)
        {
            /* Qualified, so a field naming a type is distinct from one naming a
               built-in of the same spelling. */
            text.Append(declared.QualifiedName);
        }
        else
        {
            text.Append(field.TypeName);
        }

        if (owner.IsStorage())
        {
            text.Append(" @").Append(field.Offset).Append(" size ").Append(field.Size);
        }
        else if (field.Resolved is TypeDecl referenced)
        {
            text.Append(" -> ").Append(Word(referenced.Kind)).Append(' ')
                .Append(referenced.QualifiedName);
        }

        if (field.MinText is not null)
        {
            text.Append(" min=").Append(field.MinText);
        }
        if (field.MaxText is not null)
        {
            text.Append(" max=").Append(field.MaxText);
        }
        if (field.DefaultText is not null)
        {
            text.Append(" default=").Append(field.DefaultText);
        }
        return text.ToString();
    }

    internal static string Word(TypeKind kind)
    {
        return kind switch
        {
            TypeKind.Enum => "enum",
            TypeKind.Bitmask => "bitmask",
            TypeKind.Struct => "struct",
            TypeKind.Component => "component",
            _ => "node",
        };
    }
}
