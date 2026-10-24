// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later

using System.Diagnostics;
using M0LTE.Fec;
using M0LTE.Ms110d;
using M0LTE.Ms110d.Fec;
using Xunit;

namespace Ms110dInterop.Tests;

/// <summary>
/// Layer 1 of docs/plan.md ("bits on the wire"): random frames through every stage that exists
/// in the C++ (via the test-only hooks library) and in the C# reference (public API only), in
/// one process, compared exactly. One test per combination of waveform number, interleaver
/// length and constraint length; a failure names the first stage that differs. See
/// docs/dev/layer1.md for the counts and the environment variables.
///
/// Each stage is fed the C# output of the stage before it, so a stage that differs is named
/// even when everything upstream agreed; a stage upstream that differed already failed first.
/// </summary>
public class Layer1Tests
{
    private readonly ITestOutputHelper _output;

    public Layer1Tests(ITestOutputHelper output) => _output = output;

    /// <summary>The plan's gate: random frames per combination in the full run.</summary>
    public const int FullFrames = 100_000;
    public const int DefaultFrames = 300;

    public static IEnumerable<TheoryDataRow<int, string, int>> Combinations()
    {
        string? filter = Environment.GetEnvironmentVariable("MS110D_L1_WN");
        HashSet<int>? only = string.IsNullOrWhiteSpace(filter)
            ? null
            : filter.Split(',', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries).Select(int.Parse).ToHashSet();
        foreach (int wn in new[] { 0, 1, 2, 3, 4, 5, 6, 7, 8, 13 })
        {
            if (only != null && !only.Contains(wn))
            {
                continue;
            }
            foreach (var kind in Enum.GetValues<Ms110dInterleaverKind>())
            {
                if (!Ms110dInterleaverParams.Has3k(wn, kind))
                {
                    continue;
                }
                foreach (int k in new[] { 7, 9 })
                {
                    yield return new TheoryDataRow<int, string, int>(wn, kind.ToString(), k);
                }
            }
        }
    }

    [Theory]
    [MemberData(nameof(Combinations))]
    public void RandomFramesAreIdenticalStageByStage(int wn, string kindName, int constraintLength)
    {
        var kind = Enum.Parse<Ms110dInterleaverKind>(kindName);
        var combo = new Combo(wn, kind, constraintLength);
        int frames = FrameCount();
        int threads = ThreadCount();
        string? failure = null;
        var sw = Stopwatch.StartNew();

        Parallel.For(0, threads, new ParallelOptions { MaxDegreeOfParallelism = threads }, t =>
        {
            var ctx = new Context(combo);
            for (int f = t; f < frames && Volatile.Read(ref failure) == null; f += threads)
            {
                string? msg = CheckFrame(ctx, f);
                if (msg != null)
                {
                    Interlocked.CompareExchange(ref failure, msg, null);
                }
            }
        });

        sw.Stop();
        _output.WriteLine($"{combo.Name}: {frames} frames, {threads} threads, {sw.Elapsed.TotalSeconds:F2} s");
        Assert.True(failure == null, failure);
    }

    // ---- The stages. EXTENSION POINT (issue #17, framing in #16): the stages run in the order of
    // ---- this method. Framing (Ms110dFraming.BuildTxBits: payload to framed bits, EOM, padding) goes
    // ---- BEFORE "encode", producing the per-block info bits; the burst symbol stream (preamble,
    // ---- mini-probes, EOT, Ms110dModulator.BuildSymbols) goes AFTER "scramble". Add a hook in
    // ---- tests/testhooks for each, a Stage call here, and a frame count of whole bursts.

    private static string? CheckFrame(Context c, int frame)
    {
        ulong seed = Mix(c.Combo.Seed, (ulong)frame);
        c.FillInfo(seed);

        string? d =
            Stage_Encode(c) ??
            Stage_Puncture(c) ??
            Stage_Interleave(c) ??
            Stage_EncodeBlock(c) ??
            Stage_Scramble(c);
        return d == null ? null : $"layer 1: first stage that differs is '{d}' ({c.Combo.Name}, frame {frame}, seed 0x{seed:x16})";
    }

    private static string? Stage_Encode(Context c)
    {
        TailBitingEncoder.Encode(c.Code, c.Info, c.CsCoded);
        int st = c.NativeEncode();
        return Compare("encode", c.CsCoded, c.NatCoded, st);
    }

    private static string? Stage_Puncture(Context c)
    {
        int n = Ms110dPuncture.Apply(c.Spec, c.CsCoded, c.CsPunctured);
        if (n != c.CsPunctured.Length)
        {
            return $"puncture (C# wrote {n}, expected {c.CsPunctured.Length})";
        }
        int st = c.NativePuncture();
        return Compare("puncture", c.CsPunctured, c.NatPunctured, st);
    }

    private static string? Stage_Interleave(Context c)
    {
        c.CsInterleaver.Interleave(c.CsPunctured, c.CsInterleaved);
        int st = c.NativeInterleave();
        return Compare("interleave", c.CsInterleaved, c.NatInterleaved, st);
    }

    // The C# composed encode path against the same three stages chained in the C++.
    private static string? Stage_EncodeBlock(Context c)
    {
        byte[] block = Ms110dFraming.EncodeBlock(c.Code, c.Spec, c.CsInterleaver, c.Info);
        return Compare("encode-block", block, c.NatInterleaved, 0);
    }

    private static string? Stage_Scramble(Context c)
    {
        if (c.Combo.Wn == 0)
        {
            // Walsh waveform: the Trinomial scramble sequence, reset at the interleaver boundary.
            var tri = new Trinomial159Scrambler();
            for (int i = 0; i < c.TrinomialCount; i++)
            {
                c.CsTrinomial[i] = (byte)tri.Next();
            }
            int st = c.NativeTrinomial();
            return Compare("scramble (trinomial)", c.CsTrinomial, c.NatTrinomial, st);
        }

        // Transcoding has no public C# entry point (it is private in the modulator), so it is
        // checked against the C# values in the C++ unit tests and the reference files; the C++
        // transcode output is the input to both scramblers here.
        int tst = c.NativeTranscode();
        if (tst != 0)
        {
            return $"transcode (native status {tst})";
        }
        var scr = new Ms110dScrambler();
        bool qam = c.Mode.Modulation == Ms110dModulation.Qam16;
        for (int i = 0; i < c.Symbols; i++)
        {
            if (i % c.Mode.U == 0)
            {
                scr.Reset();
            }
            int s = c.NatSymbols[i];
            c.CsScrambled[i] = (byte)(qam ? scr.NextQam(s, c.Mode.BitsPerSymbol) : scr.NextPsk(s));
        }
        int sst = c.NativeScramble();
        return Compare(qam ? "scramble (qam)" : "scramble (psk)", c.CsScrambled, c.NatScrambled, sst);
    }

    private static string? Compare(string stage, ReadOnlySpan<byte> expected, ReadOnlySpan<byte> actual, int nativeStatus)
    {
        if (nativeStatus != Native.Ok)
        {
            return $"{stage} (native status {nativeStatus}: {Native.StatusString(nativeStatus)})";
        }
        if (expected.Length != actual.Length)
        {
            return $"{stage} (length: C# {expected.Length}, C++ {actual.Length})";
        }
        for (int i = 0; i < expected.Length; i++)
        {
            if (expected[i] != actual[i])
            {
                return $"{stage} (index {i}: C# {expected[i]}, C++ {actual[i]})";
            }
        }
        return null;
    }

    // ---- Counts and seeds.

    public static int FrameCount()
    {
        string? n = Environment.GetEnvironmentVariable("MS110D_L1_FRAMES");
        if (!string.IsNullOrWhiteSpace(n))
        {
            return int.Parse(n);
        }
        return Environment.GetEnvironmentVariable("MS110D_L1_FULL") == "1" ? FullFrames : DefaultFrames;
    }

    private static int ThreadCount()
    {
        string? n = Environment.GetEnvironmentVariable("MS110D_L1_THREADS");
        return string.IsNullOrWhiteSpace(n) ? Math.Clamp(Environment.ProcessorCount, 1, 4) : int.Parse(n);
    }

    internal static ulong Mix(ulong a, ulong b)
    {
        // splitmix64 finaliser over a combined word: frame i of a combination is independent of the thread layout.
        ulong z = a + 0x9E3779B97F4A7C15UL * (b + 1);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9UL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBUL;
        return z ^ (z >> 31);
    }

    private sealed record Combo(int Wn, Ms110dInterleaverKind Kind, int K)
    {
        public string Name => $"wn{Wn}/{Kind}/K{K}";
        public ulong Seed => Mix(0x110D0001UL, (ulong)(Wn * 1000 + (int)Kind * 10 + K));
    }

    /// <summary>Per-thread buffers for one combination (no allocation per frame in the C++ calls).</summary>
    private sealed unsafe class Context
    {
        public readonly Combo Combo;
        public readonly Ms110dMode Mode;
        public readonly Ms110dInterleaverParams Il;
        public readonly ConvolutionalCode Code;
        public readonly PunctureSpec Spec;
        public readonly Ms110dInterleaver CsInterleaver;
        public readonly int RateNum, RateDen, Symbols, TrinomialCount;
        public readonly byte[] Info, CsCoded, NatCoded, CsPunctured, NatPunctured, CsInterleaved, NatInterleaved;
        public readonly byte[] NatSymbols, CsScrambled, NatScrambled, CsTrinomial, NatTrinomial;

        public Context(Combo combo)
        {
            Combo = combo;
            Mode = Ms110dMode.Mode3k(combo.Wn);
            Il = Ms110dInterleaverParams.Get3k(combo.Wn, combo.Kind);
            Code = combo.K == 7 ? ConvolutionalCode.K7 : ConvolutionalCode.K9;
            string[] r = Mode.CodeRate.Split('/');
            RateNum = int.Parse(r[0]);
            RateDen = int.Parse(r[1]);
            Spec = Ms110dPuncture.Get(Code, Mode.CodeRate);
            CsInterleaver = new Ms110dInterleaver(Il.SizeBits, Il.Increment);
            Info = new byte[Il.InputBits];
            CsCoded = new byte[2 * Il.InputBits];
            NatCoded = new byte[2 * Il.InputBits];
            int punctured = Spec.OutputLength(Il.InputBits);
            if (punctured != Il.SizeBits)
            {
                throw new InvalidOperationException($"{combo.Name}: punctured length {punctured} is not the interleaver size {Il.SizeBits}");
            }
            CsPunctured = new byte[punctured];
            NatPunctured = new byte[punctured];
            CsInterleaved = new byte[Il.SizeBits];
            NatInterleaved = new byte[Il.SizeBits];
            Symbols = combo.Wn == 0 ? 0 : Il.SizeBits / Mode.BitsPerSymbol;
            NatSymbols = new byte[Symbols];
            CsScrambled = new byte[Symbols];
            NatScrambled = new byte[Symbols];
            // WN 0: one interleaver block is Il.Frames Walsh symbols of 32 chips each.
            TrinomialCount = combo.Wn == 0 ? Il.Frames * 32 : 0;
            CsTrinomial = new byte[TrinomialCount];
            NatTrinomial = new byte[TrinomialCount];
        }

        public void FillInfo(ulong seed)
        {
            ulong x = seed;
            int i = 0;
            while (i < Info.Length)
            {
                x = Mix(x, 1);
                for (int b = 0; b < 64 && i < Info.Length; b++)
                {
                    Info[i++] = (byte)((x >> b) & 1);
                }
            }
        }

        public int NativeEncode()
        {
            fixed (byte* i = Info) fixed (byte* o = NatCoded)
            {
                return Native.ThEncode(Combo.K, i, (uint)Info.Length, o, (uint)NatCoded.Length, out _);
            }
        }

        public int NativePuncture()
        {
            // From the C# coded bits, so this stage is judged on its own.
            fixed (byte* i = CsCoded) fixed (byte* o = NatPunctured)
            {
                return Native.ThPuncture(Combo.K, RateNum, RateDen, i, (uint)CsCoded.Length, o, (uint)NatPunctured.Length, out _);
            }
        }

        public int NativeInterleave()
        {
            fixed (byte* i = CsPunctured) fixed (byte* o = NatInterleaved)
            {
                return Native.ThInterleave((uint)Il.SizeBits, (uint)Il.Increment, i, (uint)CsPunctured.Length, o, (uint)NatInterleaved.Length);
            }
        }

        public int NativeTranscode()
        {
            fixed (byte* i = CsInterleaved) fixed (byte* o = NatSymbols)
            {
                return Native.ThTranscode(Combo.Wn, i, (uint)CsInterleaved.Length, o, (uint)NatSymbols.Length, out _);
            }
        }

        public int NativeScramble()
        {
            fixed (byte* i = NatSymbols) fixed (byte* o = NatScrambled)
            {
                return Native.ThScramble(Combo.Wn, (uint)Mode.U, i, (uint)NatSymbols.Length, o, (uint)NatScrambled.Length);
            }
        }

        public int NativeTrinomial()
        {
            fixed (byte* o = NatTrinomial)
            {
                return Native.ThTrinomial((uint)TrinomialCount, (uint)TrinomialCount, o, (uint)NatTrinomial.Length);
            }
        }
    }
}
