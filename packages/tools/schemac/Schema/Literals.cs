using System;
using System.Globalization;
using System.Numerics;

namespace Resonate.Tools.Schemac.Schema;

/* Numeric literals in schema attributes: decimal only, canonicalised on the way
   in, so the same declaration always produces the same text in every output. */
internal static class Literals
{
    internal static bool TryInteger(string text, out BigInteger value)
    {
        value = BigInteger.Zero;
        string trimmed = text.Trim();
        int start = trimmed.Length > 0 && (trimmed[0] == '+' || trimmed[0] == '-') ? 1 : 0;
        if (start == trimmed.Length)
        {
            return false;
        }
        for (int index = start; index < trimmed.Length; ++index)
        {
            if (!char.IsAsciiDigit(trimmed[index]))
            {
                return false;
            }
        }
        return BigInteger.TryParse(trimmed, NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture,
                                   out value);
    }

    internal static bool TryFloat(string text, out double value)
    {
        value = 0.0;
        string trimmed = text.Trim();
        return trimmed.Length > 0
               && double.TryParse(trimmed, NumberStyles.Float, CultureInfo.InvariantCulture, out value)
               && double.IsFinite(value);
    }

    internal static string Integer(BigInteger value)
    {
        return value.ToString(CultureInfo.InvariantCulture);
    }

    internal static string Float(double value)
    {
        return value.ToString("R", CultureInfo.InvariantCulture);
    }
}
