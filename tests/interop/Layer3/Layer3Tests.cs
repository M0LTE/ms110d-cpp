// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later

using Xunit;

namespace Ms110dInterop.Tests.Layer3;

/// <summary>Runs one cell, writes its CSV and markdown, and applies the pass rule. Shared by the fast and long tests.</summary>
internal static class Layer3Runner
{
    public static Layer3SweepResult RunCell(int wn, int il, int k, string channel, int frames, ITestOutputHelper output,
        IReadOnlyList<ILayer3Transmitter>? txs = null, string? idSuffix = null)
    {
        var (r, shifts) = Layer3Sweep.RunLocated(wn, il, k, Layer3Channel.ByName(channel), frames,
            txs ?? Layer3Endpoints.Transmitters, Layer3Endpoints.Receivers);
        if (shifts > 0)
        {
            output.WriteLine($"ladder moved {shifts} time(s) to bracket the target");
        }

        if (idSuffix is null)
        {
            Layer3Report.Write(r);
        }

        output.WriteLine(Layer3Report.Markdown(r));
        return r;
    }

    /// <summary>The pass rule: every available pairing's frame-loss SNR (10 % AWGN, 20 % fading) within 0.5 dB of the others.</summary>
    public static void AssertAgreement(Layer3SweepResult r)
    {
        var ts = Layer3Report.Thresholds(r);
        Layer3Analysis.Agreement ag = Layer3Analysis.Agree(ts, Layer3Report.ToleranceDb);
        Assert.True(ag.Pass, $"{r.Spec.Label}: {ag.Message}; " + string.Join("; ", ts.Select(t => $"{t.Pairing} {t.SnrDb:F2} dB")));
    }
}

/// <summary>Pure maths of the pass rule, no modems.</summary>
public class Layer3AnalysisTests
{
    [Fact]
    public void WilsonIntervalIsSensible()
    {
        var (lo, hi) = Layer3Analysis.Wilson(10, 100);
        Assert.InRange(lo, 0.055, 0.056);
        Assert.InRange(hi, 0.17, 0.18);
        var (lo0, hi0) = Layer3Analysis.Wilson(0, 50);
        Assert.Equal(0, lo0, 9);
        Assert.InRange(hi0, 0.06, 0.08);
    }

    [Fact]
    public void MonotoneFitPoolsViolations()
    {
        double[] m = Layer3Analysis.MonotoneDecreasing([0.5, 0.2, 0.4, 0.1], [10, 10, 10, 10]);
        Assert.Equal([0.5, 0.3, 0.3, 0.1], m.Select(x => Math.Round(x, 9)));
    }

    [Fact]
    public void CrossingInterpolatesInLogLoss()
    {
        // 40 % at 0 dB and 2.5 % at 2 dB: ln(0.1) is 0.6 of the way down in ln, so 1.2 dB.
        double? c = Layer3Analysis.Crossing([0, 2], [0.4, 0.025], 100, 0.1, out _);
        Assert.NotNull(c);
        Assert.Equal(2 * (Math.Log(0.4) - Math.Log(0.1)) / (Math.Log(0.4) - Math.Log(0.025)), c!.Value, 9);
        Assert.Null(Layer3Analysis.Crossing([0, 2], [0.4, 0.3], 100, 0.1, out string above));
        Assert.Contains("above", above);
        Assert.Null(Layer3Analysis.Crossing([0, 2], [0.05, 0.01], 100, 0.1, out string below));
        Assert.Contains("below", below);
    }

    private static Layer3Point P(string tx, double snr, int lostCount, int n) =>
        new(tx, "csharp", snr, [.. Enumerable.Range(0, n).Select(i => i < lostCount)], 1000, 0);

    private static IReadOnlyList<Layer3Point> Curve(string tx, double shiftDb) =>
        [.. new[] { 0.0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 }.Select(s =>
        {
            // 100 frames; a curve that falls by a factor e every 1.5 dB, shifted right by shiftDb.
            int lost = (int)Math.Clamp(Math.Round(100 * Math.Exp(-(s - shiftDb) / 1.5)), 0, 100);
            return P(tx, s, lost, 100);
        })];

    [Fact]
    public void PassRuleAcceptsCloseCurvesAndRejectsShiftedOnes()
    {
        var a = Layer3Analysis.Threshold("a->x", Curve("a", 0), 0.1);
        var close = Layer3Analysis.Threshold("b->x", Curve("b", 0.3), 0.1);
        var far = Layer3Analysis.Threshold("c->x", Curve("c", 0.8), 0.1);
        Assert.True(Layer3Analysis.Agree([a, close], 0.5).Pass);
        Assert.False(Layer3Analysis.Agree([a, far], 0.5).Pass);
        Assert.False(Layer3Analysis.Agree([a, close, far], 0.5).Pass);
        Assert.InRange(Layer3Analysis.Agree([a, far], 0.5).SpreadDb!.Value, 0.7, 0.9);
    }

    [Fact]
    public void UnbracketedCurveFailsTheRuleInsteadOfPassing()
    {
        var flat = Layer3Analysis.Threshold("a->x", [P("a", 0, 50, 100), P("a", 1, 40, 100)], 0.1);
        Assert.False(Layer3Analysis.Agree([flat, flat], 0.5).Pass);
    }

    [Fact]
    public void PairedDifferenceSeesAShiftAndAnIdentityIsZero()
    {
        var a = Curve("a", 0);
        var same = Layer3Analysis.PairedDifference(a, a, 0.1);
        Assert.Equal(0, same.Diff!.Value, 9);
        Assert.Equal(0, same.Low!.Value, 9);
        Assert.Equal(0, same.High!.Value, 9);
        var shifted = Layer3Analysis.PairedDifference(a, Curve("b", 1), 0.1);
        Assert.InRange(shifted.Diff!.Value, 0.9, 1.1);
    }

    [Fact]
    public void BitErrorsCountMissingAndExtraBits()
    {
        Assert.Equal(4, Layer3Sweep.BitErrors([0, 1, 0, 1], null));
        Assert.Equal(0, Layer3Sweep.BitErrors([0, 1], [0, 1]));
        Assert.Equal(1, Layer3Sweep.BitErrors([0, 1], [0, 0]));
        Assert.Equal(2, Layer3Sweep.BitErrors([0, 1, 1], [0]));
    }

    [Fact]
    public void TheCppReceiverSlotIsReportedAsNotAvailableAndItsPairingsAreSkipped()
    {
        Assert.False(Layer3Endpoints.CppReceiver.IsAvailable);
        var spec = new Layer3Spec(2, 1, 7, Layer3Channel.Awgn, [10], 2);
        var r = Layer3Sweep.Run(spec, Layer3Endpoints.Transmitters, [Layer3Endpoints.CSharpRx, new MissingReceiver("cpp", "not yet")]);
        Assert.Equal(["csharp->csharp", "cpp->csharp"], r.Pairings);
        Assert.Equal(2, r.Skipped.Count);
        Assert.All(r.Skipped, s => Assert.Contains("not yet available", s));
    }

    [Fact]
    public void SeedsAreIdenticalForEveryPairingAndDifferentBetweenChannels()
    {
        var a = new Layer3Spec(2, 1, 7, Layer3Channel.Poor, [0], 4);
        var b = new Layer3Spec(2, 1, 7, Layer3Channel.Good, [5], 4);
        Assert.Equal(a.PayloadSeed(3), b.PayloadSeed(3));
        Assert.NotEqual(a.ChannelSeed(3), b.ChannelSeed(3));
        Assert.NotEqual(a.ChannelSeed(2), a.ChannelSeed(3));
    }
}

/// <summary>
/// The default run: a few modes on AWGN and on the fading channels, short interleaver, enough frames per
/// point for the threshold to carry a useful interval (printed with every
/// result). C# to C# and C++ to C# must agree within 0.5 dB.
/// </summary>
[Trait("Category", "Layer3")]
public class Layer3FastTests(ITestOutputHelper output)
{
    public static TheoryData<int, int, int, string, int> Cases() => new()
    {
        { 2, 1, 7, "awgn", 40 },
        { 2, 1, 7, "moderate", 64 },
    };

    [Theory]
    [MemberData(nameof(Cases))]
    public void CppTransmitterMatchesCSharpTransmitterThroughTheChannel(int wn, int il, int k, string channel, int frames)
    {
        Assert.SkipWhen(Layer3LongTests.Enabled, "the long run covers these cells");
        Layer3SweepResult r = Layer3Runner.RunCell(wn, il, k, channel, frames, output);
        Layer3Runner.AssertAgreement(r);
    }
}

/// <summary>
/// A transmitter that is wrong in a way the harness must see: its audio carries its own noise, 3 dB below the
/// signal. If the pass rule could not tell, a pass on the real C++ transmitter would mean nothing.
/// </summary>
[Trait("Category", "Layer3")]
public class Layer3SensitivityTests(ITestOutputHelper output)
{
    private sealed class NoisyTransmitter : ILayer3Transmitter
    {
        public string Name => "cpp-noisy";

        public float[] Modulate(M0LTE.Ms110d.Ms110dTxSettings settings, byte[] payload)
        {
            float[] a = Layer3Endpoints.CppTx.Modulate(settings, payload);
            double power = a.Sum(x => (double)x * x) / a.Length;
            double sigma = Math.Sqrt(power / 2); // 3 dB below the signal
            var rng = new Random(payload.Length ^ (payload.Length > 0 ? payload[0] : 0) ^ a.Length);
            var o = new float[a.Length];
            for (int i = 0; i < a.Length; i++)
            {
                double g = Math.Sqrt(-2 * Math.Log(1 - rng.NextDouble())) * Math.Cos(2 * Math.PI * rng.NextDouble());
                o[i] = a[i] + (float)(sigma * g);
            }

            return o;
        }
    }

    [Fact]
    public void ThePassRuleFailsForADegradedTransmitter()
    {
        Assert.SkipWhen(Layer3LongTests.Enabled, "default run only");
        var (r, _) = Layer3Sweep.RunLocated(2, 1, 7, Layer3Channel.Awgn, 32, [Layer3Endpoints.CSharpTx, new NoisyTransmitter()], [Layer3Endpoints.CSharpRx]);
        output.WriteLine(Layer3Report.Markdown(r));
        var ag = Layer3Analysis.Agree(Layer3Report.Thresholds(r), Layer3Report.ToleranceDb);
        Assert.False(ag.Pass, "a transmitter with its own noise at -3 dB should have been caught: " + ag.Message);
    }
}

/// <summary>
/// The full matrix: every waveform, interleaver and constraint length on all four channels. Hours of CPU, so it
/// is gated: MS110D_LAYER3_LONG=1, with MS110D_LAYER3_SHARD=i/n to take every n-th cell starting at i, and
/// MS110D_LAYER3_CHANNELS, MS110D_LAYER3_WN, MS110D_LAYER3_IL to narrow it. Frames per point:
/// 60 on AWGN, 160 on the fading channels (MS110D_LAYER3_FRAMES overrides both).
/// </summary>
[Trait("Category", "Layer3")]
public class Layer3LongTests(ITestOutputHelper output)
{
    internal static bool Enabled => Environment.GetEnvironmentVariable("MS110D_LAYER3_LONG") == "1";

    private static HashSet<string>? Filter(string name) =>
        Environment.GetEnvironmentVariable(name) is { Length: > 0 } v ? [.. v.Split(',')] : null;

    public static IEnumerable<(int Wn, int Il, int K, string Channel)> Cells()
    {
        HashSet<string>? chans = Filter("MS110D_LAYER3_CHANNELS");
        HashSet<string>? wns = Filter("MS110D_LAYER3_WN");
        HashSet<string>? ils = Filter("MS110D_LAYER3_IL");
        HashSet<string>? ks = Filter("MS110D_LAYER3_K");
        int shard = 0, shards = 1;
        if (Environment.GetEnvironmentVariable("MS110D_LAYER3_SHARD") is { Length: > 0 } sh)
        {
            string[] p = sh.Split('/');
            shard = int.Parse(p[0]);
            shards = int.Parse(p[1]);
        }

        var cells = new List<(int Wn, int Il, int K, string Channel, long Cost)>();
        foreach (Layer3Channel ch in Layer3Channel.All)
        {
            foreach (var (wn, il, k) in TxCrossCheckTests.AllCombinations())
            {
                if ((chans?.Contains(ch.Name) ?? true) && (wns?.Contains(wn.ToString()) ?? true)
                    && (ils?.Contains(il.ToString()) ?? true) && (ks?.Contains(k.ToString()) ?? true))
                {
                    // Decode time grows with the block length, and fading channels take more frames.
                    long cost = (long)Ms110dBlockBits(wn, il) * (ch.Fading ? 3 : 1);
                    cells.Add((wn, il, k, ch.Name, cost));
                }
            }
        }

        // Deal the dearest cells out first, back and forth, so every shard gets a similar load.
        int n = 0;
        foreach (var c in cells.OrderByDescending(c => c.Cost).ThenBy(c => c.Wn).ThenBy(c => c.Il).ThenBy(c => c.K).ThenBy(c => c.Channel))
        {
            int round = n / shards;
            int slot = n % shards;
            if ((round % 2 == 0 ? slot : shards - 1 - slot) == shard)
            {
                yield return (c.Wn, c.Il, c.K, c.Channel);
            }

            n++;
        }
    }

    private static int Ms110dBlockBits(int wn, int il) =>
        M0LTE.Ms110d.Ms110dInterleaverParams.Get3k(wn, (M0LTE.Ms110d.Ms110dInterleaverKind)il).InputBits;

    public static TheoryData<int, int, int, string> Matrix()
    {
        var d = new TheoryData<int, int, int, string>();
        foreach (var (wn, il, k, ch) in Cells())
        {
            d.Add(wn, il, k, ch);
        }

        if (d.Count == 0)
        {
            d.Add(-1, 0, 0, "none"); // an empty theory is an error; the test skips this row
        }

        return d;
    }

    [Theory]
    [MemberData(nameof(Matrix))]
    public void CppTransmitterMatchesCSharpTransmitterThroughTheChannel(int wn, int il, int k, string channel)
    {
        Assert.SkipUnless(Enabled && wn >= 0, "set MS110D_LAYER3_LONG=1 for the full matrix");
        int frames = int.TryParse(Environment.GetEnvironmentVariable("MS110D_LAYER3_FRAMES"), out int f) ? f : channel == "awgn" ? 60 : 160;
        Layer3SweepResult r = Layer3Runner.RunCell(wn, il, k, channel, frames, output);
        Layer3Runner.AssertAgreement(r);
    }
}
