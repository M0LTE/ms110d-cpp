// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later

using Xunit;

namespace Ms110dInterop.Tests.Layer3;

/// <summary>The cells of the default run and their frame counts (docs/dev/layer3.md says why these).</summary>
internal static class Layer3Plan
{
    // The real C++ transmitter's audio is the C# transmitter's to within 1e-6, so its pairing comes out
    // frame for frame the same and needs few frames; these cells exist to prove it on each channel type.
    public static readonly (int Wn, int Il, int K)[] AwgnCells = [(3, 0, 7), (4, 0, 7)];

    public const int AwgnFrames = 24;

    public static readonly (int Wn, int Il, int K)[] ModerateCells = [(4, 0, 7)];

    public const int ModerateFrames = 48;

    // The sensitivity experiment is what needs power: receivers a known amount worse must be told apart from
    // equal ones, so it takes more cells and frames (see the validation table in docs/dev/layer3.md).
    public static readonly (int Wn, int Il, int K)[] SensitivityCells = [(3, 0, 7), (4, 0, 7), (6, 0, 7)];

    public const int SensitivityFrames = 40;

    public static readonly (int Wn, int Il, int K)[] ModerateValidationCells = [(3, 0, 7), (4, 0, 7), (6, 0, 7), (2, 1, 7)];

    public const int ModerateValidationFrames = 96;
}

/// <summary>Runs one cell, writes its CSV and markdown, and applies the cell rule. Shared by the fast and long tests.</summary>
internal static class Layer3Runner
{
    public static Layer3SweepResult RunCell(int wn, int il, int k, string channel, int frames, ITestOutputHelper output)
    {
        Layer3SweepResult r = Layer3Sweep.RunLocated(wn, il, k, Layer3Channel.ByName(channel), frames,
            Layer3Endpoints.Transmitters, Layer3Endpoints.Receivers);
        Layer3Report.Write(r);
        output.WriteLine(Layer3Report.Markdown(r));
        return r;
    }

    /// <summary>The cell rule: fail only when a pairing is provably worse than the reference (its paired interval
    /// wholly above +0.5 dB). Inconclusive and not-reached cells do not fail; they are in the report.</summary>
    public static void AssertCellNotFailed(Layer3SweepResult r)
    {
        var failed = Layer3Report.Compare(r).Where(c => c.Verdict == Layer3Analysis.Verdict.Fail).ToList();
        Assert.True(failed.Count == 0, $"{r.Spec.Label}: " + string.Join("; ", failed.Select(c =>
            $"{c.Pairing} is {c.Diff:F2} dB worse than {c.Reference} (95% interval {c.Low:F2} to {c.High:F2} dB, limit +0.5)")));
    }

    public static Layer3Analysis.AggregateResult AggregateOf(IEnumerable<Layer3SweepResult> cells, string pairing)
    {
        var pairs = new List<Layer3Analysis.CellPair>();
        foreach (Layer3SweepResult r in cells.Where(x => x.Reached))
        {
            string reference = r.Pairings.First();
            if (r.Pairings.Contains(pairing))
            {
                pairs.Add(new(r.Spec.Id, r.Curve(reference), r.Curve(pairing), Layer3Report.TargetFor(r.Spec.Channel)));
            }
        }

        return Layer3Analysis.Aggregate(pairs, Layer3Report.ToleranceDb);
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
    public void LogisticFitRecoversTheThresholdOfAnExactCurve()
    {
        // logit(loss) = -1.2 (snr - 3): the 10 % point is at 3 + ln(9)/1.2.
        double[] snr = [0, 1, 2, 3, 4, 5, 6];
        double[] n = [.. snr.Select(_ => 100000.0)];
        double[] lost = [.. snr.Select(s => 100000.0 / (1 + Math.Exp(1.2 * (s - 3))))];
        double? t = Layer3Analysis.FitThreshold(snr, lost, n, 0.1, out double? se, out _);
        Assert.NotNull(t);
        Assert.Equal(3 + (Math.Log(9) / 1.2), t!.Value, 2);
        Assert.InRange(se!.Value, 0, 0.01);
    }

    [Fact]
    public void FitReportsWhenTheLadderDoesNotBracketTheTarget()
    {
        double[] snr = [0, 1, 2, 3];
        double[] n = [100, 100, 100, 100];
        Assert.Null(Layer3Analysis.FitThreshold(snr, [90, 80, 70, 60], n, 0.1, out _, out string above));
        Assert.Contains("above", above);
        Assert.Null(Layer3Analysis.FitThreshold(snr, [20, 10, 4, 1], n, 0.5, out _, out string below));
        Assert.Contains("below", below);
        Assert.Null(Layer3Analysis.FitThreshold(snr, [10, 12, 9, 11], n, 0.1, out _, out _));
    }

    [Fact]
    public void FitIsStableWhenThePointsAreAllZeroOrAll100Percent()
    {
        double? t = Layer3Analysis.FitThreshold([0, 0.5, 1, 1.5, 2, 2.5, 3], [40, 40, 40, 0, 0, 0, 0], [40, 40, 40, 40, 40, 40, 40], 0.1, out _, out _);
        Assert.NotNull(t);
        Assert.InRange(t!.Value, 1.0, 2.0);
    }

    private static Layer3Point P(string tx, double snr, int lostCount, int n) =>
        new(tx, "csharp", snr, [.. Enumerable.Range(0, n).Select(i => i < lostCount)], 1000, 0);

    private static IReadOnlyList<Layer3Point> Curve(string tx, double shiftDb) =>
        [.. new[] { 0.0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 }.Select(s =>
        {
            // 100 frames; a logistic curve of slope 1.2 per dB centred on 3 dB, shifted right by shiftDb.
            int lost = (int)Math.Round(100 / (1 + Math.Exp(1.2 * (s - shiftDb - 3))));
            return P(tx, s, lost, 100);
        })];

    [Fact]
    public void JudgeNeedsTheWholeIntervalAboveToFailAndWithinToPass()
    {
        Assert.Equal(Layer3Analysis.Verdict.Pass, Layer3Analysis.Judge(0.1, -0.2, 0.4, 0.5));
        Assert.Equal(Layer3Analysis.Verdict.Fail, Layer3Analysis.Judge(0.9, 0.6, 1.2, 0.5));
        Assert.Equal(Layer3Analysis.Verdict.Inconclusive, Layer3Analysis.Judge(0.7, 0.2, 1.2, 0.5));
        Assert.Equal(Layer3Analysis.Verdict.Inconclusive, Layer3Analysis.Judge(-0.7, -1.2, -0.6, 0.5));
        Assert.Equal(Layer3Analysis.Verdict.Inconclusive, Layer3Analysis.Judge(0.3, null, null, 0.5));
        Assert.Equal(Layer3Analysis.Verdict.NotReached, Layer3Analysis.Judge(null, null, null, 0.5));
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
    public void AggregateOfIdenticalCellsPassesAndOfShiftedCellsFails()
    {
        var a = Curve("a", 0);
        var same = Layer3Analysis.Aggregate([new("x", a, a, 0.1), new("y", a, a, 0.1)], 0.5);
        Assert.Equal(Layer3Analysis.Verdict.Pass, same.Verdict);
        var far = Layer3Analysis.Aggregate([new("x", a, Curve("b", 1.5), 0.1), new("y", a, Curve("c", 1.4), 0.1)], 0.5);
        Assert.Equal(Layer3Analysis.Verdict.Fail, far.Verdict);
        Assert.InRange(far.Mean!.Value, 1.3, 1.6);
        var none = Layer3Analysis.Aggregate([new("x", [P("a", 0, 50, 100), P("a", 1, 40, 100)], a, 0.1)], 0.5);
        Assert.Equal(Layer3Analysis.Verdict.NotReached, none.Verdict);
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
        var spec = new Layer3Spec(3, 0, 7, Layer3Channel.Awgn, [10], 2);
        var r = Layer3Sweep.Run(spec, Layer3Endpoints.Transmitters, [Layer3Endpoints.CSharpRx, new MissingReceiver("cpp", "not yet")]);
        Assert.Equal(["csharp->csharp", "cpp->csharp"], r.Pairings);
        Assert.Equal(2, r.Skipped.Count);
        Assert.All(r.Skipped, s => Assert.Contains("not yet available", s));
    }

    private sealed class ShortTransmitter : ILayer3Transmitter
    {
        public string Name => "short";

        public float[] Modulate(M0LTE.Ms110d.Ms110dTxSettings settings, byte[] payload) =>
            Layer3Endpoints.CSharpTx.Modulate(settings, payload)[..^1];
    }

    [Fact]
    public void TransmittersWithDifferentAudioLengthsFailLoudly()
    {
        var spec = new Layer3Spec(3, 0, 7, Layer3Channel.Awgn, [10], 1);
        var ex = Assert.Throws<InvalidOperationException>(() => Layer3Sweep.Run(spec, [Layer3Endpoints.CSharpTx, new ShortTransmitter()], [Layer3Endpoints.CSharpRx]));
        Assert.Contains("different audio lengths", ex.Message);
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

    [Fact]
    public void TheLadderIsCentredOnTheCrossing()
    {
        double[] awgn = Layer3Sweep.CentredLadder(3.2, Layer3Channel.Awgn);
        Assert.Equal(7, awgn.Length);
        Assert.Equal(3.0, awgn[3]);
        Assert.Equal(1.5, awgn[0]);
        double[] fading = Layer3Sweep.CentredLadder(6.0, Layer3Channel.Poor);
        Assert.Equal([3.0, 4, 5, 6, 7, 8, 9], fading);
    }
}

/// <summary>
/// The default run: a few cheap modes on AWGN and Moderate, short interleavers. Per cell, the C++ transmitter's
/// pairing must not be provably worse than the C# one; per channel, the aggregate must not be either.
/// </summary>
[Trait("Category", "Layer3")]
public class Layer3FastTests(ITestOutputHelper output)
{
    private void Channel(string channel, (int Wn, int Il, int K)[] cells, int frames)
    {
        Assert.SkipWhen(Layer3LongTests.Enabled || Layer3ValidationTests.Enabled, "the long or validation run is selected");
        var results = cells.Select(c => Layer3Runner.RunCell(c.Wn, c.Il, c.K, channel, frames, output)).ToList();
        foreach (Layer3SweepResult r in results)
        {
            Layer3Runner.AssertCellNotFailed(r);
        }

        foreach (string pairing in results[0].Pairings.Skip(1))
        {
            var agg = Layer3Runner.AggregateOf(results, pairing);
            output.WriteLine($"{channel} aggregate, {pairing}: {agg.UsedCells} of {agg.Cells} cells, mean difference {agg.Mean:F2} dB, 95% interval {agg.Low:F2} to {agg.High:F2} dB, {agg.Verdict}");
            Assert.NotEqual(Layer3Analysis.Verdict.Fail, agg.Verdict);
        }
    }

    [Fact]
    public void AwgnCppTransmitterAgainstCSharpTransmitter() => Channel("awgn", Layer3Plan.AwgnCells, Layer3Plan.AwgnFrames);

    [Fact]
    public void ModerateCppTransmitterAgainstCSharpTransmitter() => Channel("moderate", Layer3Plan.ModerateCells, Layer3Plan.ModerateFrames);
}

/// <summary>
/// Receivers that are a known amount worse (the C# receiver plus extra noise) at the counts of the default run, one
/// repetition: the aggregate rule must pass 0.2 dB and fail 1.0 dB. The reliability over 20 repetitions is in
/// docs/dev/layer3.md (Layer3ValidationTests regenerates it).
/// </summary>
[Trait("Category", "Layer3")]
public class Layer3SensitivityTests(ITestOutputHelper output)
{
    [Fact]
    public void TheAggregateRulePassesTwoTenthsOfADbAndFailsOneDb()
    {
        Assert.SkipWhen(Layer3LongTests.Enabled || Layer3ValidationTests.Enabled, "default run only");
        var prepared = Layer3Fakes.Prepare("awgn", Layer3Plan.SensitivityCells, Layer3Plan.SensitivityFrames);
        var agg = Layer3Fakes.OneRepeat(prepared, runSeed: 1, Layer3Fakes.Levels[1..]);
        foreach (var (name, a) in agg)
        {
            output.WriteLine($"{name}: mean difference {a.Mean:F2} dB, 95% interval {a.Low:F2} to {a.High:F2} dB, {a.Verdict}");
        }

        Assert.NotEqual(Layer3Analysis.Verdict.Fail, agg[Layer3Fakes.Levels[1].Name].Verdict);
        Assert.Equal(Layer3Analysis.Verdict.Fail, agg[Layer3Fakes.Levels[2].Name].Verdict);
    }
}

/// <summary>Helpers shared by the sensitivity test and the validation table.</summary>
internal static class Layer3Fakes
{
    /// <summary>Extra noise levels: decoder jitter (0.002 of the channel noise, 0.009 dB: a true zero, but not a
    /// bit-identical decode), 0.2 dB and 1.0 dB.</summary>
    public static readonly (string Name, double Fraction)[] Levels =
    [
        ("csharp+0.01dB", 0.002),
        ("csharp+0.2dB", ExtraNoiseReceiver.FractionFor(0.2)),
        ("csharp+1.0dB", ExtraNoiseReceiver.FractionFor(1.0)),
    ];

    public sealed record Prepared(Layer3SweepResult Reference);

    /// <summary>The reference (C# to C#) curves for each cell, on a ladder centred on its target.</summary>
    public static List<Prepared> Prepare(string channel, IEnumerable<(int Wn, int Il, int K)> cells, int frames)
    {
        var list = new List<Prepared>();
        foreach (var (wn, il, k) in cells)
        {
            Layer3SweepResult r = Layer3Sweep.RunLocated(wn, il, k, Layer3Channel.ByName(channel), frames, [Layer3Endpoints.CSharpTx], [Layer3Endpoints.CSharpRx]);
            if (r.Reached)
            {
                list.Add(new Prepared(r));
            }
        }

        return list;
    }

    /// <summary>One repetition: every fake receiver decodes the same noisy audio (different noise per run seed), and the
    /// aggregate verdict of each against the reference is returned.</summary>
    public static Dictionary<string, Layer3Analysis.AggregateResult> OneRepeat(List<Prepared> cells, int runSeed, (string Name, double Fraction)[]? levels = null)
    {
        levels ??= Levels;
        ILayer3Receiver[] fakes = [.. levels.Select(l => new ExtraNoiseReceiver(Layer3Endpoints.CSharpRx, l.Name, l.Fraction, runSeed))];
        var pairs = levels.ToDictionary(l => l.Name, _ => new List<Layer3Analysis.CellPair>());
        foreach (Prepared c in cells)
        {
            Layer3SweepResult r = Layer3Sweep.Run(c.Reference.Spec, [Layer3Endpoints.CSharpTx], fakes);
            foreach (var l in levels)
            {
                pairs[l.Name].Add(new(c.Reference.Spec.Id, c.Reference.Curve("csharp->csharp"), r.Curve("csharp->" + l.Name),
                    Layer3Report.TargetFor(c.Reference.Spec.Channel)));
            }
        }

        return levels.ToDictionary(l => l.Name, l => Layer3Analysis.Aggregate(pairs[l.Name], Layer3Report.ToleranceDb));
    }
}

/// <summary>
/// Regenerates the validation table in docs/dev/layer3.md: repeats the sensitivity experiment with different
/// noise seeds and counts how often the aggregate rule fails each fake receiver. MS110D_LAYER3_VALIDATE=1;
/// MS110D_LAYER3_REPS (default 20); MS110D_LAYER3_VALIDATE_CHANNELS (default awgn,moderate); frames and cells as in
/// the sensitivity test (AWGN) or the Moderate validation plan unless MS110D_LAYER3_VALIDATE_FRAMES / _CELLS ("3.0.7;4.0.7") say otherwise.
/// </summary>
[Trait("Category", "Layer3")]
public class Layer3ValidationTests(ITestOutputHelper output)
{
    internal static bool Enabled => Environment.GetEnvironmentVariable("MS110D_LAYER3_VALIDATE") == "1";

    [Fact]
    public void RuleReliability()
    {
        Assert.SkipUnless(Enabled, "set MS110D_LAYER3_VALIDATE=1");
        int reps = int.TryParse(Environment.GetEnvironmentVariable("MS110D_LAYER3_REPS"), out int rp) ? rp : 20;
        string[] channels = (Environment.GetEnvironmentVariable("MS110D_LAYER3_VALIDATE_CHANNELS") ?? "awgn,moderate").Split(',');
        var sb = new System.Text.StringBuilder();
        foreach (string ch in channels)
        {
            var cells = Environment.GetEnvironmentVariable("MS110D_LAYER3_VALIDATE_CELLS") is { Length: > 0 } cs
                ? cs.Split(';').Select(x => x.Split('.')).Select(a => (int.Parse(a[0]), int.Parse(a[1]), int.Parse(a[2]))).ToArray()
                : ch == "awgn" ? Layer3Plan.SensitivityCells : Layer3Plan.ModerateValidationCells;
            int frames = int.TryParse(Environment.GetEnvironmentVariable("MS110D_LAYER3_VALIDATE_FRAMES"), out int fr) ? fr
                : ch == "awgn" ? Layer3Plan.SensitivityFrames : Layer3Plan.ModerateValidationFrames;
            var prepared = Layer3Fakes.Prepare(ch, cells, frames);
            var tally = Layer3Fakes.Levels.ToDictionary(l => l.Name, _ => new List<Layer3Analysis.AggregateResult>());
            for (int rep = 0; rep < reps; rep++)
            {
                foreach (var (name, a) in Layer3Fakes.OneRepeat(prepared, runSeed: 1000 + rep))
                {
                    tally[name].Add(a);
                }

                output.WriteLine($"{ch} repeat {rep + 1}/{reps} done");
            }

            sb.AppendLine($"{ch}: {prepared.Count} cells x {frames} frames per point, {reps} repetitions");
            sb.AppendLine();
            sb.AppendLine("| extra noise | FAIL | PASS (interval within +/-0.5) | INCONCLUSIVE | mean of mean difference (dB) | mean interval width (dB) |");
            sb.AppendLine("| --- | ---: | ---: | ---: | ---: | ---: |");
            foreach (var l in Layer3Fakes.Levels)
            {
                var t = tally[l.Name];
                int Count(Layer3Analysis.Verdict v) => t.Count(x => x.Verdict == v);
                sb.AppendLine($"| {l.Name.Replace("csharp", "").Trim('+')} | {Count(Layer3Analysis.Verdict.Fail)}/{reps} | {Count(Layer3Analysis.Verdict.Pass)}/{reps} | {Count(Layer3Analysis.Verdict.Inconclusive)}/{reps} | {t.Where(x => x.Mean.HasValue).Average(x => x.Mean!.Value):F2} | {t.Where(x => x.Low.HasValue).Average(x => x.High!.Value - x.Low!.Value):F2} |");
            }

            sb.AppendLine();
        }

        string file = Path.Combine(Layer3Report.OutDir(), "validation.md");
        File.WriteAllText(file, sb.ToString());
        output.WriteLine(sb.ToString());
    }
}

/// <summary>
/// The full matrix: every waveform, interleaver and constraint length on all four channels. Hours of CPU, so it
/// is gated: MS110D_LAYER3_LONG=1, with MS110D_LAYER3_SHARD=i/n to take cells dealt out by cost, and
/// MS110D_LAYER3_CHANNELS, MS110D_LAYER3_WN, MS110D_LAYER3_IL, MS110D_LAYER3_K to narrow it. Frames per point:
/// 60 on AWGN, 160 on the fading channels (MS110D_LAYER3_FRAMES overrides both). Each cell fails only if a pairing
/// is provably worse; the verdict on a channel model is the aggregate over its cells (Layer3CombineTests).
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
    public void CppTransmitterAgainstCSharpTransmitterThroughTheChannel(int wn, int il, int k, string channel)
    {
        Assert.SkipUnless(Enabled && wn >= 0, "set MS110D_LAYER3_LONG=1 for the full matrix");
        int frames = int.TryParse(Environment.GetEnvironmentVariable("MS110D_LAYER3_FRAMES"), out int f) ? f : channel == "awgn" ? 60 : 160;
        Layer3SweepResult r = Layer3Runner.RunCell(wn, il, k, channel, frames, output);
        Layer3Runner.AssertCellNotFailed(r);
    }
}

/// <summary>
/// Combines the cells written by the long shards into the per-channel aggregate verdicts: reads every
/// *.outcomes.csv under MS110D_LAYER3_COMBINE_DIR, writes summary.md and aggregate.csv to MS110D_LAYER3_COMBINE_OUT,
/// and fails if any channel's aggregate or any cell is provably worse than the reference.
/// </summary>
[Trait("Category", "Layer3")]
public class Layer3CombineTests(ITestOutputHelper output)
{
    private sealed record Row(string Id, string Channel, double Target, string Pairing, double Snr, bool[] Lost);

    [Fact]
    public void CombineShards()
    {
        string? dir = Environment.GetEnvironmentVariable("MS110D_LAYER3_COMBINE_DIR");
        Assert.SkipWhen(string.IsNullOrEmpty(dir), "set MS110D_LAYER3_COMBINE_DIR to combine shard results");
        string outDir = Environment.GetEnvironmentVariable("MS110D_LAYER3_COMBINE_OUT") ?? dir!;
        var inv = System.Globalization.CultureInfo.InvariantCulture;
        var rows = new List<Row>();
        foreach (string file in Directory.EnumerateFiles(dir!, "*.outcomes.csv", SearchOption.AllDirectories))
        {
            foreach (string line in File.ReadLines(file).Skip(1).Where(l => l.Length > 0))
            {
                string[] c = line.Split(',');
                rows.Add(new Row($"wn{c[0]}-il{c[1]}-k{c[2]}-{c[3]}", c[3], double.Parse(c[4], inv), c[5], double.Parse(c[6], inv), [.. c[8].Select(ch => ch == '1')]));
            }
        }

        var summary = new System.Text.StringBuilder();
        var csv = new System.Text.StringBuilder("channel,pairing,cells,cells_used,mean_diff_db,ci_low_db,ci_high_db,verdict\n");
        var bad = new List<string>();
        summary.AppendLine("## Layer 3 results");
        summary.AppendLine();
        summary.AppendLine("Per channel model: the mean paired threshold difference (pairing minus C# to C#, positive = worse) over all cells, bootstrap 95% interval over cells and frames. FAIL only when the interval is wholly above +0.5 dB.");
        summary.AppendLine();
        summary.AppendLine("| channel | pairing | cells | used | mean diff dB | 95% interval dB | verdict |");
        summary.AppendLine("| --- | --- | ---: | ---: | ---: | --- | --- |");
        var notable = new List<string>();
        foreach (var chan in rows.GroupBy(r => r.Channel).OrderBy(g => g.Key))
        {
            foreach (var pairing in chan.Select(r => r.Pairing).Distinct().Where(p => p != "csharp->csharp").OrderBy(p => p))
            {
                var pairs = new List<Layer3Analysis.CellPair>();
                foreach (var cell in chan.GroupBy(r => r.Id))
                {
                    var rf = cell.Where(r => r.Pairing == "csharp->csharp").OrderBy(r => r.Snr).ToList();
                    var ot = cell.Where(r => r.Pairing == pairing).OrderBy(r => r.Snr).ToList();
                    if (rf.Count == 0 || ot.Count == 0)
                    {
                        continue;
                    }

                    Layer3Point ToPt(Row r) => new(r.Pairing.Split("->")[0], r.Pairing.Split("->")[1], r.Snr, r.Lost, 0, 0);
                    var refCurve = rf.Select(ToPt).ToList();
                    var otCurve = ot.Select(ToPt).ToList();
                    double target = rf[0].Target;
                    pairs.Add(new(cell.Key, refCurve, otCurve, target));
                    var pd = Layer3Analysis.PairedDifference(refCurve, otCurve, target);
                    var v = Layer3Analysis.Judge(pd.Diff, pd.Low, pd.High, Layer3Report.ToleranceDb);
                    if (v != Layer3Analysis.Verdict.Pass)
                    {
                        string d = pd.Diff is { } x ? $"{x:F2} dB ({pd.Low:F2} to {pd.High:F2})" : "";
                        notable.Add($"- {cell.Key} {pairing}: {v} {d}".TrimEnd());
                    }

                    if (v == Layer3Analysis.Verdict.Fail)
                    {
                        bad.Add($"{cell.Key} {pairing}");
                    }
                }

                var agg = Layer3Analysis.Aggregate(pairs, Layer3Report.ToleranceDb);
                string ci = agg.Low is { } l ? $"{l:F2} to {agg.High:F2}" : "n/a";
                summary.AppendLine($"| {chan.Key} | {pairing} | {agg.Cells} | {agg.UsedCells} | {agg.Mean:F2} | {ci} | {agg.Verdict.ToString().ToUpperInvariant()} |");
                csv.AppendLine($"{chan.Key},{pairing},{agg.Cells},{agg.UsedCells},{agg.Mean:F3},{agg.Low:F3},{agg.High:F3},{agg.Verdict}");
                if (agg.Verdict == Layer3Analysis.Verdict.Fail)
                {
                    bad.Add($"channel {chan.Key} {pairing}");
                }
            }
        }

        summary.AppendLine();
        summary.AppendLine($"Cells that were not a conclusive pass (first 100 of {notable.Count}; every cell is in the artifact):");
        summary.AppendLine();
        foreach (string n in notable.Take(100))
        {
            summary.AppendLine(n);
        }

        Directory.CreateDirectory(outDir);
        File.WriteAllText(Path.Combine(outDir, "summary.md"), summary.ToString());
        File.WriteAllText(Path.Combine(outDir, "aggregate.csv"), csv.ToString());
        output.WriteLine(summary.ToString());
        Assert.Empty(bad);
    }
}
