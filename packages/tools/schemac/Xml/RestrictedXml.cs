using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using System.Xml;

namespace Resonate.Tools.Schemac.Xml;

/* Reads a hand-written engine document under the XML subset every engine
   reader implements: BOM-free UTF-8, elements, attributes and whitespace,
   with no DTD, no entities beyond the five predefined ones, no CDATA, no
   processing instructions and no namespaces. The rejections are the point: a
   construct the canonical writer cannot re-emit has no business in a source
   file, and a DTD is what makes XXE reachable. */
internal static class RestrictedXml
{
    internal static XmlElement? Parse(string path, Diagnostics diagnostics)
    {
        byte[] bytes;
        try
        {
            bytes = File.ReadAllBytes(path);
        }
        catch (IOException error)
        {
            diagnostics.Error(path, 0, 0, $"cannot read the file: {error.Message}");
            return null;
        }

        if (bytes.Length >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF)
        {
            diagnostics.Error(path, 1, 1, "the file starts with a UTF-8 byte order mark; the subset is "
                                              + "BOM-free UTF-8");
            return null;
        }

        string text;
        try
        {
            /* Strict decoding, so a byte sequence that is not UTF-8 is reported
               here rather than turned into replacement characters. */
            text = new UTF8Encoding(false, true).GetString(bytes);
        }
        catch (DecoderFallbackException)
        {
            diagnostics.Error(path, 0, 0, "the file is not valid UTF-8");
            return null;
        }

        XmlReaderSettings settings = new()
        {
            ConformanceLevel = ConformanceLevel.Document,
            DtdProcessing = DtdProcessing.Prohibit,
            XmlResolver = null,
            CheckCharacters = true,
        };

        List<XmlElement> open = [];
        XmlElement? root = null;
        try
        {
            using XmlReader reader = XmlReader.Create(new StringReader(text), settings);
            while (reader.Read())
            {
                (int line, int column) = PositionOf(reader);

                switch (reader.NodeType)
                {
                    case XmlNodeType.XmlDeclaration:
                        CheckDeclaration(reader, path, line, column, diagnostics);
                        break;

                    case XmlNodeType.Element:
                        if (reader.Prefix.Length > 0)
                        {
                            diagnostics.Error(path, line, column,
                                              "namespaces are not part of the XML subset; names are plain");
                            return null;
                        }

                        XmlElement element = new(reader.LocalName, path, line, column);
                        if (!ReadAttributes(reader, element, path, diagnostics))
                        {
                            return null;
                        }

                        if (open.Count == 0)
                        {
                            root = element;
                        }
                        else
                        {
                            open[^1].Children.Add(element);
                        }

                        if (!reader.IsEmptyElement)
                        {
                            open.Add(element);
                        }
                        break;

                    case XmlNodeType.EndElement:
                        open.RemoveAt(open.Count - 1);
                        break;

                    case XmlNodeType.Text:
                    case XmlNodeType.Whitespace:
                    case XmlNodeType.SignificantWhitespace:
                        if (reader.Value.Trim().Length > 0)
                        {
                            diagnostics.Error(path, line, column,
                                              "content text is not part of the XML subset; a declaration "
                                                  + "holds elements and attributes only");
                            return null;
                        }
                        break;

                    case XmlNodeType.Comment:
                        break;

                    case XmlNodeType.CDATA:
                        diagnostics.Error(path, line, column,
                                          "CDATA is not part of the XML subset; write the text directly");
                        return null;

                    case XmlNodeType.ProcessingInstruction:
                        diagnostics.Error(path, line, column,
                                          "processing instructions are not part of the XML subset");
                        return null;

                    default:
                        diagnostics.Error(path, line, column,
                                          $"'{reader.Name}' is not part of the XML subset");
                        return null;
                }
            }
        }
        catch (XmlException error)
        {
            diagnostics.Error(path, error.LineNumber, error.LinePosition, Describe(error));
            return null;
        }

        if (root is null)
        {
            diagnostics.Error(path, 0, 0, "the file has no root element");
            return null;
        }
        return root;
    }

    /* The reader reports positions when the underlying implementation can; the
       string-backed reader used here always can. */
    private static (int Line, int Column) PositionOf(XmlReader reader)
    {
        IXmlLineInfo? info = reader as IXmlLineInfo;
        return info is not null && info.HasLineInfo() ? (info.LineNumber, info.LinePosition) : (0, 0);
    }

    /* The BCL appends "Line n, position m" to its syntax errors, which the
       diagnostic prefix already carries, and its DTD text is a sentence about
       XmlReaderSettings rather than about the document. */
    private static string Describe(XmlException error)
    {
        if (error.Message.Contains("DTD is prohibited", StringComparison.Ordinal))
        {
            return "a DTD is not part of the XML subset";
        }
        if (error.Message.Contains("undeclared prefix", StringComparison.Ordinal))
        {
            return "namespaces are not part of the XML subset; names are plain";
        }

        int trailer = error.Message.IndexOf(" Line ", StringComparison.Ordinal);
        return trailer < 0 ? error.Message : error.Message[..trailer];
    }

    /* True when the element's attributes were read; false aborts the file. */
    private static bool ReadAttributes(XmlReader reader, XmlElement element, string path,
                                       Diagnostics diagnostics)
    {
        for (int index = 0; index < reader.AttributeCount; ++index)
        {
            reader.MoveToAttribute(index);
            (int line, int column) = PositionOf(reader);
            if (reader.Name == "xmlns" || reader.Name.StartsWith("xmlns:", StringComparison.Ordinal))
            {
                diagnostics.Error(path, line, column,
                                  "namespaces are not part of the XML subset; a document has no xmlns "
                                      + "declarations");
                reader.MoveToElement();
                return false;
            }

            element.Attributes.Add(new XmlAttribute(reader.Name, reader.Value, line, column));
        }
        reader.MoveToElement();
        return true;
    }

    /* The declaration may say UTF-8 or nothing; the bytes decide what they are
       and something else here means the file was written by a different tool. */
    private static void CheckDeclaration(XmlReader reader, string path, int line, int column,
                                         Diagnostics diagnostics)
    {
        string? encoding = reader.GetAttribute("encoding");
        if (encoding is not null && !encoding.Equals("utf-8", StringComparison.OrdinalIgnoreCase))
        {
            diagnostics.Error(path, line, column,
                              $"the declaration says encoding '{encoding}'; engine documents are UTF-8");
        }
    }
}
