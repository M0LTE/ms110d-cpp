// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later

using M0LTE.Ms110d;
using Xunit;

namespace Ms110dInterop.Tests;

// The C# 0.4.0 acquisition scenarios (its Ms110dAcquisitionTests: issues 8, 9 and 10 in
// M0LTE/M0LTE.Ms110d) built the same way, run through the pinned C# receiver and the C++ receiver,
// fed in 20 ms blocks. Each test asserts what the C# test asserts, on both receivers, and that the
// two give the same bursts: waveform, interleaver, K, end reason, block count and payload.
public class AcquisitionTests
{
    static byte[] RandomBits(int count, int seed)
    {
        var random = new Random(seed);
        var bits = new byte[count];
        for (int i = 0; i < bits.Length; i++)
        {
            bits[i] = (byte)random.Next(2);
        }
        return bits;
    }

    static void AddNoise(float[] audio, double sigma, int seed)
    {
        var random = new Random(seed);
        for (int i = 0; i < audio.Length; i += 2)
        {
            double u1 = 1.0 - random.NextDouble(), u2 = random.NextDouble();
            double r = Math.Sqrt(-2 * Math.Log(u1));
            audio[i] += (float)(sigma * r * Math.Cos(2 * Math.PI * u2));
            if (i + 1 < audio.Length)
            {
                audio[i + 1] += (float)(sigma * r * Math.Sin(2 * Math.PI * u2));
            }
        }
    }

    static double Rms(float[] x)
    {
        double sum = 0;
        int n = 0;
        foreach (float v in x)
        {
            if (v != 0)
            {
                sum += v * v;
                n++;
            }
        }
        return Math.Sqrt(sum / Math.Max(1, n));
    }

    static float[] Concat(params float[][] parts)
    {
        var result = new float[parts.Sum(p => p.Length)];
        int at = 0;
        foreach (float[] part in parts)
        {
            part.CopyTo(result, at);
            at += part.Length;
        }
        return result;
    }

    sealed record Burst(int Wn, Ms110dInterleaverKind Il, int K, Ms110dBurstEndReason Reason, int Blocks, byte[] Payload)
    {
        public override string ToString() =>
            $"WN {Wn} {Il} K{K} {Reason} blocks {Blocks} payload {Payload.Length} bits";
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

    static void SameBursts(List<Burst> csharp, List<Burst> cpp)
    {
        Assert.Equal(csharp.Count, cpp.Count);
        for (int i = 0; i < csharp.Count; i++)
        {
            Assert.Equal(csharp[i].ToString(), cpp[i].ToString());
            Assert.Equal(csharp[i].Payload, cpp[i].Payload);
        }
    }

    [Fact]
    public void A_Rejected_Earliest_Echo_Is_Read_Again_At_The_Strongest_Peak()
    {
        // C# issue 8: two static paths 4.2 ms apart, the earliest 4.2 dB weaker, 2 dB SNR. The read
        // at the earliest echo fails; read again at the strongest peak, the burst decodes.
        const int seed = 3;
        const double early = 0.62;
        const int delay = 40;
        var tx = new Ms110dModulator(new Ms110dTxSettings { WaveformNumber = 5, PreambleSuperframes = 2 });
        byte[] payload = RandomBits(800, 77 + seed);
        float[] x = tx.Modulate(payload);
        var twoPath = new float[x.Length + delay];
        for (int i = 0; i < x.Length; i++)
        {
            twoPath[i] += (float)(early * x[i]);
            twoPath[i + delay] += x[i];
        }
        var audio = new float[3000 + twoPath.Length + 9600];
        twoPath.CopyTo(audio, 3000);
        AddNoise(audio, Rms(twoPath) * Math.Pow(10, -2.0 / 20), 1000 + seed);

        (List<Burst> csharp, List<Burst> cpp) = Receive(audio);

        foreach (List<Burst> bursts in new[] { csharp, cpp })
        {
            Burst eom = Assert.Single(bursts, b => b.Reason == Ms110dBurstEndReason.Eom);
            Assert.Equal(payload, eom.Payload);
        }
        SameBursts(csharp, cpp);
    }

    [Fact]
    public void A_Long_Interleaver_Burst_After_A_Short_One_Decodes_On_The_Same_Receiver()
    {
        // C# issue 9 (a crash in the C#; the C++ sizes its buffers for the longest block).
        byte[] first = RandomBits(1200, 9);
        byte[] second = RandomBits(1200, 10);
        float[] audio = Concat(
            new float[3000],
            new Ms110dModulator(new Ms110dTxSettings { WaveformNumber = 7 }).Modulate(first),
            new float[9600],
            new Ms110dModulator(new Ms110dTxSettings { WaveformNumber = 7, Interleaver = Ms110dInterleaverKind.Long })
                .Modulate(second),
            new float[9600]);

        (List<Burst> csharp, List<Burst> cpp) = Receive(audio);

        foreach (List<Burst> bursts in new[] { csharp, cpp })
        {
            Assert.Equal(2, bursts.Count);
            Assert.Equal(Ms110dBurstEndReason.Eom, bursts[0].Reason);
            Assert.Equal(first, bursts[0].Payload);
            Assert.Equal(Ms110dBurstEndReason.Eom, bursts[1].Reason);
            Assert.Equal(second, bursts[1].Payload);
        }
        SameBursts(csharp, cpp);
    }

    [Fact]
    public void A_Wrong_Waveform_Id_Does_Not_Stop_The_Receiver()
    {
        // C# issue 9, the on-air shape: a WN 13 burst whose preamble says WN 7 Long, between two
        // ordinary bursts.
        const int preambleSamples = 3 * 576 * 4;
        byte[] first = RandomBits(1200, 21);
        byte[] last = RandomBits(800, 22);
        float[] wn13 = new Ms110dModulator(new Ms110dTxSettings { WaveformNumber = 13 }).Modulate(RandomBits(30000, 23));
        float[] wrongPreamble = new Ms110dModulator(new Ms110dTxSettings
        {
            WaveformNumber = 7,
            Interleaver = Ms110dInterleaverKind.Long,
        }).Modulate(RandomBits(64, 24));
        Array.Copy(wrongPreamble, wn13, preambleSamples);
        float[] audio = Concat(
            new float[3000],
            new Ms110dModulator(new Ms110dTxSettings { WaveformNumber = 7 }).Modulate(first),
            new float[9600],
            wn13,
            new float[9600],
            new Ms110dModulator(new Ms110dTxSettings { WaveformNumber = 4 }).Modulate(last),
            new float[9600]);
        AddNoise(audio, 0.01, 5);

        (List<Burst> csharp, List<Burst> cpp) = Receive(audio);

        foreach (List<Burst> bursts in new[] { csharp, cpp })
        {
            Assert.Contains(bursts, b => b.Reason == Ms110dBurstEndReason.Eom && b.Payload.SequenceEqual(first));
            Assert.Contains(bursts, b => b.Reason == Ms110dBurstEndReason.Eom && b.Payload.SequenceEqual(last));
        }
        SameBursts(csharp, cpp);
    }

    [Fact]
    public void A_New_Preamble_Ends_A_Burst_That_Lost_Its_Eom()
    {
        // C# issue 10: the first burst is cut off half way and the next starts 1 s later. The
        // background search finds the new preamble and lets go of the dead burst.
        var tx = new Ms110dModulator(new Ms110dTxSettings { WaveformNumber = 4 });
        float[] cut = tx.Modulate(RandomBits(4000, 31));
        Array.Resize(ref cut, cut.Length / 2);
        byte[] second = RandomBits(800, 32);
        float[] audio = Concat(new float[3000], cut, new float[9600], tx.Modulate(second), new float[9600]);
        AddNoise(audio, 0.005, 6);

        (List<Burst> csharp, List<Burst> cpp) = Receive(audio);

        foreach (List<Burst> bursts in new[] { csharp, cpp })
        {
            Assert.Equal(2, bursts.Count);
            Assert.Equal(Ms110dBurstEndReason.NewPreamble, bursts[0].Reason);
            Assert.Equal(Ms110dBurstEndReason.Eom, bursts[1].Reason);
            Assert.Equal(second, bursts[1].Payload);
        }
        SameBursts(csharp, cpp);
    }

    [Theory]
    [InlineData(0)]
    [InlineData(1)]
    [InlineData(3)]
    [InlineData(5)]
    [InlineData(6)]
    [InlineData(7)]
    [InlineData(8)]
    [InlineData(13)]
    public void A_Burst_Without_A_New_Preamble_Is_Not_Let_Go(int wn)
    {
        // C# issue 10, the other side: nothing in a long burst's data may pass for a new preamble.
        var tx = new Ms110dModulator(new Ms110dTxSettings { WaveformNumber = wn });
        byte[] payload = RandomBits(wn == 0 ? 600 : 6000, 40 + wn);
        float[] audio = Concat(new float[3000], tx.Modulate(payload), new float[9600]);
        AddNoise(audio, 0.003, 50 + wn);

        (List<Burst> csharp, List<Burst> cpp) = Receive(audio);

        foreach (List<Burst> bursts in new[] { csharp, cpp })
        {
            Burst only = Assert.Single(bursts);
            Assert.Equal(Ms110dBurstEndReason.Eom, only.Reason);
            Assert.Equal(payload, only.Payload);
        }
        SameBursts(csharp, cpp);
    }
}
