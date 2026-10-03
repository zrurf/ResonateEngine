using System;
using System.IO;
using System.Xml;
using System.Xml.Schema;

namespace Resonate.Tools.Schemac.Schema;

/* The shape contract for declaration files, as an XSD: editors read it, and the
   build has the compiler check every declaration against it too, so the
   contract an editor shows and the checks the compiler makes cannot drift apart
   in the direction that matters. It is deliberately looser than the compiler
   wherever XSD cannot say the same thing (type references, constraint ranges,
   migration chains), so the only thing this check adds is the strict direction:
   a declaration the compiler accepts that the contract does not know about. */
internal static class ShapeContract
{
    /* Null when the schema cannot be read or does not compile; both are
       reported, and a build that passes a broken contract has to fail. */
    internal static XmlSchemaSet? Load(string path, Diagnostics diagnostics)
    {
        XmlSchemaSet schemas = new();
        try
        {
            using XmlReader reader = XmlReader.Create(path, new XmlReaderSettings
            {
                DtdProcessing = DtdProcessing.Prohibit,
                XmlResolver = null,
            });
            schemas.Add(null, reader);
            schemas.Compile();
        }
        catch (XmlSchemaException error)
        {
            diagnostics.Error(path, error.LineNumber, error.LinePosition,
                              $"the shape contract does not compile: {Trim(error.Message)}");
            return null;
        }
        catch (Exception error) when (error is IOException or XmlException
                                      or UnauthorizedAccessException)
        {
            diagnostics.Error($"cannot read the shape contract '{PathText.Relative(path)}': "
                              + error.Message);
            return null;
        }
        return schemas;
    }

    /* Validates one document, reporting every violation. */
    internal static void Validate(string path, XmlSchemaSet schemas, Diagnostics diagnostics)
    {
        XmlReaderSettings settings = new()
        {
            ValidationType = ValidationType.Schema,
            DtdProcessing = DtdProcessing.Prohibit,
            XmlResolver = null,
            Schemas = schemas,
        };

        settings.ValidationEventHandler += (_, arguments) =>
        {
            XmlSchemaException? at = arguments.Exception as XmlSchemaException;
            diagnostics.Error(path, at?.LineNumber ?? 0, at?.LinePosition ?? 0,
                              "the shape contract rejects this declaration: "
                                  + Trim(arguments.Message));
        };

        try
        {
            using XmlReader reader = XmlReader.Create(path, settings);
            while (reader.Read())
            {
            }
        }
        catch (XmlException error)
        {
            diagnostics.Error(path, error.LineNumber, error.LinePosition,
                              "the shape contract rejects this declaration: "
                                  + Trim(error.Message));
        }
    }

    /* The BCL appends a "; Line n, position m." trailer to its messages, which
       the diagnostic prefix already carries. */
    private static string Trim(string message)
    {
        int trailer = message.IndexOf(" Line ", StringComparison.Ordinal);
        return trailer < 0 ? message : message[..trailer].TrimEnd();
    }
}
