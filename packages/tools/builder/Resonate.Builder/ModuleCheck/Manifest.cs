using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

namespace Resonate.Builder.ModuleCheck;

/* The manifest TOML subset, kept deliberately narrow: one [resonate.<kind>]
   table with flat keys, whose values are strings, integers, booleans or arrays
   of those. Anything else TOML can express is reported as a problem rather
   than parsed, because this parser is what keeps every manifest inside the
   subset the loader, editor and tooling implement. The grammar and the checks
   are the ones scripts/module_check.lua enforced. */
internal sealed class Manifest
{
    public Dictionary<string, Dictionary<string, object?>> Sections { get; } =
        new(StringComparer.Ordinal);

    public List<string> Problems { get; } = [];

    public static Manifest? Read(string path)
    {
        string text;
        try
        {
            text = File.ReadAllText(path);
        }
        catch (IOException)
        {
            return null;
        }
        Manifest manifest = new();
        manifest.Parse(text, path);
        return manifest;
    }

    private void Problem(string file, int line, string message)
    {
        Problems.Add($"{file}:{line}: {message}");
    }

    private void Parse(string text, string file)
    {
        Dictionary<string, object?>? current = null;
        string? key = null;
        StringBuilder keyChars = new();
        StringBuilder valueChars = new();
        StringBuilder headerChars = new();
        bool inValue = false;
        int bracket = 0;
        bool inString = false;
        bool inHeader = false;
        bool headerClosed = false;
        bool literal = false;
        bool escaped = false;
        int line = 1;

        int index = 0;
        while (index < text.Length)
        {
            char character = text[index];
            if (character == '\n')
            {
                if (inString)
                {
                    Problem(file, line, "unterminated string");
                    inString = false;
                }
                else if (inHeader)
                {
                    Problem(file, line, "unterminated table header");
                    inHeader = false;
                    headerChars.Clear();
                }
                else if (inValue)
                {
                    if (bracket > 0)
                    {
                        // A multiline array continues on the next line.
                        valueChars.Append(' ');
                    }
                    else
                    {
                        Flush(file, line, ref current, ref key, valueChars);
                        inValue = false;
                    }
                }
                else
                {
                    string candidate = keyChars.ToString().Trim();
                    if (candidate.Length != 0)
                    {
                        Problem(file, line, $"'{candidate}' has no '='");
                    }
                    keyChars.Clear();
                }
                headerClosed = false;
                ++line;
                ++index;
            }
            else if (character == '\r')
            {
                ++index;
            }
            else if (inString)
            {
                if (escaped)
                {
                    valueChars.Append('\\');
                    valueChars.Append(character);
                    escaped = false;
                }
                else if (character == '\\' && !literal)
                {
                    escaped = true;
                }
                else if (character == (literal ? '\'' : '"'))
                {
                    inString = false;
                    valueChars.Append(character);
                }
                else
                {
                    valueChars.Append(character);
                }
                ++index;
            }
            else if (inHeader)
            {
                if (character == ']')
                {
                    string name = headerChars.ToString().Trim();
                    int dot = name.IndexOf('.');
                    bool valid = dot > 0 && name.IndexOf('.', dot + 1) < 0 &&
                                 IsWord(name[..dot]) && IsWord(name[(dot + 1)..]) &&
                                 name[..dot] == "resonate";
                    if (!valid)
                    {
                        Problem(file, line,
                                $"table '{name}' is not engine TOML; manifest headers are "
                                + "[resonate.<kind>]");
                    }
                    else if (Sections.ContainsKey(name))
                    {
                        Problem(file, line, $"table [{name}] is defined twice");
                    }
                    else
                    {
                        Sections[name] = [];
                        current = Sections[name];
                    }
                    inHeader = false;
                    headerClosed = true;
                    headerChars.Clear();
                }
                else
                {
                    headerChars.Append(character);
                }
                ++index;
            }
            else if (character == '#')
            {
                while (index < text.Length && text[index] != '\n')
                {
                    ++index;
                }
            }
            else if (inValue && (character == '"' || character == '\''))
            {
                inString = true;
                literal = character == '\'';
                valueChars.Append(character);
                ++index;
            }
            else if (!inValue)
            {
                if (character == '[' && !headerClosed && keyChars.ToString().Trim().Length == 0)
                {
                    inHeader = true;
                    headerChars.Clear();
                }
                else if (character == '=')
                {
                    key = keyChars.ToString().Trim();
                    if (!IsKey(key))
                    {
                        Problem(file, line, $"invalid key '{key}'");
                        key = null;
                    }
                    keyChars.Clear();
                    valueChars.Clear();
                    inValue = true;
                }
                else if (headerClosed && character != ' ' && character != '\t')
                {
                    Problem(file, line, "unexpected text after a table header");
                    headerClosed = false;
                    keyChars.Append(character);
                }
                else
                {
                    keyChars.Append(character);
                }
                ++index;
            }
            else
            {
                if (character == '[')
                {
                    ++bracket;
                }
                else if (character == ']')
                {
                    --bracket;
                }
                valueChars.Append(character);
                ++index;
            }
        }

        Flush(file, line, ref current, ref key, valueChars);
        string leftover = keyChars.ToString().Trim();
        if (leftover.Length != 0)
        {
            Problem(file, line, $"'{leftover}' has no '='");
        }
        if (bracket != 0)
        {
            Problem(file, line, "unbalanced '[' in a value");
        }
    }

    private void Flush(string file, int line, ref Dictionary<string, object?>? current,
                       ref string? key, StringBuilder valueChars)
    {
        if (key is null)
        {
            return;
        }
        string raw = valueChars.ToString().Trim();
        if (current is null)
        {
            Problem(file, line,
                    $"'{key}' is outside any table; manifest fields live inside [resonate.<kind>]");
        }
        else if (raw.Length == 0)
        {
            Problem(file, line, $"'{key}' has no value");
        }
        else if (current.ContainsKey(key))
        {
            Problem(file, line, $"'{key}' is defined twice");
        }
        else if (raw[0] == '[')
        {
            if (raw[^1] != ']')
            {
                Problem(file, line, $"'{key}' has an unterminated array");
            }
            else
            {
                current[key] = ParseArray(file, line, key, raw);
            }
        }
        else
        {
            current[key] = ParseScalar(file, line, key, raw);
        }
        key = null;
        valueChars.Clear();
    }

    private List<object?> ParseArray(string file, int line, string key, string raw)
    {
        List<string> items = [];
        StringBuilder item = new();
        char? quote = null;
        for (int index = 1; index < raw.Length - 1; ++index)
        {
            char character = raw[index];
            if (quote is not null)
            {
                item.Append(character);
                if (character == quote)
                {
                    quote = null;
                }
            }
            else if (character == '"' || character == '\'')
            {
                quote = character;
                item.Append(character);
            }
            else if (character == ',')
            {
                items.Add(item.ToString());
                item.Clear();
            }
            else
            {
                item.Append(character);
            }
        }
        items.Add(item.ToString());

        List<object?> values = [];
        foreach (string entry in items)
        {
            string trimmed = entry.Trim();
            if (trimmed.Length != 0)
            {
                values.Add(ParseScalar(file, line, key, trimmed));
            }
        }
        return values;
    }

    private object? ParseScalar(string file, int line, string key, string raw)
    {
        char quote = raw[0];
        if (quote == '"' || quote == '\'')
        {
            if (raw.Length < 2 || raw[^1] != quote)
            {
                Problem(file, line, $"'{key}' has an unterminated string");
                return null;
            }
            string inner = raw[1..^1];
            if (quote == '\'')
            {
                return inner;
            }
            StringBuilder text = new();
            for (int index = 0; index < inner.Length; ++index)
            {
                if (inner[index] == '\\' && index + 1 < inner.Length)
                {
                    char escape = inner[++index];
                    switch (escape)
                    {
                        case 't':
                            text.Append('\t');
                            break;
                        case 'n':
                            text.Append('\n');
                            break;
                        case 'r':
                            text.Append('\r');
                            break;
                        case '"':
                            text.Append('"');
                            break;
                        case '\\':
                            text.Append('\\');
                            break;
                        default:
                            Problem(file, line,
                                    $"unsupported escape \\{escape} in '{key}'");
                            break;
                    }
                }
                else
                {
                    text.Append(inner[index]);
                }
            }
            return text.ToString();
        }
        if (raw == "true")
        {
            return true;
        }
        if (raw == "false")
        {
            return false;
        }
        bool integer = raw.Length != 0;
        for (int index = 0; index < raw.Length; ++index)
        {
            char character = raw[index];
            if (character == '-' && index == 0)
            {
                continue;
            }
            if (character < '0' || character > '9')
            {
                integer = false;
                break;
            }
        }
        if (integer && long.TryParse(raw, out long value))
        {
            return value;
        }
        Problem(file, line, $"unsupported value '{raw}' for '{key}'");
        return null;
    }

    private static bool IsWord(char character)
    {
        return char.IsAsciiLetterOrDigit(character) || character == '_' || character == '-';
    }

    private static bool IsWord(string text)
    {
        if (text.Length == 0)
        {
            return false;
        }
        foreach (char character in text)
        {
            if (!IsWord(character))
            {
                return false;
            }
        }
        return true;
    }

    private static bool IsKey(string key)
    {
        if (key.Length == 0)
        {
            return false;
        }
        foreach (char character in key)
        {
            if (!IsWord(character))
            {
                return false;
            }
        }
        return true;
    }
}
