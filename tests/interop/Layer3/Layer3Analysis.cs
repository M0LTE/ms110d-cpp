// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later

namespace Ms110dInterop.Tests.Layer3;

/// <summary>Where a frame-loss curve crosses a target, with a 95 % interval from the per-point binomial error.</summary>
public sealed record Layer3Threshold(string Pairing, double Target, double? SnrDb, double? LowDb, double? HighDb, string Note)
{
    /// <summary>Half the width of the 95 % interval, or null when either end is open.</summary>
    public double? HalfWidthDb => LowDb is { } l && HighDb is { } h ? (h - l) / 2 : null;
}

/// <summary>The maths of the pass rule. Pure functions, unit tested without any modem.</summary>
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

    /// <summary>Pool-adjacent-violators: the closest non-increasing curve (frame loss must fall as SNR rises).</summary>
    public static double[] MonotoneDecreasing(double[] loss, double[] weight)
    {
        var val = new List<double>();
        var w = new List<double>();
        var len = new List<int>();
        for (int i = 0; i < loss.Length; i++)
        {
            val.Add(loss[i]);
            w.Add(weight[i]);
            len.Add(1);
            while (val.Count > 1 && val[^2] < val[^1])
            {
                int a = val.Count - 2;
                double tw = w[a] + w[a + 1];
                val[a] = ((val[a] * w[a]) + (val[a + 1] * w[a + 1])) / tw;
                w[a] = tw;
                len[a] += len[a + 1];
                val.RemoveAt(a + 1);
                w.RemoveAt(a + 1);
                len.RemoveAt(a + 1);
            }
        }

        var outv = new double[loss.Length];
        int k = 0;
        for (int b = 0; b < val.Count; b++)
        {
            for (int j = 0; j < len[b]; j++)
            {
                outv[k++] = val[b];
            }
        }

        return outv;
    }

    /// <summary>
    /// The SNR at which a monotone curve first falls to <paramref name="target"/>, linear in log(loss) between
    /// the bracketing points. Zero losses are floored at half a frame. Null when the ladder does not bracket it.
    /// </summary>
    public static double? Crossing(double[] snr, double[] loss, int frames, double target, out string note)
    {
        double floor = 0.5 / frames;
        double lt = Math.Log(target);
        note = "";
        double L(int i) => Math.Log(Math.Max(loss[i], floor));
        if (L(0) < lt)
        {
            note = "already below target at the lowest SNR";
            return null;
        }

        for (int i = 1; i < snr.Length; i++)
        {
            if (L(i) <= lt)
            {
                double a = L(i - 1);
                double b = L(i);
                double t = a == b ? 1 : (a - lt) / (a - b);
                return snr[i - 1] + (t * (snr[i] - snr[i - 1]));
            }
        }

        note = "still above target at the highest SNR";
        return null;
    }

    /// <summary>The threshold of one pairing's curve, and the interval from the Wilson bounds of every point.</summary>
    public static Layer3Threshold Threshold(string pairing, IReadOnlyList<Layer3Point> curve, double target)
    {
        double[] snr = [.. curve.Select(p => p.SnrDb)];
        double[] w = [.. curve.Select(p => (double)p.Frames)];
        int n = curve[0].Frames;
        double[] Mono(Func<Layer3Point, double> f) => MonotoneDecreasing([.. curve.Select(f)], w);
        double? mid = Crossing(snr, Mono(p => p.FrameLoss), n, target, out string note);
        double? lo = Crossing(snr, Mono(p => Wilson(p.FrameErrors, p.Frames).Low), n, target, out _);
        double? hi = Crossing(snr, Mono(p => Wilson(p.FrameErrors, p.Frames).High), n, target, out _);
        return new Layer3Threshold(pairing, target, mid, lo, hi, note);
    }

    public sealed record Agreement(bool Pass, double? SpreadDb, string Message);

    /// <summary>
    /// The pass rule: every pairing's threshold within <paramref name="toleranceDb"/> of the others (the spread,
    /// largest minus smallest). A pairing whose curve does not bracket the target cannot be compared and fails:
    /// the ladder must be fixed, not the rule.
    /// </summary>
    public static Agreement Agree(IReadOnlyList<Layer3Threshold> thresholds, double toleranceDb)
    {
        var open = thresholds.Where(t => t.SnrDb is null).ToList();
        if (open.Count > 0)
        {
            return new Agreement(false, null, "target not bracketed by the ladder for " + string.Join(", ", open.Select(t => $"{t.Pairing} ({t.Note})")));
        }

        double lo = thresholds.Min(t => t.SnrDb!.Value);
        double hi = thresholds.Max(t => t.SnrDb!.Value);
        double spread = hi - lo;
        return new Agreement(spread <= toleranceDb, spread, $"spread {spread:F2} dB against a limit of {toleranceDb:F2} dB");
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

    /// <summary>
    /// The difference in threshold SNR (b minus a) with a paired bootstrap 95 % interval: frames are resampled
    /// together for both pairings (the same frame index in each), so the realisation luck cancels and only the
    /// modems' own difference is left. Null when either curve does not bracket the target in a resample.
    /// </summary>
    public static (double? Diff, double? Low, double? High) PairedDifference(
        IReadOnlyList<Layer3Point> a, IReadOnlyList<Layer3Point> b, double target, int resamples = 400, int seed = 12345)
    {
        double? Thr(IReadOnlyList<Layer3Point> c, int[] idx)
        {
            int n = idx.Length;
            double[] snr = [.. c.Select(p => p.SnrDb)];
            double[] loss = [.. c.Select(p => (double)idx.Count(i => p.Lost[i]) / n)];
            double[] w = [.. c.Select(_ => (double)n)];
            return Crossing(snr, MonotoneDecreasing(loss, w), n, target, out _);
        }

        int frames = a[0].Frames;
        int[] all = [.. Enumerable.Range(0, frames)];
        double? da = Thr(a, all);
        double? db = Thr(b, all);
        double? diff = da is { } x && db is { } y ? y - x : null;
        var rng = new Random(seed);
        var ds = new List<double>();
        for (int r = 0; r < resamples; r++)
        {
            int[] idx = [.. Enumerable.Range(0, frames).Select(_ => rng.Next(frames))];
            if (Thr(a, idx) is { } ta && Thr(b, idx) is { } tb)
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
}
