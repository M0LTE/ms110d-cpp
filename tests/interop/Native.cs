// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later

using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Ms110dInterop.Tests;

/// <summary>P/Invoke declarations for the C interface (include/m0lte/ms110d/ms110d.h).</summary>
internal static class Native
{
    private const string Lib = "ms110d_c";
    private const string HooksLib = "ms110d_testhooks";

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
        if (name == Lib)
        {
            return NativeLibrary.Load(LibraryPath());
        }
        if (name == HooksLib)
        {
            return NativeLibrary.Load(TestHooksPath());
        }
        return IntPtr.Zero;
    }

    /// <summary>The test-only hooks library (tests/testhooks), from MS110D_TESTHOOKS_LIB (set by ctest).</summary>
    public static string TestHooksPath()
    {
        string? p = Environment.GetEnvironmentVariable("MS110D_TESTHOOKS_LIB");
        if (string.IsNullOrEmpty(p) || !File.Exists(p))
        {
            throw new InvalidOperationException("MS110D_TESTHOOKS_LIB is not set to a file; run these tests through ctest, or point it at libms110d_testhooks.");
        }
        return p;
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

    // ---- Test-only stage hooks (tests/testhooks/ms110d_testhooks.h). Not part of the C interface. ----

    [DllImport(HooksLib, EntryPoint = "ms110d_th_encode")]
    public static extern unsafe int ThEncode(int constraintLength, byte* info, uint infoLen, byte* coded, uint codedCapacity, out uint codedLen);

    [DllImport(HooksLib, EntryPoint = "ms110d_th_puncture")]
    public static extern unsafe int ThPuncture(int constraintLength, int rateNum, int rateDen, byte* coded, uint codedLen, byte* output, uint outCapacity, out uint outLen);

    [DllImport(HooksLib, EntryPoint = "ms110d_th_interleave")]
    public static extern unsafe int ThInterleave(uint sizeBits, uint increment, byte* input, uint inLen, byte* output, uint outCapacity);

    [DllImport(HooksLib, EntryPoint = "ms110d_th_transcode")]
    public static extern unsafe int ThTranscode(int wn, byte* bits, uint inLen, byte* symbols, uint symbolsCapacity, out uint symbolsLen);

    [DllImport(HooksLib, EntryPoint = "ms110d_th_scramble")]
    public static extern unsafe int ThScramble(int wn, uint frameSymbols, byte* symbols, uint symbolsLen, byte* output, uint outCapacity);

    [DllImport(HooksLib, EntryPoint = "ms110d_th_trinomial")]
    public static extern unsafe int ThTrinomial(uint blockSymbols, uint count, byte* output, uint outCapacity);
}
