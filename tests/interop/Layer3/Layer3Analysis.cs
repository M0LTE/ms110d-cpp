// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later

namespace Ms110dInterop.Tests.Layer3;

/// <summary>Where a frame-loss curve crosses a target, with an approximate 95 % interval from the fit's standard error.
/// It treats the points as independent binomials, so it is a guide, not a test; the paired bootstrap is the test.</summary>
public sealed record Layer3Threshold(string Pairing, double Target, double? SnrDb, double? LowDb, double? HighDb, string Note)
{
    /// <summary>Half the width of the 95 % interval, or null when either end is open.</summary>
    public double? HalfWidthDb => LowDb is { } l && HighDb is { } h ? (h - l) / 2 : null;
}

/// <summary>The maths of the pass rule (docs/dev/layer3.md). Pure functions, unit tested without any modem.</summary>
public static class Layer3Analysis
{
    /// <summary>Wilson score interval for k losses in n frames (z = 1.96).</summary>
    public static (double Low, double High) Wilson(int k, int n)
    {
        const double z = 1.96;
        double p = (double)k / n;
        double d = 1 + (z * z / n);
        double c = (p + (z * z / (2 * n))) / d;
        double h = z * Math.Sqrt((p * (1 - p) / n) + (z * z / (4.0 * n * n))) / d;
        return (Math.Max(0, c - h), Math.Min(1, c + h));
    }

    /// <summary>
    /// Fits logit(loss) = a + b x to the points by iteratively reweighted least squares, each point carrying half a
    /// pseudo-frame lost and half kept so that a run of 0 % or 100 % points cannot send the fit to infinity. Returns
    /// the SNR at which the fitted curve reaches <paramref name="target"/> and its standard error (delta method,
    /// treating the points as independent binomials), or null with a note when that SNR lies outside the ladder, by more than <paramref name="extrapolateDb"/> (or
    /// the curve does not fall with SNR). Using every frame of every point makes this far less noisy than reading the
    /// crossing off the two points either side of the target.
    /// </summary>
    public static double? FitThreshold(double[] snr, double[] lost, double[] frames, double target, out double? se, out string note, double extrapolateDb = 0)
    {
        se = null;
        note = "";
        int m = snr.Length;
        double mean = snr.Average();
        double[] x = [.. snr.Select(v => v - mean)];
        double[] y = [.. lost.Select(v => v + 0.5)];
        double[] n = [.. frames.Select(v => v + 1)];
        double a = Math.Log(y.Sum() / (n.Sum() - y.Sum()));
        double b = -0.5;
        double i00 = 0, i01 = 0, i11 = 0;
        for (int it = 0; it < 50; it++)
        {
            double s0 = 0, s1 = 0;
            i00 = i01 = i11 = 0;
            for (int i = 0; i < m; i++)
            {
                double mu = 1 / (1 + Math.Exp(-Math.Clamp(a + (b * x[i]), -30, 30)));
                double w = n[i] * mu * (1 - mu);
                double r = y[i] - (n[i] * mu);
                s0 += r;
                s1 += r * x[i];
                i00 += w;
                i01 += w * x[i];
                i11 += w * x[i] * x[i];
            }

            double det = (i00 * i11) - (i01 * i01);
            if (det < 1e-12)
            {
                break;
            }

            double da = ((i11 * s0) - (i01 * s1)) / det;
            double db = ((i00 * s1) - (i01 * s0)) / det;
            double step = Math.Max(Math.Abs(da), Math.Abs(db));
            double damp = step > 3 ? 3 / step : 1;
            a += da * damp;
            b += db * damp;
            if (step < 1e-9)
            {
                break;
            }
        }

        if (b >= -1e-6)
        {
            note = "still above target at the highest SNR (the curve does not fall with SNR)";
            return null;
        }

        double logit = Math.Log(target / (1 - target));
        double t = ((logit - a) / b) + mean;
        double dets = (i00 * i11) - (i01 * i01);
        if (dets > 1e-12)
        {
            // Covariance = inverse information; gradient of t=(L-a)/b is (-1/b, -(t-mean)/b).
            double v00 = i11 / dets, v01 = -i01 / dets, v11 = i00 / dets;
            double g0 = -1 / b, g1 = -(t - mean) / b;
            se = Math.Sqrt(Math.Max(0, (g0 * g0 * v00) + (2 * g0 * g1 * v01) + (g1 * g1 * v11)));
        }

        if (t > snr.Max() + extrapolateDb)
        {
            note = "still above target at the highest SNR";
            return null;
        }

        if (t < snr.Min() - extrapolateDb)
        {
            note = "already below target at the lowest SNR";
            return null;
        }

        return t;
    }

    /// <summary>The threshold of one pairing's curve (logistic fit), with an approximate 95 % interval from the fit's standard error.</summary>
    public static Layer3Threshold Threshold(string pairing, IReadOnlyList<Layer3Point> curve, double target)
    {
        double[] snr = [.. curve.Select(p => p.SnrDb)];
        double[] lost = [.. curve.Select(p => (double)p.FrameErrors)];
        double[] n = [.. curve.Select(p => (double)p.Frames)];
        double? t = FitThreshold(snr, lost, n, target, out double? se, out string note);
        return t is { } v && se is { } e
            ? new Layer3Threshold(pairing, target, v, v - (1.96 * e), v + (1.96 * e), note)
            : new Layer3Threshold(pairing, target, t, null, null, note);
    }

    /// <summary>Frames where exactly one of the two pairings was lost, over all SNRs (the paired comparison).</summary>
    public static (int Discordant, int OnlyA, int OnlyB, int Total) Discordance(IReadOnlyList<Layer3Point> a, IReadOnlyList<Layer3Point> b)
    {
        int onlyA = 0, onlyB = 0, total = 0;
        for (int s = 0; s < a.Count; s++)
        {
            for (int f = 0; f < a[s].Frames; f++)
            {
                total++;
                if (a[s].Lost[f] && !b[s].Lost[f])
                {
                    onlyA++;
                }
                else if (!a[s].Lost[f] && b[s].Lost[f])
                {
                    onlyB++;
                }
            }
        }

        return (onlyA + onlyB, onlyA, onlyB, total);
    }

    /// <summary>When comparing two curves on a ladder centred on the reference, the other curve's crossing may lie a little
    /// beyond the ladder (that is what a worse receiver looks like); the fit is trusted this far past either end.</summary>
    public const double ComparisonExtrapolationDb = 2.0;

    /// <summary>Threshold of a curve over a resample of its frames (idx), or null when the target is not bracketed.</summary>
    private static double? ResampledThreshold(IReadOnlyList<Layer3Point> c, int[] idx, double target)
    {
        int n = idx.Length;
        double[] snr = [.. c.Select(p => p.SnrDb)];
        double[] loss = new double[c.Count];
        for (int s = 0; s < c.Count; s++)
        {
            bool[] lost = c[s].Lost;
            int k = 0;
            foreach (int i in idx)
            {
                if (lost[i])
                {
                    k++;
                }
            }

            loss[s] = (double)k / n;
        }

        return FitThreshold(snr, [.. loss.Select(l => l * n)], [.. c.Select(_ => (double)n)], target, out _, out _, ComparisonExtrapolationDb);
    }

    /// <summary>
    /// The difference in threshold SNR (b minus a, so positive means b needs more SNR, is worse) with a paired
    /// bootstrap 95 % interval: frames are resampled together for both pairings (the same frame index in each),
    /// so the realisation luck cancels and only the modems' own difference is left. The interval is null when
    /// fewer than half the resamples bracket the target in both curves.
    /// </summary>
    public static (double? Diff, double? Low, double? High) PairedDifference(
        IReadOnlyList<Layer3Point> a, IReadOnlyList<Layer3Point> b, double target, int resamples = 400, int seed = 12345)
    {
        int frames = a[0].Frames;
        int[] all = [.. Enumerable.Range(0, frames)];
        double? da = ResampledThreshold(a, all, target);
        double? db = ResampledThreshold(b, all, target);
        double? diff = da is { } x && db is { } y ? y - x : null;
        var rng = new Random(seed);
        var ds = new List<double>();
        for (int r = 0; r < resamples; r++)
        {
            int[] idx = [.. Enumerable.Range(0, frames).Select(_ => rng.Next(frames))];
            if (ResampledThreshold(a, idx, target) is { } ta && ResampledThreshold(b, idx, target) is { } tb)
            {
                ds.Add(tb - ta);
            }
        }

        if (ds.Count < resamples / 2)
        {
            return (diff, null, null);
        }

        ds.Sort();
        return (diff, ds[(int)(0.025 * ds.Count)], ds[Math.Min(ds.Count - 1, (int)(0.975 * ds.Count))]);
    }

    public enum Verdict
    {
        /// <summary>The interval lies wholly within the tolerance.</summary>
        Pass,

        /// <summary>The interval lies wholly above the tolerance: the second pairing is provably worse.</summary>
        Fail,

        /// <summary>The data cannot say: the interval straddles the tolerance.</summary>
        Inconclusive,

        /// <summary>The curve never reached the target frame loss (a floor); nothing to compare.</summary>
        NotReached,
    }

    /// <summary>
    /// The verdict on a difference (positive = worse) and its interval. A failure needs the whole interval above
    /// the tolerance; a pass needs it wholly inside +/- the tolerance; anything else is inconclusive.
    /// </summary>
    public static Verdict Judge(double? diff, double? low, double? high, double toleranceDb)
    {
        if (diff is null)
        {
            return Verdict.NotReached;
        }

        if (low is not { } l || high is not { } h)
        {
            return Verdict.Inconclusive;
        }

        if (l > toleranceDb)
        {
            return Verdict.Fail;
        }

        return l >= -toleranceDb && h <= toleranceDb ? Verdict.Pass : Verdict.Inconclusive;
    }

    /// <summary>One cell's two curves to be compared (reference and other), at the cell's target.</summary>
    public sealed record CellPair(string Id, IReadOnlyList<Layer3Point> Reference, IReadOnlyList<Layer3Point> Other, double Target);

    public sealed record AggregateResult(int Cells, int UsedCells, double? Mean, double? Low, double? High, Verdict Verdict);

    /// <summary>
    /// The verdict over many cells of one channel model: the mean of the per-cell paired differences with a
    /// bootstrap 95 % interval that resamples cells and, within each, frames (the same frames in both curves).
    /// Cells whose target is not reached in either curve are left out and counted in <c>Cells - UsedCells</c>.
    /// </summary>
    public static AggregateResult Aggregate(IReadOnlyList<CellPair> cells, double toleranceDb, int resamples = 1000, int seed = 777)
    {
        var used = new List<(CellPair Cell, int[] All)>();
        var diffs = new List<double>();
        foreach (CellPair c in cells)
        {
            int[] all = [.. Enumerable.Range(0, c.Reference[0].Frames)];
            if (ResampledThreshold(c.Reference, all, c.Target) is { } a && ResampledThreshold(c.Other, all, c.Target) is { } b)
            {
                used.Add((c, all));
                diffs.Add(b - a);
            }
        }

        if (used.Count == 0)
        {
            return new AggregateResult(cells.Count, 0, null, null, null, Verdict.NotReached);
        }

        double mean = diffs.Average();
        var rng = new Random(seed);
        var means = new List<double>();
        for (int r = 0; r < resamples; r++)
        {
            double sum = 0;
            int n = 0;
            for (int j = 0; j < used.Count; j++)
            {
                var (cell, all) = used[rng.Next(used.Count)];
                int frames = all.Length;
                int[] idx = new int[frames];
                for (int i = 0; i < frames; i++)
                {
                    idx[i] = rng.Next(frames);
                }

                if (ResampledThreshold(cell.Reference, idx, cell.Target) is { } ta && ResampledThreshold(cell.Other, idx, cell.Target) is { } tb)
                {
                    sum += tb - ta;
                    n++;
                }
            }

            if (n > 0)
            {
                means.Add(sum / n);
            }
        }

        if (means.Count < resamples / 2)
        {
            return new AggregateResult(cells.Count, used.Count, mean, null, null, Verdict.Inconclusive);
        }

        means.Sort();
        double lo = means[(int)(0.025 * means.Count)];
        double hi = means[Math.Min(means.Count - 1, (int)(0.975 * means.Count))];
        return new AggregateResult(cells.Count, used.Count, mean, lo, hi, Judge(mean, lo, hi, toleranceDb));
    }
}
