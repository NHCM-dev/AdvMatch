using System.Runtime.InteropServices;

Console.WriteLine($"AdvMatch ABI {Native.AbiVersion()}");

internal static class Native
{
    [DllImport("advmatch", EntryPoint = "advmatch_abi_version",
        CallingConvention = CallingConvention.Cdecl)]
    internal static extern int AbiVersion();
}
