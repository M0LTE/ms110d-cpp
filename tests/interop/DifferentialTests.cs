// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later

using M0LTE.HfChannel;
using M0LTE.Ms110d;
using Ms110dInterop.Tests.Layer3;
using Xunit;

namespace Ms110dInterop.Tests;

// Random streams through both receivers (docs/dev/parity.md). Each stream is one to three bursts of
// random waveform, interleaver, code, length and level, some cut short or without an EOM, with gaps,
// through a random HF channel, noise and carrier offset. The pinned C# receiver and the C++ receiver
// (default build) must give the same bursts. MS110D_DIFF_STREAMS sets how many (default 16) and
// MS110D_DIFF_SEED the first seed (default 1); a failure names the seed, which replays the stream.
public class DifferentialTests(ITestOutputHelper output)
{
    static readonly int[] Waveforms = [0, 1, 2, 3, 4, 5, 6, 7, 8, 13];
    static readonly Layer3Channel[] Channels = [Layer3Channel.Awgn, Layer3Channel.Good, Layer3Channel.Moderate, Layer3Channel.Poor];

    sealed record Burst(int Wn, Ms110dInterleaverKind Il, int K, Ms110dBurstEndReason Reason, int Blocks, byte[] Payload)
    {
        public override string ToString() =>
            $"WN {Wn} {Il} K{K} {Reason} blocks {Blocks} payload {Payload.Length} bits {Convert.ToHexString(System.Security.Cryptography.SHA256.HashData(Payload))[..8]}";
    }

    // The stream for one seed, and a line saying what is in it.
    static (float[] Audio, string What) Stream(int seed)
    {
        var r = new Random(seed);
        var parts = new List<float[]> { new float[r.Next(1000, 9600)] };
        var what = new List<string>();
        int bursts = r.Next(1, 4);
        for (int b = 0; b < bursts; b++)
        {
            int wn = Waveforms[r.Next(Waveforms.Length)];
            var il = r.Next(3) == 0 ? Ms110dInterleaverKind.UltraShort : Ms110dInterleaverKind.Short;
            if (!Ms110dInterleaverParams.Has3k(wn, il))
            {
                il = Ms110dInterleaverKind.Short;
            }
            int k = r.Next(2) == 0 ? 7 : 9;
            int bits = wn == 0 ? r.Next(40, 240) : r.Next(100, 2400);
            bool eom = r.Next(6) != 0;
            var payload = new byte[bits];
            for (int i = 0; i < bits; i++)
            {
                payload[i] = (byte)r.Next(2);
            }
            float[] x = new Ms110dModulator(new Ms110dTxSettings
            {
                WaveformNumber = wn,
                Interleaver = il,
                ConstraintLength = k,
                PreambleSuperframes = r.Next(2, 5),
                AppendEom = eom,
            }).Modulate(payload);
            string cut = "";
            if (r.Next(5) == 0)
            {
                int keep = (int)(x.Length * (0.3 + (0.5 * r.NextDouble())));
                Array.Resize(ref x, keep);
                cut = " cut";
            }
            parts.Add(x);
            parts.Add(new float[r.Next(1900, 19200)]);
            what.Add($"WN {wn} {il} K{k} {bits} bits{(eom ? "" : " no EOM")}{cut}");
        }

        float[] clean = new float[parts.Sum(p => p.Length)];
        int at = 0;
        foreach (float[] p in parts)
        {
            p.CopyTo(clean, at);
            at += p.Length;
        }

        Layer3Channel channel = Channels[r.Next(Channels.Length)];
        double snr = 3 + (17 * r.NextDouble());
        double cfo = -40 + (80 * r.NextDouble());
        float level = (float)(0.05 + (1.95 * r.NextDouble()));
        float[] audio = channel.Create(seed).Apply(clean, snr, leadInSamples: 0, leadOutSamples: 4800, frequencyOffsetHz: cfo);
        for (int i = 0; i < audio.Length; i++)
        {
            audio[i] *= level;
        }

        return (audio, $"{channel.Name} {snr:F1} dB {cfo:F1} Hz level {level:F2}: {string.Join(", ", what)}");
    }

    static (List<Burst> CSharp, List<Burst> Cpp) Receive(float[] audio)
    {
        var demod = new Ms110dDemodulator();
        var csharp = new List<Burst>();
        demod.BurstCompleted += b => csharp.Add(new Burst(b.Lock?.WaveformNumber ?? -1,
            b.Lock?.Interleaver ?? Ms110dInterleaverKind.Short, b.Lock?.ConstraintLength ?? 0, b.Reason, b.Blocks,
            b.PayloadBits));
        using var rx = new CppReceiver();
        var cpp = new List<Burst>();
        for (int i = 0; i < audio.Length; i += 192)
        {
            var block = audio.AsSpan(i, Math.Min(192, audio.Length - i));
            demod.Process(block);
            foreach (CppBurst b in rx.Process(block))
            {
                cpp.Add(new Burst(b.Wn, b.Il, b.K, b.Reason, b.Blocks, b.PayloadBits));
            }
        }
        return (csharp, cpp);
    }

    // The first difference between the two lists, or null.
    static string? FirstDifference(List<Burst> csharp, List<Burst> cpp)
    {
        for (int i = 0; i < Math.Max(csharp.Count, cpp.Count); i++)
        {
            string a = i < csharp.Count ? csharp[i].ToString() : "none";
            string b = i < cpp.Count ? cpp[i].ToString() : "none";
            if (a != b)
            {
                return $"burst {i}: C# {a}; C++ {b}";
            }
        }
        return null;
    }

    static int FromEnv(string name, int fallback) =>
        int.TryParse(Environment.GetEnvironmentVariable(name), out int v) && v > 0 ? v : fallback;

    [Fact]
    public void RandomStreamsGiveTheSameBurstsOnBothReceivers()
    {
        int count = FromEnv("MS110D_DIFF_STREAMS", 16);
        int first = FromEnv("MS110D_DIFF_SEED", 1);
        var differences = new System.Collections.Concurrent.ConcurrentBag<(int Seed, string Line)>();
        int csharpBursts = 0, decodedBoth = 0;
        Parallel.For(first, first + count, seed =>
        {
            (float[] audio, string what) = Stream(seed);
            (List<Burst> csharp, List<Burst> cpp) = Receive(audio);
            Interlocked.Add(ref csharpBursts, csharp.Count);
            Interlocked.Add(ref decodedBoth, csharp.Count(b => b.Reason == Ms110dBurstEndReason.Eom));
            if (FirstDifference(csharp, cpp) is string d)
            {
                differences.Add((seed, $"seed {seed} ({what}): {d}"));
            }
        });

        string summary = $"differential: {count} streams from seed {first}, {csharpBursts} C# bursts ({decodedBoth} to their EOM), " +
            $"streams that differ {differences.Count}";
        output.WriteLine(summary);
        Console.WriteLine(summary);
        foreach (var (_, line) in differences.OrderBy(d => d.Seed).Take(20))
        {
            output.WriteLine(line);
            Console.WriteLine("differential: " + line);
        }
        if (!differences.IsEmpty)
        {
            Assert.Fail($"{differences.Count} of {count} streams differ; first: {differences.OrderBy(d => d.Seed).First().Line}");
        }
    }
}
