// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later

using M0LTE.HfChannel;
using M0LTE.Ms110d;
using Xunit;

namespace Ms110dInterop.Tests;

/// <summary>
/// Layer 2 (issue 21) and C++ transmit, C# receive (issue 22), for every waveform number,
/// interleaver length and constraint length the standard defines. A failure here is a bug in
/// one implementation: do not loosen a check, find which side is wrong.
/// Set MS110D_LONG_TESTS=1 for many more random payloads and settings per combination.
/// </summary>
public class TxCrossCheckTests
{
    /// <summary>Per-sample tolerance between the two transmitters (docs/dev/modulator.md).</summary>
    private const double Tolerance = 1e-6;

    private static readonly int[] Waveforms = [0, 1, 2, 3, 4, 5, 6, 7, 8, 13];

    internal static bool Long => Environment.GetEnvironmentVariable("MS110D_LONG_TESTS") == "1";

    public static TheoryData<int, int, int> Combinations() => RxCases.Where((_, _, _) => true);

    internal static IEnumerable<(int Wn, int Il, int K)> AllCombinations()
    {
        foreach (int wn in Waveforms)
        {
            for (int il = 0; il < 4; il++)
            {
                if (wn == 0 && il == 0)
                {
                    continue; // Table D-XXXVII has no WN 0 ultra-short row
                }

                foreach (int k in new[] { 7, 9 })
                {
                    yield return (wn, il, k);
                }
            }
        }
    }

    /// <summary>A plain deterministic seed (string.GetHashCode and HashCode are randomised per process).</summary>
    internal static int Seed(int purpose, int wn, int il, int k, int round) =>
        (purpose * 1_000_003) + (wn * 40_009) + (il * 1_009) + (k * 101) + round;

    /// <summary>Payload lengths that matter for a block of <paramref name="b"/> info bits: the EOM ending
    /// exactly at a block end, exactly one block of payload (the EOM then needs a second block), one bit
    /// more, and several blocks.</summary>
    internal static int[] EdgeLengths(int b)
    {
        int eomEnd = b > 32 ? b - 32 : (2 * b) - 32;
        return [eomEnd, b, b + 1, (2 * b) + 5];
    }

    internal static byte[] RandomBits(Random rng, int n)
    {
        var bits = new byte[n];
        for (int i = 0; i < n; i++)
        {
            bits[i] = (byte)rng.Next(2);
        }

        return bits;
    }

    /// <summary>Settings varied from the seed so the run covers preamble, TLC, EOM, EOT, level and roll-off.</summary>
    private static Ms110dTxSettings RandomSettings(Random rng, int wn, int il, int k) => new()
    {
        WaveformNumber = wn,
        Interleaver = (Ms110dInterleaverKind)il,
        ConstraintLength = k,
        PreambleSuperframes = 1 + rng.Next(Long ? 6 : 3),
        TlcBlocks = rng.Next(3) == 0 ? rng.Next(1, 4) : 0,
        AppendEom = rng.Next(5) != 0,
        AppendEot = rng.Next(4) != 0,
        Amplitude = 0.1f + (0.8f * (float)rng.NextDouble()),
        RollOff = rng.Next(3) == 0 ? 0.05 + (0.95 * rng.NextDouble()) : 0.35,
    };

    [Theory]
    [MemberData(nameof(Combinations))]
    public void TransmitAudioMatchesTheCSharpModulator(int wn, int il, int k)
    {
        int input = Ms110dInterleaverParams.Get3k(wn, (Ms110dInterleaverKind)il).InputBits;
        int[] edges = EdgeLengths(input);
        // Default: every edge length. Long: many more rounds, with random lengths over up to four blocks.
        int rounds = Long ? 25 * edges.Length : edges.Length;
        for (int round = 0; round < rounds; round++)
        {
            int seed = Seed(1, wn, il, k, round);
            var rng = new Random(seed);
            Ms110dTxSettings settings = RandomSettings(rng, wn, il, k);
            int n = round < edges.Length || round % 2 == 0 ? edges[round % edges.Length] : rng.Next(0, (4 * input) + 1);
            byte[] payload = RandomBits(rng, n);

            // The C# in its standard probe pattern mode: the C++ sends the D.5.2.2 pattern (issue 68).
            float[] expected = StandardModulator.Create(settings).Modulate(payload);
            using var cpp = new CppTransmitter(settings, n);
            bool streamed = round % 2 == 1;
            float[] actual = streamed ? cpp.ModulateStreamed(payload, 1 + rng.Next(2000)) : cpp.ModulateOneShot(payload);

            string what = $"WN{wn} il{il} K{k} round {round} seed {seed} payload {n} bits ({(streamed ? "streamed" : "one-shot")}), settings {settings}";
            Assert.True(expected.Length == actual.Length, $"length {actual.Length} vs C# {expected.Length}: {what}");
            double worst = 0;
            int at = -1;
            for (int i = 0; i < expected.Length; i++)
            {
                double d = Math.Abs(expected[i] - (double)actual[i]);
                if (d > worst)
                {
                    worst = d;
                    at = i;
                }
            }

            Assert.True(worst <= Tolerance, $"max sample difference {worst:E2} at {at} exceeds {Tolerance:E0}: {what}");
        }
    }

    internal static void DecodeAndCompare(float[] audio, byte[] payload, string what, int wn, int il, int k)
    {
        var demod = new Ms110dDemodulator();
        Ms110dBurst? burst = null;
        demod.BurstCompleted += b => burst ??= b;
        demod.Process(audio);
        demod.Process(new float[6000]);
        Assert.True(burst is not null, $"no burst decoded: {what}");
        Assert.True(burst!.Reason == Ms110dBurstEndReason.Eom, $"burst ended {burst.Reason}, not Eom: {what}");
        Assert.True(burst.PayloadBits.AsSpan().SequenceEqual(payload), $"payload differs ({burst.PayloadBits.Length} bits vs {payload.Length}): {what}");
        Assert.True(burst.Lock is not null, $"no lock info: {what}");
        Assert.True(burst.Lock!.WaveformNumber == wn && (int)burst.Lock.Interleaver == il && burst.Lock.ConstraintLength == k,
            $"preamble announced WN{burst.Lock.WaveformNumber} il{(int)burst.Lock.Interleaver} K{burst.Lock.ConstraintLength}: {what}");
    }

    /// <summary>A C++ burst for the receive tests. Round 0 is a payload that ends exactly where the EOM
    /// ends a block (one block); round 1 is one block and one bit (two blocks, the second nearly empty);
    /// later rounds (long mode) are random over up to three blocks.</summary>
    internal static (Ms110dTxSettings Settings, byte[] Payload, float[] Audio, int Seed) CppBurst(int purpose, int wn, int il, int k, int round)
    {
        int seed = Seed(purpose, wn, il, k, round);
        var rng = new Random(seed);
        var settings = new Ms110dTxSettings
        {
            WaveformNumber = wn,
            Interleaver = (Ms110dInterleaverKind)il,
            ConstraintLength = k,
            PreambleSuperframes = 3 + rng.Next(3),
        };
        int input = Ms110dInterleaverParams.Get3k(wn, (Ms110dInterleaverKind)il).InputBits;
        int[] edges = EdgeLengths(input);
        int n = round < edges.Length ? edges[round] : rng.Next(1, (3 * input) + 1);
        byte[] payload = RandomBits(rng, n);
        using var cpp = new CppTransmitter(settings, n);
        return (settings, payload, round % 2 == 0 ? cpp.ModulateOneShot(payload) : cpp.ModulateStreamed(payload, 1 + rng.Next(5000)), seed);
    }
}

/// <summary>
/// The receive tests are split over several classes only so that xunit runs them in parallel (one class is
/// one collection): a decode of a long-interleaver block costs seconds. Default rounds: the clean channel
/// takes the payload that ends exactly where the EOM ends a block (round 0) and one block plus one bit
/// (round 2, a second nearly empty block); the AWGN channel takes two blocks and a bit (round 3).
/// MS110D_LONG_TESTS=1 runs twelve rounds of each, up to three blocks.
/// </summary>
internal static class RxCases
{
    public static TheoryData<int, int, int> Where(Func<int, int, int, bool> keep)
    {
        var data = new TheoryData<int, int, int>();
        foreach (var (wn, il, k) in TxCrossCheckTests.AllCombinations())
        {
            if (keep(wn, il, k))
            {
                data.Add(wn, il, k);
            }
        }

        return data;
    }

    public static void Clean(int wn, int il, int k, int[] defaultRounds)
    {
        int[] rounds = TxCrossCheckTests.Long ? Enumerable.Range(0, 12).ToArray() : defaultRounds;
        foreach (int round in rounds)
        {
            (Ms110dTxSettings s, byte[] payload, float[] audio, int seed) = TxCrossCheckTests.CppBurst(2, wn, il, k, round);
            var padded = new float[1500 + audio.Length];
            audio.CopyTo(padded, 1500);
            TxCrossCheckTests.DecodeAndCompare(padded, payload,
                $"clean WN{wn} il{il} K{k} round {round} seed {seed} payload {payload.Length} bits, settings {s}", wn, il, k);
        }
    }

    public static void Awgn(int wn, int il, int k, int[] defaultRounds)
    {
        // 20 dB in 3 kHz: well above every AWGN mask in Table D-LXIV (at most 13 dB), so a failure is a
        // transmit fault and not a receiver-limit fault. WN 8 has no mask row; it gets the same.
        const double snrDb = 20;
        int[] rounds = TxCrossCheckTests.Long ? Enumerable.Range(0, 12).ToArray() : defaultRounds;
        foreach (int round in rounds)
        {
            (Ms110dTxSettings s, byte[] payload, float[] audio, int seed) = TxCrossCheckTests.CppBurst(3, wn, il, k, round);
            int noiseSeed = TxCrossCheckTests.Seed(4, wn, il, k, round);
            var channel = new WattersonChannel(9600, seed: noiseSeed);
            float[] noisy = channel.Apply(audio, snrDb, leadInSamples: 2400, leadOutSamples: 4800);
            TxCrossCheckTests.DecodeAndCompare(noisy, payload,
                $"AWGN {snrDb} dB WN{wn} il{il} K{k} round {round} seed {seed} noise seed {noiseSeed} payload {payload.Length} bits, settings {s}", wn, il, k);
        }
    }
}

public class RxCleanEdgeTests
{
    public static TheoryData<int, int, int> Cases() => RxCases.Where((_, _, _) => true);

    [Theory]
    [MemberData(nameof(Cases))]
    public void CSharpReceiverDecodesTheCppTransmitterOnACleanChannel_EomEndsABlock(int wn, int il, int k) =>
        RxCases.Clean(wn, il, k, [0]);
}

public class RxCleanSpillTests
{
    public static TheoryData<int, int, int> Cases() => RxCases.Where((_, _, _) => true);

    [Theory]
    [MemberData(nameof(Cases))]
    public void CSharpReceiverDecodesTheCppTransmitterOnACleanChannel_OneBlockAndABit(int wn, int il, int k) =>
        RxCases.Clean(wn, il, k, [2]);
}

public class RxAwgnShortInterleaverTests
{
    public static TheoryData<int, int, int> Cases() => RxCases.Where((_, il, _) => il <= 1);

    [Theory]
    [MemberData(nameof(Cases))]
    public void CSharpReceiverDecodesTheCppTransmitterInAwgn(int wn, int il, int k) => RxCases.Awgn(wn, il, k, [3]);
}

public class RxAwgnMediumInterleaverTests
{
    public static TheoryData<int, int, int> Cases() => RxCases.Where((_, il, _) => il == 2);

    [Theory]
    [MemberData(nameof(Cases))]
    public void CSharpReceiverDecodesTheCppTransmitterInAwgn(int wn, int il, int k) => RxCases.Awgn(wn, il, k, [3]);
}

public class RxAwgnLongInterleaverTests
{
    public static TheoryData<int, int, int> Cases() => RxCases.Where((_, il, _) => il == 3);

    [Theory]
    [MemberData(nameof(Cases))]
    public void CSharpReceiverDecodesTheCppTransmitterInAwgn(int wn, int il, int k) => RxCases.Awgn(wn, il, k, [3]);
}
