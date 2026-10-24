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

    private static bool Long => Environment.GetEnvironmentVariable("MS110D_LONG_TESTS") == "1";

    public static TheoryData<int, int, int> Combinations()
    {
        var data = new TheoryData<int, int, int>();
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
                    data.Add(wn, il, k);
                }
            }
        }

        return data;
    }

    private static byte[] RandomBits(Random rng, int n)
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
        int rounds = Long ? 25 : 2;
        int input = Ms110dInterleaverParams.Get3k(wn, (Ms110dInterleaverKind)il).InputBits;
        for (int round = 0; round < rounds; round++)
        {
            var rng = new Random(HashCode.Combine(wn, il, k, round, 2101));
            Ms110dTxSettings settings = RandomSettings(rng, wn, il, k);
            // Lengths from nothing to a little over a block, with block edges often chosen.
            int cap = Long ? 30000 : 12000;
            int[] lengths = [0, 1, rng.Next(1, Math.Min(cap, input + 40)), Math.Max(0, input - 32), input, Math.Min(cap, (input * 3 / 2) + 1)];
            int n = lengths[round % lengths.Length];
            if (n > cap)
            {
                n = cap;
            }

            byte[] payload = RandomBits(rng, n);

            float[] expected = new Ms110dModulator(settings).Modulate(payload);
            using var cpp = new CppTransmitter(settings, n);
            float[] actual = round % 2 == 0 ? cpp.ModulateOneShot(payload) : cpp.ModulateStreamed(payload, 1 + rng.Next(2000));

            string what = $"WN{wn} il{il} K{k} round {round} payload {n} bits, settings {settings}";
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

    private static void DecodeAndCompare(float[] audio, byte[] payload, string what, int wn, int il, int k)
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

    private static (Ms110dTxSettings Settings, byte[] Payload, float[] Audio) CppBurst(int wn, int il, int k, int round)
    {
        var rng = new Random(HashCode.Combine(wn, il, k, round, 2201));
        var settings = new Ms110dTxSettings
        {
            WaveformNumber = wn,
            Interleaver = (Ms110dInterleaverKind)il,
            ConstraintLength = k,
            PreambleSuperframes = 3 + rng.Next(3),
        };
        int input = Ms110dInterleaverParams.Get3k(wn, (Ms110dInterleaverKind)il).InputBits;
        int n = rng.Next(1, Math.Min(2000, Math.Max(1, input - 32)) + 1);
        byte[] payload = RandomBits(rng, n);
        using var cpp = new CppTransmitter(settings, n);
        return (settings, payload, cpp.ModulateOneShot(payload));
    }

    [Theory]
    [MemberData(nameof(Combinations))]
    public void CSharpReceiverDecodesTheCppTransmitterOnACleanChannel(int wn, int il, int k)
    {
        int rounds = Long ? 5 : 1;
        for (int round = 0; round < rounds; round++)
        {
            (Ms110dTxSettings s, byte[] payload, float[] audio) = CppBurst(wn, il, k, round);
            var padded = new float[1500 + audio.Length];
            audio.CopyTo(padded, 1500);
            DecodeAndCompare(padded, payload, $"clean WN{wn} il{il} K{k} round {round} payload {payload.Length}", wn, il, k);
        }
    }

    [Theory]
    [MemberData(nameof(Combinations))]
    public void CSharpReceiverDecodesTheCppTransmitterInAwgn(int wn, int il, int k)
    {
        // 20 dB in 3 kHz: well above every AWGN mask in Table D-LXIV (at most 13 dB), so a failure is a
        // transmit fault and not a receiver-limit fault. WN 8 has no mask row; it gets the same.
        const double snrDb = 20;
        int rounds = Long ? 5 : 1;
        for (int round = 0; round < rounds; round++)
        {
            (Ms110dTxSettings s, byte[] payload, float[] audio) = CppBurst(wn, il, k, round);
            var channel = new WattersonChannel(9600, seed: 9000 + wn + (100 * il) + round);
            float[] noisy = channel.Apply(audio, snrDb, leadInSamples: 2400, leadOutSamples: 4800);
            DecodeAndCompare(noisy, payload, $"AWGN {snrDb} dB WN{wn} il{il} K{k} round {round} payload {payload.Length}", wn, il, k);
        }
    }
}
