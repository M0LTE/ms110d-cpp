// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later

using System.Globalization;
using System.Text;

namespace Ms110dInterop.Tests.Layer3;

/// <summary>One non-reference pairing compared with the reference pairing in a cell.</summary>
public sealed record Layer3Comparison(string Reference, string Pairing, double? Diff, double? Low, double? High,
    int Discordant, int Total, Layer3Analysis.Verdict Verdict, string Note = "");

/// <summary>Writes the curves as CSV and a markdown summary, files per cell so shards never collide.</summary>
public static class Layer3Report
{
    /// <summary>The frame loss at which curves are compared: 10 % on AWGN, whose curve falls steeply. Fading
    /// curves are shallow (a few % per dB) and, on a short block, have a floor of a few % from deep fades, so
    /// 10 % is ill-conditioned there and 20 % is used instead.</summary>
    public static double TargetFor(Layer3Channel channel) => channel.Fading ? 0.20 : 0.10;

    public const double ToleranceDb = 0.5;

    public const string CurvesHeader = "waveform,interleaver,code_k,channel,tx,rx,snr_db,frames,frame_errors,frame_loss,loss_ci_low,loss_ci_high,bits,bit_errors,ber";

    public const string ThresholdsHeader = "waveform,interleaver,code_k,channel,tx,rx,target_loss,snr_db,approx_ci_low_db,approx_ci_high_db,frames_per_point,note,diff_vs_reference_db,diff_ci_low_db,diff_ci_high_db,differing_decodes,decodes,verdict";

    public const string OutcomesHeader = "waveform,interleaver,code_k,channel,target_loss,pairing,snr_db,frames,lost";

    /// <summary>Directory for results: MS110D_LAYER3_OUT, else layer3-results under the current directory.</summary>
    public static string OutDir()
    {
        string d = Environment.GetEnvironmentVariable("MS110D_LAYER3_OUT") ?? Path.Combine(Directory.GetCurrentDirectory(), "layer3-results");
        string run = Environment.GetEnvironmentVariable("MS110D_LAYER3_RUN") ?? "default";
        string full = Path.Combine(d, run, "cells");
        Directory.CreateDirectory(full);
        return full;
    }

    private static string F(double v, string fmt = "F4") => v.ToString(fmt, CultureInfo.InvariantCulture);

    private static string F(double? v, string fmt = "F2") => v is { } x ? x.ToString(fmt, CultureInfo.InvariantCulture) : "";

    public static string Curves(Layer3SweepResult r)
    {
        var sb = new StringBuilder();
        Layer3Spec s = r.Spec;
        foreach (Layer3Point p in r.Points.OrderBy(p => p.Pairing).ThenBy(p => p.SnrDb))
        {
            var (lo, hi) = Layer3Analysis.Wilson(p.FrameErrors, p.Frames);
            sb.AppendLine(string.Join(',', s.Wn, s.Il, s.K, s.Channel.Name, p.Tx, p.Rx, F(p.SnrDb, "F1"), p.Frames, p.FrameErrors,
                F(p.FrameLoss), F(lo), F(hi), p.Bits, p.BitErrors, ((double)p.BitErrors / p.Bits).ToString("E3", CultureInfo.InvariantCulture)));
        }

        return sb.ToString();
    }

    /// <summary>Per frame outcome of every point, enough to recompute any threshold or aggregate later.</summary>
    public static string Outcomes(Layer3SweepResult r)
    {
        var sb = new StringBuilder();
        Layer3Spec s = r.Spec;
        foreach (Layer3Point p in r.Points.OrderBy(p => p.Pairing).ThenBy(p => p.SnrDb))
        {
            sb.AppendLine(string.Join(',', s.Wn, s.Il, s.K, s.Channel.Name, F(TargetFor(s.Channel), "F2"), p.Pairing, F(p.SnrDb, "F1"), p.Frames,
                new string([.. p.Lost.Select(l => l ? '1' : '0')])));
        }

        return sb.ToString();
    }

    public static IReadOnlyList<Layer3Threshold> Thresholds(Layer3SweepResult r) =>
        [.. r.Pairings.Select(p => Layer3Analysis.Threshold(p, r.Curve(p), TargetFor(r.Spec.Channel)))];

    /// <summary>The reference is the first pairing; every other one is judged against it.</summary>
    public static IReadOnlyList<Layer3Comparison> Compare(Layer3SweepResult r)
    {
        string[] pairings = [.. r.Pairings];
        var result = new List<Layer3Comparison>();
        if (pairings.Length < 2)
        {
            return result;
        }

        double target = TargetFor(r.Spec.Channel);
        var reference = r.Curve(pairings[0]);
        foreach (string p in pairings.Skip(1))
        {
            var curve = r.Curve(p);
            var d = Layer3Analysis.Discordance(reference, curve);
            if (!r.Reached)
            {
                result.Add(new Layer3Comparison(pairings[0], p, null, null, null, d.Discordant, d.Total, Layer3Analysis.Verdict.NotReached));
                continue;
            }

            var j = Layer3Analysis.JudgeCell(reference, curve, target, ToleranceDb);
            result.Add(new Layer3Comparison(pairings[0], p, j.Diff, j.Low, j.High, d.Discordant, d.Total, j.Verdict, j.Note));
        }

        return result;
    }

    public static string ThresholdCsv(Layer3SweepResult r)
    {
        var sb = new StringBuilder();
        Layer3Spec s = r.Spec;
        var cmp = Compare(r).ToDictionary(c => c.Pairing);
        foreach (Layer3Threshold t in Thresholds(r))
        {
            string[] pr = t.Pairing.Split("->");
            cmp.TryGetValue(t.Pairing, out Layer3Comparison? c);
            string verdict = !r.Reached ? "NotReached" : c is null ? "reference" : c.Verdict.ToString();
            sb.AppendLine(string.Join(',', s.Wn, s.Il, s.K, s.Channel.Name, pr[0], pr[1], F(t.Target, "F2"), F(t.SnrDb), F(t.LowDb), F(t.HighDb), s.Frames,
                "\"" + (t.Note.Length > 0 ? t.Note : r.Note) + "\"", F(c?.Diff), F(c?.Low), F(c?.High), c?.Discordant.ToString() ?? "", c?.Total.ToString() ?? "", verdict));
        }

        return sb.ToString();
    }

    public static string Markdown(Layer3SweepResult r)
    {
        Layer3Spec s = r.Spec;
        var sb = new StringBuilder();
        sb.AppendLine($"### {s.Label}");
        sb.AppendLine();
        sb.AppendLine($"{s.Frames} frames per point (each a single burst of {s.PayloadBits} payload bits, one interleaver block), seeds fixed, identical fading and noise for every pairing. Ran in {r.Seconds:F0} s." + (r.Shifts > 0 ? $" Ladder moved {r.Shifts} time(s)." : ""));
        sb.AppendLine();
        string[] pairings = [.. r.Pairings];
        sb.AppendLine("| SNR dB | " + string.Join(" | ", pairings.Select(p => $"{p} loss")) + " |");
        sb.AppendLine("| ---: |" + string.Join("", pairings.Select(_ => " ---: |")));
        foreach (double snr in s.SnrLadder)
        {
            sb.AppendLine($"| {F(snr, "F1")} | " + string.Join(" | ", pairings.Select(p =>
            {
                Layer3Point pt = r.Curve(p).First(x => x.SnrDb == snr);
                return $"{pt.FrameErrors}/{pt.Frames} ({F(pt.FrameLoss * 100, "F1")}%)";
            })) + " |");
        }

        sb.AppendLine();
        double target = TargetFor(s.Channel);
        if (!r.Reached)
        {
            sb.AppendLine($"Target {target * 100:F0}% frame loss: NOT REACHED. {r.Note}");
            sb.AppendLine();
        }
        else
        {
            sb.AppendLine($"SNR at {target * 100:F0}% frame loss (the interval is approximate, from the fit's standard error):");
            sb.AppendLine();
            foreach (Layer3Threshold t in Thresholds(r))
            {
                string ci = t.HalfWidthDb is { } h ? $" (approx. +/- {F(h)} dB)" : t.SnrDb is null ? "" : " (interval open-ended)";
                sb.AppendLine($"- {t.Pairing}: {(t.SnrDb is null ? "not bracketed, " + t.Note : F(t.SnrDb) + " dB" + ci)}");
            }

            sb.AppendLine();
        }

        foreach (Layer3Comparison c in Compare(r))
        {
            sb.AppendLine($"Paired by frame, {c.Pairing} against {c.Reference}: {c.Discordant} of {c.Total} frame decodes differ. "
                + (c.Diff is { } df
                    ? $"Threshold difference {F(df)} dB (positive = worse), paired bootstrap 95% interval {F(c.Low)} to {F(c.High)} dB. Verdict: {c.Verdict.ToString().ToUpperInvariant()}."
                    : $"Verdict: {c.Verdict.ToString().ToUpperInvariant()}. {c.Note}".TrimEnd()));
            sb.AppendLine();
        }

        foreach (string sk in r.Skipped)
        {
            sb.AppendLine($"- skipped {sk}");
        }

        return sb.ToString();
    }

    public static void Write(Layer3SweepResult r)
    {
        string dir = OutDir();
        string id = r.Spec.Id;
        File.WriteAllText(Path.Combine(dir, id + ".curves.csv"), CurvesHeader + "\n" + Curves(r));
        File.WriteAllText(Path.Combine(dir, id + ".thresholds.csv"), ThresholdsHeader + "\n" + ThresholdCsv(r));
        File.WriteAllText(Path.Combine(dir, id + ".outcomes.csv"), OutcomesHeader + "\n" + Outcomes(r));
        File.WriteAllText(Path.Combine(dir, id + ".md"), Markdown(r));
    }
}
