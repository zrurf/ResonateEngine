using System.Collections.Generic;
using System.IO;
using System.Text;
using System.Text.Json;
using Resonate.Tools.Schemac.Schema;

namespace Resonate.Tools.Schemac.Emit;

/* The canonical intermediate: what the declarations mean, as machine-written
   JSON. It is the metadata the editor reads and the reference the generated
   C++ is compared against, and it is produced from the same model as the code,
   so the two cannot drift. Numeric literals stay strings, because JSON numbers
   cannot carry every i64 or f64 exactly. */
internal static class JsonEmitter
{
    /* Bumped when the metadata's shape changes. */
    internal const int Format = 1;

    internal static string Render(ModuleUnit module)
    {
        using MemoryStream stream = new();
        using (Utf8JsonWriter writer = new(stream, new JsonWriterOptions
        {
            Indented = true,
            IndentCharacter = ' ',
            IndentSize = 2,
            NewLine = "\n",
        }))
        {
            writer.WriteStartObject();
            writer.WriteNumber("format", Format);
            writer.WriteString("tool", Schemac.Version);
            writer.WriteString("module", module.Id);

            writer.WriteStartArray("sources");
            foreach (SchemaFile file in module.Files)
            {
                writer.WriteStringValue(PathText.Relative(file.Path));
            }
            writer.WriteEndArray();

            writer.WriteStartArray("types");
            foreach (TypeDecl type in module.Types)
            {
                WriteType(writer, type);
            }
            writer.WriteEndArray();

            writer.WriteEndObject();
        }

        return Encoding.UTF8.GetString(stream.ToArray()) + "\n";
    }

    private static void WriteType(Utf8JsonWriter writer, TypeDecl type)
    {
        writer.WriteStartObject();
        writer.WriteString("kind", Kind(type.Kind));
        writer.WriteString("name", type.Name);
        writer.WriteString("qualifiedName", type.QualifiedName);

        if (type.Kind is TypeKind.Component or TypeKind.Node)
        {
            writer.WriteNumber("version", type.Version);
        }

        if (type.Kind is TypeKind.Enum or TypeKind.Bitmask)
        {
            writer.WriteString("width", type.Width);
        }

        if (type.Kind == TypeKind.Enum)
        {
            writer.WriteStartArray("values");
            foreach (EnumValueDecl value in type.Values)
            {
                writer.WriteStartObject();
                writer.WriteString("name", value.Name);
                writer.WriteString("value", value.Literal);
                writer.WriteEndObject();
            }
            writer.WriteEndArray();
        }

        if (type.Kind == TypeKind.Bitmask)
        {
            writer.WriteStartArray("flags");
            foreach (BitmaskFlagDecl flag in type.Flags)
            {
                writer.WriteStartObject();
                writer.WriteString("name", flag.Name);
                writer.WriteNumber("bit", flag.Bit);
                writer.WriteEndObject();
            }
            writer.WriteEndArray();
        }

        if (type.Layout is Layout layout)
        {
            writer.WriteStartObject("layout");
            writer.WriteNumber("size", layout.Size);
            writer.WriteNumber("align", layout.Align);
            writer.WriteEndObject();
        }

        if (type.Migrations.Count > 0)
        {
            writer.WriteStartArray("migrations");
            foreach (MigrationDecl migration in type.Migrations)
            {
                writer.WriteStartObject();
                writer.WriteNumber("from", migration.From);
                writer.WriteNumber("to", migration.To);
                writer.WriteEndObject();
            }
            writer.WriteEndArray();
        }

        if (type.Fields.Count > 0)
        {
            writer.WriteStartArray("fields");
            foreach (FieldDecl field in type.Fields)
            {
                WriteField(writer, type, field);
            }
            writer.WriteEndArray();
        }

        writer.WriteEndObject();
    }

    private static void WriteField(Utf8JsonWriter writer, TypeDecl owner, FieldDecl field)
    {
        writer.WriteStartObject();
        writer.WriteString("name", field.Name);
        writer.WriteString("type", field.TypeName);

        if (field.Resolved is not null)
        {
            writer.WriteString("declaredType", field.Resolved.QualifiedName);
        }

        /* A node field names a component or another node rather than inlining
           anything, and has no offset. */
        if (owner.IsStorage)
        {
            writer.WriteNumber("offset", field.Offset);
            writer.WriteNumber("size", field.Size);
            writer.WriteNumber("align", field.Align);
        }

        if (field.MinText is not null)
        {
            writer.WriteString("min", field.MinText);
        }
        if (field.MaxText is not null)
        {
            writer.WriteString("max", field.MaxText);
        }
        if (field.DefaultText is not null)
        {
            writer.WriteString("default", field.DefaultText);
        }

        writer.WriteEndObject();
    }

    private static string Kind(TypeKind kind)
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
