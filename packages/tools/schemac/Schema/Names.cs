using System;
using System.Collections.Generic;

namespace Resonate.Tools.Schemac.Schema;

/* What a name may be, in the places a schema gives one. The rules exist because
   every name ends up in generated C++: a type becomes a struct, the module id
   becomes a namespace, a field becomes a member. */
internal static class Names
{
    private static readonly HashSet<string> CppKeywords = new(StringComparer.Ordinal)
    {
        "alignas", "alignof", "and", "and_eq", "asm", "atomic_cancel", "atomic_commit",
        "atomic_noexcept", "auto", "bitand", "bitor", "bool", "break", "case", "catch", "char",
        "char8_t", "char16_t", "char32_t", "class", "compl", "concept", "const", "consteval",
        "constexpr", "constinit", "const_cast", "continue", "co_await", "co_return", "co_yield",
        "decltype", "default", "delete", "do", "double", "dynamic_cast", "else", "enum",
        "explicit", "export", "extern", "false", "float", "for", "friend", "goto", "if", "inline",
        "int", "long", "mutable", "namespace", "new", "noexcept", "not", "not_eq", "nullptr",
        "operator", "or", "or_eq", "private", "protected", "public", "reflexpr", "register",
        "reinterpret_cast", "requires", "return", "short", "signed", "sizeof", "static",
        "static_assert", "static_cast", "struct", "switch", "synchronized", "template", "this",
        "thread_local", "throw", "true", "try", "typedef", "typeid", "typename", "union",
        "unsigned", "using", "virtual", "void", "volatile", "wchar_t", "while", "xor", "xor_eq",
    };

    internal static bool IsIdentifierShape(string text)
    {
        if (text.Length == 0 || (!char.IsAsciiLetter(text[0]) && text[0] != '_'))
        {
            return false;
        }
        foreach (char character in text)
        {
            if (!char.IsAsciiLetterOrDigit(character) && character != '_')
            {
                return false;
            }
        }
        return true;
    }

    internal static bool IsCppKeyword(string text)
    {
        return CppKeywords.Contains(text);
    }

    /* A module id is the reverse-DNS identity the manifest and the loader use,
       and it becomes the generated C++ namespace: lowercase, at least two
       segments, each a plain identifier. */
    internal static bool IsModuleId(string text)
    {
        string[] segments = text.Split('.');
        if (segments.Length < 2)
        {
            return false;
        }

        foreach (string segment in segments)
        {
            if (segment.Length == 0 || !char.IsAsciiLetterLower(segment[0]) || IsCppKeyword(segment))
            {
                return false;
            }
            foreach (char character in segment)
            {
                if (!char.IsAsciiLetterLower(character) && !char.IsAsciiDigit(character))
                {
                    return false;
                }
            }
        }
        return true;
    }

    /* The shared "this name is usable" check: reports at the caller's position
       and answers whether the name passed. */
    internal static bool Check(string text, string what, string file, int line, int column,
                               Diagnostics diagnostics)
    {
        if (!IsIdentifierShape(text))
        {
            diagnostics.Error(file, line, column,
                              $"{what} '{text}' is not an identifier: letters, digits and "
                                  + "underscores, not starting with a digit");
            return false;
        }
        if (IsCppKeyword(text))
        {
            diagnostics.Error(file, line, column, $"{what} '{text}' is a C++ keyword");
            return false;
        }
        return true;
    }
}
