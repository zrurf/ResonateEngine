using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

namespace Resonate.Builder.Assets;

/* An asset's identity sidecar: the GUID every reference names it by, plus the
   importer state the pipeline records. The engine's parser is the contract;
   this one accepts the same subset the sidecars are written in (three top-level
   string keys) and reports the same problems. */
internal sealed class AssetMeta
{
    public ulong GuidHigh { get; set; }

    public ulong GuidLow { get; set; }

    public string Importer { get; set; } = string.Empty;

    public string SourceHash { get; set; } = string.Empty;

    /* The [settings] table's key/value strings, as the engine's parser defines
       them; empty when the sidecar carries none. */
    public Dictionary<string, string> Settings { get; } = new(StringComparer.Ordinal);

    public bool ValidGuid => GuidHigh != 0UL || GuidLow != 0UL;

    internal static bool Read(string path, out AssetMeta meta, out string error)
    {
        meta = new AssetMeta();
        string text;
        try
        {
            text = File.ReadAllText(path);
        }
        catch (IOException exception)
        {
            error = exception.Message;
            return false;
        }
        return Parse(text, meta, out error);
    }

    internal static bool Parse(string text, AssetMeta meta, out string error)
    {
        Dictionary<string, string> values = new(StringComparer.Ordinal);
        Dictionary<string, string>? settings = null;
        int line = 0;
        foreach (string rawLine in text.Split('\n'))
        {
            ++line;
            string lineText = rawLine.Trim();
            if (lineText.Length == 0 || lineText[0] == '#')
            {
                continue;
            }
            if (lineText == "[settings]")
            {
                settings = meta.Settings;
                continue;
            }
            if (lineText.StartsWith('['))
            {
                error = $"line {line}: unknown table '{lineText}'";
                return false;
            }
            int equals = lineText.IndexOf('=');
            if (equals <= 0)
            {
                error = $"line {line}: expected `key = \"value\"`";
                return false;
            }
            string key = lineText[..equals].Trim();
            string value = lineText[(equals + 1)..].Trim();
            if (value.Length < 2 || value[0] != '"' || value[^1] != '"')
            {
                error = $"line {line}: '{key}' must be a quoted string";
                return false;
            }
            string? parsed = Unescape(value[1..^1]);
            if (parsed is null)
            {
                error = $"line {line}: '{key}' has an unsupported escape";
                return false;
            }
            (settings ?? values)[key] = parsed;
        }

        if (!values.TryGetValue("guid", out string? guid) || guid is null ||
            !ParseGuid(guid, out ulong high, out ulong low) || (high == 0UL && low == 0UL))
        {
            error = "the sidecar needs a non-null guid";
            return false;
        }
        meta.GuidHigh = high;
        meta.GuidLow = low;
        if (values.TryGetValue("importer", out string? importer))
        {
            if (importer.Length == 0)
            {
                error = "importer must be a non-empty string";
                return false;
            }
            meta.Importer = importer;
        }
        if (values.TryGetValue("source_hash", out string? hash))
        {
            if (hash.Length != 64U)
            {
                error = "source_hash must be 64 hexadecimal digits";
                return false;
            }
            StringBuilder normalized = new(64);
            foreach (char character in hash)
            {
                if (!Uri.IsHexDigit(character))
                {
                    error = "source_hash must be 64 hexadecimal digits";
                    return false;
                }
                normalized.Append(char.ToLowerInvariant(character));
            }
            meta.SourceHash = normalized.ToString();
        }
        error = string.Empty;
        return true;
    }

    /* The engine formats the two halves big-endian with the canonical dashes,
       so the text both sides compare is byte-for-byte the same. */
    internal static string FormatGuid(ulong high, ulong low)
    {
        const string digits = "0123456789abcdef";
        StringBuilder text = new(36);
        int absolute = 0;
        foreach (ulong half in new[] { high, low })
        {
            for (int shift = 56; shift >= 0; shift -= 8)
            {
                byte value = (byte)((half >> shift) & 0xFFUL);
                text.Append(digits[value >> 4]);
                text.Append(digits[value & 0x0F]);
                if (absolute == 3 || absolute == 5 || absolute == 7 || absolute == 9)
                {
                    text.Append('-');
                }
                ++absolute;
            }
        }
        return text.ToString();
    }

    /* Dashes are optional, as in the engine: the compact 32-digit form parses
       the same. */
    internal static bool ParseGuid(string text, out ulong high, out ulong low)
    {
        high = 0UL;
        low = 0UL;
        int half = 0;
        int nibbles = 0;
        foreach (char character in text)
        {
            if (character == '-')
            {
                continue;
            }
            int digit = HexValue(character);
            if (digit < 0 || half >= 2 || nibbles >= 16)
            {
                return false;
            }
            if (half == 0)
            {
                high = (high << 4) | (uint)digit;
            }
            else
            {
                low = (low << 4) | (uint)digit;
            }
            ++nibbles;
            if (nibbles == 16)
            {
                ++half;
                nibbles = 0;
            }
        }
        return half == 2 && nibbles == 0;
    }

    private static int HexValue(char character)
    {
        if (character >= '0' && character <= '9')
        {
            return character - '0';
        }
        if (character >= 'a' && character <= 'f')
        {
            return character - 'a' + 10;
        }
        if (character >= 'A' && character <= 'F')
        {
            return character - 'A' + 10;
        }
        return -1;
    }

    /* The writer escapes exactly these three, so the reader has to understand
       exactly them. */
    private static string? Unescape(string text)
    {
        StringBuilder value = new(text.Length);
        for (int index = 0; index < text.Length; ++index)
        {
            if (text[index] != '\\')
            {
                value.Append(text[index]);
                continue;
            }
            if (index + 1 >= text.Length)
            {
                return null;
            }
            char escape = text[++index];
            switch (escape)
            {
                case '\\':
                    value.Append('\\');
                    break;
                case '"':
                    value.Append('"');
                    break;
                case 'n':
                    value.Append('\n');
                    break;
                default:
                    return null;
            }
        }
        return value.ToString();
    }
}
