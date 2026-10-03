using System.Collections.Generic;

namespace Resonate.Tools.Schemac.Xml;

/* One attribute of an element, in document order and positioned for
   diagnostics. */
internal sealed record XmlAttribute(string Name, string Value, int Line, int Column);

/* The subset's element tree: a name, its attributes, its child elements. Text
   content that is not whitespace is refused by the reader, so there is none
   here. */
internal sealed class XmlElement(string name, string file, int line, int column)
{
    internal string Name { get; } = name;
    internal string File { get; } = file;
    internal int Line { get; } = line;
    internal int Column { get; } = column;

    internal List<XmlAttribute> Attributes { get; } = [];
    internal List<XmlElement> Children { get; } = [];

    internal XmlAttribute? Attribute(string name)
    {
        foreach (XmlAttribute attribute in Attributes)
        {
            if (attribute.Name == name)
            {
                return attribute;
            }
        }
        return null;
    }
}
