using System.Runtime.InteropServices;

namespace Resonate.Tests.Managed;

/* The native host test reaches these through their unmanaged entry points. */
public static class Probe
{
    [UnmanagedCallersOnly]
    public static int Add(int left, int right)
    {
        return left + right;
    }
}
