using System;
using System.Collections.Generic;
using System.Globalization;
using System.Numerics;
using Resonate.Tools.Schemac.Xml;

namespace Resonate.Tools.Schemac.Schema;

internal sealed class SchemaFile(string path, string module, int moduleLine, int moduleColumn)
{
    internal string Path { get; } = path;
    internal string Module { get; } = module;
    internal int ModuleLine { get; } = moduleLine;
    internal int ModuleColumn { get; } = moduleColumn;
    internal List<TypeDecl> Types { get; } = [];
}

/* Turns one .rschema file into declarations. What is checked here is the shape
   of the declaration — elements, attributes, names; everything that depends on
   another type (resolution, constraints, layouts) is the compiler's pass. */
internal static class SchemaParser
{
    private static readonly string[] SchemaAttributes = ["module"];
    private static readonly string[] EnumAttributes = ["name", "width"];
    private static readonly string[] BitmaskAttributes = ["name", "width"];
    private static readonly string[] StructAttributes = ["name"];
    private static readonly string[] VersionedAttributes = ["name", "version"];
    private static readonly string[] ComponentAttributes = ["name", "version", "derived"];
    private static readonly string[] UseAttributes = ["component", "expose"];
    private static readonly string[] ChildrenAttributes = ["allow"];
    private static readonly string[] ValueAttributes = ["name", "value"];
    private static readonly string[] FlagAttributes = ["name", "bit"];
    private static readonly string[] FieldAttributes = ["name", "type", "min", "max", "default"];
    private static readonly string[] MigrateAttributes = ["from", "to"];

    internal static SchemaFile? Parse(string path, Diagnostics diagnostics)
    {
        XmlElement? root = RestrictedXml.Parse(path, diagnostics);
        if (root is null)
        {
            return null;
        }

        if (root.Name != "schema")
        {
            diagnostics.Error(path, root.Line, root.Column,
                              $"the root element is '{root.Name}'; a schema file's root is 'schema'");
            return null;
        }

        CheckAttributes(root, SchemaAttributes, diagnostics, "the schema root");
        XmlAttribute? module = Required(root, "module", diagnostics, "the schema root");
        if (module is null)
        {
            return null;
        }
        if (!Names.IsModuleId(module.Value))
        {
            diagnostics.Error(path, module.Line, module.Column,
                              $"'{module.Value}' is not a module id: lowercase reverse-DNS with at "
                                  + "least two segments, as in resonate.gameplay");
            return null;
        }

        SchemaFile file = new(path, module.Value, module.Line, module.Column);
        foreach (XmlElement element in root.Children)
        {
            TypeDecl? type = element.Name switch
            {
                "enum" => ParseEnum(element, module.Value, diagnostics),
                "bitmask" => ParseBitmask(element, module.Value, diagnostics),
                "struct" => ParseRecord(element, module.Value, TypeKind.Struct, diagnostics),
                "component" => ParseRecord(element, module.Value, TypeKind.Component, diagnostics),
                "node" => ParseRecord(element, module.Value, TypeKind.Node, diagnostics),
                _ => UnknownElement(element, diagnostics),
            };
            if (type is not null)
            {
                file.Types.Add(type);
            }
        }
        return file;
    }

    private static TypeDecl? UnknownElement(XmlElement element, Diagnostics diagnostics)
    {
        diagnostics.Error(element.File, element.Line, element.Column,
                          $"'{element.Name}' is not a type declaration; a schema declares enum, "
                              + "bitmask, struct, component or node");
        return null;
    }

    private static TypeDecl? ParseEnum(XmlElement element, string module, Diagnostics diagnostics)
    {
        CheckAttributes(element, EnumAttributes, diagnostics, "an enum");
        XmlAttribute? name = Required(element, "name", diagnostics, "an enum");
        XmlAttribute? width = Required(element, "width", diagnostics, "an enum");
        if (name is null || width is null || !CheckTypeName(element, name, diagnostics))
        {
            return null;
        }

        Primitive? underlying = Primitives.Find(width.Value);
        if (underlying is null || !underlying.IsInteger)
        {
            diagnostics.Error(element.File, width.Line, width.Column,
                              $"an enum's width is an integer type (i8..u64); '{width.Value}' is not one");
            return null;
        }
        (BigInteger Min, BigInteger Max) range = Primitives.Range(underlying)!.Value;

        TypeDecl type = new()
        {
            Name = name.Value,
            Module = module,
            Kind = TypeKind.Enum,
            File = element.File,
            Line = element.Line,
            Column = element.Column,
            Width = width.Value,
        };

        BigInteger next = BigInteger.Zero;
        HashSet<string> declared = new(StringComparer.Ordinal);
        foreach (XmlElement child in element.Children)
        {
            if (child.Name != "value")
            {
                diagnostics.Error(child.File, child.Line, child.Column,
                                  $"an enum holds 'value' elements; '{child.Name}' is not one");
                continue;
            }
            CheckAttributes(child, ValueAttributes, diagnostics, "an enum value");
            XmlAttribute? valueName = Required(child, "name", diagnostics, "an enum value");
            if (valueName is null
                || !Names.Check(valueName.Value, "an enum value name", child.File, valueName.Line,
                                valueName.Column, diagnostics))
            {
                continue;
            }
            if (!declared.Add(valueName.Value))
            {
                diagnostics.Error(child.File, valueName.Line, valueName.Column,
                                  $"the enum value '{valueName.Value}' is declared twice");
                continue;
            }

            BigInteger value = next;
            XmlAttribute? explicitValue = child.Attribute("value");
            if (explicitValue is not null && !Literals.TryInteger(explicitValue.Value, out value))
            {
                diagnostics.Error(child.File, explicitValue.Line, explicitValue.Column,
                                  $"'{explicitValue.Value}' is not an integer value for "
                                      + $"'{valueName.Value}'");
                continue;
            }
            if (value < range.Min || value > range.Max)
            {
                diagnostics.Error(child.File, child.Line, child.Column,
                                  $"{Literals.Integer(value)} does not fit the enum's width "
                                      + $"'{width.Value}'");
                continue;
            }

            type.Values.Add(new EnumValueDecl(valueName.Value, Literals.Integer(value), child.Line,
                                              child.Column));
            next = value + BigInteger.One;
        }

        if (type.Values.Count == 0)
        {
            diagnostics.Error(element.File, element.Line, element.Column,
                              $"the enum '{name.Value}' declares no values");
            return null;
        }
        return type;
    }

    private static TypeDecl? ParseBitmask(XmlElement element, string module, Diagnostics diagnostics)
    {
        CheckAttributes(element, BitmaskAttributes, diagnostics, "a bitmask");
        XmlAttribute? name = Required(element, "name", diagnostics, "a bitmask");
        XmlAttribute? width = Required(element, "width", diagnostics, "a bitmask");
        if (name is null || width is null || !CheckTypeName(element, name, diagnostics))
        {
            return null;
        }

        Primitive? underlying = Primitives.Find(width.Value);
        if (underlying is null || !underlying.IsInteger || underlying.Signed)
        {
            diagnostics.Error(element.File, width.Line, width.Column,
                              $"a bitmask's width is an unsigned type (u8..u64); '{width.Value}' is "
                                  + "not one");
            return null;
        }

        TypeDecl type = new()
        {
            Name = name.Value,
            Module = module,
            Kind = TypeKind.Bitmask,
            File = element.File,
            Line = element.Line,
            Column = element.Column,
            Width = width.Value,
        };

        int next = 0;
        HashSet<string> declared = new(StringComparer.Ordinal);
        HashSet<int> used = [];
        foreach (XmlElement child in element.Children)
        {
            if (child.Name != "flag")
            {
                diagnostics.Error(child.File, child.Line, child.Column,
                                  $"a bitmask holds 'flag' elements; '{child.Name}' is not one");
                continue;
            }
            CheckAttributes(child, FlagAttributes, diagnostics, "a bitmask flag");
            XmlAttribute? flagName = Required(child, "name", diagnostics, "a bitmask flag");
            if (flagName is null
                || !Names.Check(flagName.Value, "a bitmask flag name", child.File, flagName.Line,
                                flagName.Column, diagnostics))
            {
                continue;
            }
            if (!declared.Add(flagName.Value))
            {
                diagnostics.Error(child.File, flagName.Line, flagName.Column,
                                  $"the bitmask flag '{flagName.Value}' is declared twice");
                continue;
            }

            int bit = next;
            XmlAttribute? explicitBit = child.Attribute("bit");
            if (explicitBit is not null
                && !int.TryParse(explicitBit.Value.Trim(), NumberStyles.None,
                                 CultureInfo.InvariantCulture, out bit))
            {
                diagnostics.Error(child.File, explicitBit.Line, explicitBit.Column,
                                  $"'{explicitBit.Value}' is not a bit index for '{flagName.Value}'");
                continue;
            }
            if (bit >= underlying.Bits)
            {
                diagnostics.Error(child.File, child.Line, child.Column,
                                  $"bit {bit} is outside the bitmask's width '{width.Value}'");
                continue;
            }
            if (!used.Add(bit))
            {
                diagnostics.Error(child.File, child.Line, child.Column,
                                  $"bit {bit} is already taken in '{name.Value}'");
                continue;
            }

            next = bit + 1;
            type.Flags.Add(new BitmaskFlagDecl(flagName.Value, bit, child.Line, child.Column));
        }

        if (type.Flags.Count == 0)
        {
            diagnostics.Error(element.File, element.Line, element.Column,
                              $"the bitmask '{name.Value}' declares no flags");
            return null;
        }
        return type;
    }

    private static TypeDecl? ParseRecord(XmlElement element, string module, TypeKind kind,
                                         Diagnostics diagnostics)
    {
        string what = kind.Describe();
        string[] attributes = kind switch
        {
            TypeKind.Struct => StructAttributes,
            TypeKind.Component => ComponentAttributes,
            _ => VersionedAttributes,
        };
        CheckAttributes(element, attributes, diagnostics, what);

        XmlAttribute? name = Required(element, "name", diagnostics, what);
        if (name is null || !CheckTypeName(element, name, diagnostics))
        {
            return null;
        }

        TypeDecl type = new()
        {
            Name = name.Value,
            Module = module,
            Kind = kind,
            File = element.File,
            Line = element.Line,
            Column = element.Column,
        };

        if (kind != TypeKind.Struct)
        {
            XmlAttribute? version = Required(element, "version", diagnostics, what);
            if (version is null)
            {
                return null;
            }
            if (!Literals.TryInteger(version.Value, out BigInteger parsed) || parsed < BigInteger.One
                || parsed > int.MaxValue)
            {
                diagnostics.Error(element.File, version.Line, version.Column,
                                  $"the version '{version.Value}' is not an integer of at least 1");
                return null;
            }
            type.Version = (int)parsed;
        }

        if (kind == TypeKind.Component)
        {
            XmlAttribute? derived = element.Attribute("derived");
            if (derived is not null)
            {
                if (derived.Value is "true" or "false")
                {
                    type.Derived = derived.Value == "true";
                }
                else
                {
                    diagnostics.Error(element.File, derived.Line, derived.Column,
                                      $"derived is true or false; '{derived.Value}' is neither");
                }
            }
        }

        HashSet<string> fieldNames = new(StringComparer.Ordinal);
        foreach (XmlElement child in element.Children)
        {
            switch (child.Name)
            {
                case "field":
                {
                    FieldDecl? field = ParseField(child, diagnostics, fieldNames);
                    if (field is not null)
                    {
                        type.Fields.Add(field);
                    }
                    break;
                }
                case "migrate" when kind == TypeKind.Component:
                {
                    MigrationDecl? migration = ParseMigration(child, diagnostics);
                    if (migration is not null)
                    {
                        type.Migrations.Add(migration);
                    }
                    break;
                }
                case "migrate" when kind == TypeKind.Node:
                    diagnostics.Error(child.File, child.Line, child.Column,
                                      "a node type's migrations arrive with the document pipeline; "
                                          + "v0 declares migrations on components only");
                    break;
                case "uses" when kind == TypeKind.Node:
                {
                    NodeUse? use = ParseUse(child, type, diagnostics);
                    if (use is not null)
                    {
                        type.Uses.Add(use);
                    }
                    break;
                }
                case "children" when kind == TypeKind.Node:
                    ParseChildren(child, type, diagnostics);
                    break;
                default:
                    diagnostics.Error(child.File, child.Line, child.Column,
                                      kind == TypeKind.Component
                                          ? $"a component holds 'field' and 'migrate' elements; "
                                                + $"'{child.Name}' is not one"
                                          : kind == TypeKind.Node
                                              ? "a node type holds 'field', 'uses' and 'children' "
                                                    + $"elements; '{child.Name}' is not one"
                                              : $"{what} holds 'field' elements; '{child.Name}' is not one");
                    break;
            }
        }

        if (kind == TypeKind.Component && type.Fields.Count == 0)
        {
            diagnostics.Error(element.File, element.Line, element.Column,
                              $"{what} '{name.Value}' declares no fields");
            return null;
        }
        return type;
    }

    private static NodeUse? ParseUse(XmlElement element, TypeDecl owner,
                                     Diagnostics diagnostics)
    {
        XmlAttribute? component = Required(element, "component", diagnostics, "a uses element");
        if (component is null
            || !Names.Check(component.Value, "a used component name", element.File, component.Line,
                            component.Column, diagnostics))
        {
            return null;
        }
        foreach (NodeUse existing in owner.Uses)
        {
            if (existing.Component == component.Value)
            {
                diagnostics.Error(element.File, component.Line, component.Column,
                                  $"the component '{component.Value}' is used twice");
                return null;
            }
        }

        NodeUse use = new(component.Value, element.Line, element.Column);
        XmlAttribute? expose = element.Attribute("expose");
        if (expose is not null)
        {
            HashSet<string> exposed = new(StringComparer.Ordinal);
            foreach (string field in expose.Value.Split((char[]?)null,
                                                        StringSplitOptions.RemoveEmptyEntries))
            {
                if (!Names.Check(field, "an exposed field name", element.File, expose.Line,
                                 expose.Column, diagnostics))
                {
                    continue;
                }
                if (!exposed.Add(field))
                {
                    diagnostics.Error(element.File, expose.Line, expose.Column,
                                      $"the field '{field}' is exposed twice");
                    continue;
                }
                use.Exposed.Add(field);
            }
        }

        foreach (XmlAttribute attribute in element.Attributes)
        {
            if (attribute.Name is "component" or "expose")
            {
                continue;
            }
            if (!Names.Check(attribute.Name, "a default field name", element.File, attribute.Line,
                             attribute.Column, diagnostics))
            {
                continue;
            }
            use.Defaults.Add(new NodeUseDefault(attribute.Name, attribute.Value, attribute.Line,
                                                attribute.Column));
        }

        if (element.Children.Count != 0)
        {
            diagnostics.Error(element.Children[0].File, element.Children[0].Line,
                              element.Children[0].Column, "<uses> has no content");
            return null;
        }
        return use;
    }

    private static void ParseChildren(XmlElement element, TypeDecl owner,
                                      Diagnostics diagnostics)
    {
        CheckAttributes(element, ChildrenAttributes, diagnostics, "a children element");
        if (owner.ChildAllow is not null)
        {
            diagnostics.Error(element.File, element.Line, element.Column,
                              "a node type declares the children rule once");
            return;
        }
        XmlAttribute? allow = Required(element, "allow", diagnostics, "a children element");
        if (allow is not null)
        {
            owner.ChildAllow = allow.Value;
        }
        if (element.Children.Count != 0)
        {
            diagnostics.Error(element.Children[0].File, element.Children[0].Line,
                              element.Children[0].Column, "<children> has no content");
        }
    }

    private static FieldDecl? ParseField(XmlElement element, Diagnostics diagnostics,
                                         HashSet<string> seen)
    {
        CheckAttributes(element, FieldAttributes, diagnostics, "a field");
        XmlAttribute? name = Required(element, "name", diagnostics, "a field");
        XmlAttribute? typeName = Required(element, "type", diagnostics, "a field");
        if (name is null || typeName is null
            || !Names.Check(name.Value, "a field name", element.File, name.Line, name.Column,
                            diagnostics))
        {
            return null;
        }
        if (!seen.Add(name.Value))
        {
            diagnostics.Error(element.File, name.Line, name.Column,
                              $"the field '{name.Value}' is declared twice");
            return null;
        }
        if (typeName.Value.Trim().Length == 0)
        {
            diagnostics.Error(element.File, typeName.Line, typeName.Column, "the field has no type");
            return null;
        }

        return new FieldDecl(name.Value, typeName.Value.Trim(), element.Line, element.Column)
        {
            Min = Positioned(element.Attribute("min")),
            Max = Positioned(element.Attribute("max")),
            Default = Positioned(element.Attribute("default")),
        };
    }

    private static MigrationDecl? ParseMigration(XmlElement element, Diagnostics diagnostics)
    {
        CheckAttributes(element, MigrateAttributes, diagnostics, "a migration");
        XmlAttribute? from = Required(element, "from", diagnostics, "a migration");
        XmlAttribute? to = Required(element, "to", diagnostics, "a migration");
        if (from is null || to is null)
        {
            return null;
        }

        if (!Literals.TryInteger(from.Value, out BigInteger fromValue) || fromValue < BigInteger.One
            || fromValue > int.MaxValue)
        {
            diagnostics.Error(element.File, from.Line, from.Column,
                              $"the migration's 'from' is a version of at least 1; "
                                  + $"'{from.Value}' is not");
            return null;
        }
        if (!Literals.TryInteger(to.Value, out BigInteger toValue) || toValue < BigInteger.One
            || toValue > int.MaxValue)
        {
            diagnostics.Error(element.File, to.Line, to.Column,
                              $"the migration's 'to' is a version of at least 1; '{to.Value}' is not");
            return null;
        }

        return new MigrationDecl((int)fromValue, (int)toValue, element.Line, element.Column);
    }

    private static AttributeText? Positioned(XmlAttribute? attribute)
    {
        return attribute is null ? null : new AttributeText(attribute.Value, attribute.Line,
                                                            attribute.Column);
    }

    private static bool CheckTypeName(XmlElement element, XmlAttribute name, Diagnostics diagnostics)
    {
        if (!Names.Check(name.Value, "a type name", element.File, name.Line, name.Column, diagnostics))
        {
            return false;
        }
        if (Primitives.IsPrimitiveName(name.Value))
        {
            diagnostics.Error(element.File, name.Line, name.Column,
                              $"'{name.Value}' is a built-in field type; a declared type needs a "
                                  + "name of its own");
            return false;
        }
        return true;
    }

    /* Reports what the element may carry; a misspelled attribute is an error
       rather than something silently ignored, the same stance the formats take
       on unknown TOML keys. */
    private static void CheckAttributes(XmlElement element, string[] known, Diagnostics diagnostics,
                                        string what)
    {
        foreach (XmlAttribute attribute in element.Attributes)
        {
            if (Array.IndexOf(known, attribute.Name) < 0)
            {
                diagnostics.Error(element.File, attribute.Line, attribute.Column,
                                  $"{what} takes {string.Join(", ", known)}; '{attribute.Name}' is "
                                      + "not one of them");
            }
        }
    }

    private static XmlAttribute? Required(XmlElement element, string name, Diagnostics diagnostics,
                                          string what)
    {
        XmlAttribute? attribute = element.Attribute(name);
        if (attribute is null)
        {
            diagnostics.Error(element.File, element.Line, element.Column,
                              $"{what} needs a '{name}' attribute");
        }
        return attribute;
    }
}
