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

    public const int ErrBufferTooSmall = 2;
    public const int ErrState = 4;

    /// <summary>ms110d_tx_settings. Sequential layout puts the double at offset 40, as in the C header.</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct TxSettings
    {
        public uint StructSize;
        public int WaveformNumber;
        public int Interleaver;
        public int ConstraintLength;
        public int PreambleSuperframes;
        public int TlcBlocks;
        public int AppendEom;
        public int AppendEot;
        public float Amplitude;
        public uint Reserved;
        public double RollOff;
    }

    [DllImport(Lib, EntryPoint = "ms110d_tx_settings_default")]
    public static extern int TxSettingsDefault(ref TxSettings settings);

    [DllImport(Lib, EntryPoint = "ms110d_tx_create")]
    public static extern int TxCreate(ref TxSettings settings, uint maxPayloadBits, out IntPtr tx);

    [DllImport(Lib, EntryPoint = "ms110d_tx_destroy")]
    public static extern void TxDestroy(IntPtr tx);

    [DllImport(Lib, EntryPoint = "ms110d_tx_audio_length")]
    public static extern int TxAudioLength(IntPtr tx, uint payloadBits, out uint samples);

    [DllImport(Lib, EntryPoint = "ms110d_tx_begin")]
    public static extern int TxBegin(IntPtr tx);

    [DllImport(Lib, EntryPoint = "ms110d_tx_push_bits")]
    public static extern int TxPushBits(IntPtr tx, byte[] bits, uint count);

    [DllImport(Lib, EntryPoint = "ms110d_tx_end_payload")]
    public static extern int TxEndPayload(IntPtr tx);

    [DllImport(Lib, EntryPoint = "ms110d_tx_read")]
    public static extern int TxRead(IntPtr tx, float[] audio, uint capacity, out uint written);

    [DllImport(Lib, EntryPoint = "ms110d_tx_modulate")]
    public static extern int TxModulate(IntPtr tx, byte[] bits, uint bitCount, float[] audio, uint capacity, out uint written);

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

    [DllImport(HooksLib, EntryPoint = "ms110d_th_frame")]
    public static extern unsafe int ThFrame(byte* payload, uint payloadLen, int appendEom, uint inputBits, byte* output, uint outCapacity, out uint outLen);

    [DllImport(HooksLib, EntryPoint = "ms110d_th_preamble")]
    public static extern unsafe int ThPreamble(int tlcBlocks, int superframes, int wn, int interleaverKind, int constraintLength, byte* chips, uint capacity, out uint chipsLen);

    /// <summary>
    /// Binds every hook once, on the calling thread, with arguments the hooks reject. Lazy binding of
    /// a P/Invoke from several threads at once (through the resolver above) crashed the runtime
    /// intermittently, so tests that use worker threads call this first.
    /// </summary>
    public static unsafe void BindAll()
    {
        _ = StatusString(Ok);
        _ = ThEncode(0, null, 0, null, 0, out _);
        _ = ThPuncture(0, 0, 0, null, 0, null, 0, out _);
        _ = ThInterleave(0, 0, null, 0, null, 0);
        _ = ThTranscode(0, null, 0, null, 0, out _);
        _ = ThScramble(0, 0, null, 0, null, 0);
        _ = ThTrinomial(0, 0, null, 0);
        _ = ThFrame(null, 0, 0, 0, null, 0, out _);
        _ = ThPreamble(0, 0, 0, 0, 0, null, 0, out _);
    }
}
