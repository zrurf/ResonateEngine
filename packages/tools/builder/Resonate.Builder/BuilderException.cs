using System;

namespace Resonate.Builder;

/* A step failed with a message meant for the developer, not a stack trace. */
internal sealed class BuilderException : Exception
{
    internal BuilderException(string message) : base(message)
    {
    }
}
