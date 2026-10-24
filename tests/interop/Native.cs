using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Ms110dInterop.Tests;

/// <summary>P/Invoke declarations for the C interface (include/m0lte/ms110d/ms110d.h).</summary>
internal static class Native
{
    private const string Lib = "ms110d_c";

    /// <summary>MS110D_ABI_VERSION in the header this project was written against.</summary>
    public const uint ExpectedAbiVersion = 1;

    public const int PrecisionSingle = 32;
    public const int PrecisionDouble = 64;
    public const int Ok = 0;
    public const int ErrInvalidArgument = 1;

    [ModuleInitializer]
    internal static void Init()
    {
        NativeLibrary.SetDllImportResolver(typeof(Native).Assembly, Resolve);
    }

    private static IntPtr Resolve(string name, Assembly assembly, DllImportSearchPath? path)
    {
        if (name != Lib)
        {
            return IntPtr.Zero;
        }
        return NativeLibrary.Load(LibraryPath());
    }

    /// <summary>The shared library under test, from MS110D_NATIVE_LIB (set by ctest).</summary>
    public static string LibraryPath()
    {
        string? p = Environment.GetEnvironmentVariable("MS110D_NATIVE_LIB");
        if (string.IsNullOrEmpty(p))
        {
            throw new InvalidOperationException("MS110D_NATIVE_LIB is not set; run these tests through ctest, or point it at libms110d_c.");
        }
        if (!File.Exists(p))
        {
            throw new FileNotFoundException("MS110D_NATIVE_LIB does not name a file", p);
        }
        return p;
    }

    [DllImport(Lib, EntryPoint = "ms110d_abi_version")]
    public static extern uint AbiVersion();

    [DllImport(Lib, EntryPoint = "ms110d_version")]
    public static extern int Version(out int major, out int minor, out int patch);

    [DllImport(Lib, EntryPoint = "ms110d_precision_bits")]
    public static extern int PrecisionBits(out int precision);

    [DllImport(Lib, EntryPoint = "ms110d_status_string")]
    private static extern IntPtr StatusStringPtr(int status);

    public static string StatusString(int status) => Marshal.PtrToStringAnsi(StatusStringPtr(status))!;
}
