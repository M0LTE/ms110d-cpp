// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later

using System.Globalization;
using System.Text;

namespace Ms110dInterop.Tests.Layer3;

/// <summary>Writes the curves as CSV and a markdown summary, one pair of files per cell, so shards never collide.</summary>
public static class Layer3Report
{
    /// <summary>The frame loss at which curves are compared: 10 % on AWGN, whose curve falls steeply. Fading
    /// curves are shallow (a few % per dB) and, on a short block, have a floor of a few % from deep fades, so
    /// 10 % is ill-conditioned there and 20 % is used instead.</summary>
    public static double TargetFor(Layer3Channel channel) => channel.Fading ? 0.20 : 0.10;
    public const double ToleranceDb = 0.5;

    public const string CurvesHeader = "waveform,interleaver,code_k,channel,tx,rx,snr_db,frames,frame_errors,frame_loss,loss_ci_low,loss_ci_high,bits,bit_errors,ber";

    public const string ThresholdsHeader = "waveform,interleaver,code_k,channel,tx,rx,target_loss,snr_db,ci_low_db,ci_high_db,frames_per_point,note";

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

    public static IReadOnlyList<Layer3Threshold> Thresholds(Layer3SweepResult r) =>
        [.. r.Pairings.Select(p => Layer3Analysis.Threshold(p, r.Curve(p), TargetFor(r.Spec.Channel)))];

    public static string ThresholdCsv(Layer3SweepResult r)
    {
        var sb = new StringBuilder();
        Layer3Spec s = r.Spec;
        double target = TargetFor(s.Channel);
        {
            foreach (Layer3Threshold t in Thresholds(r))
            {
                string[] pr = t.Pairing.Split("->");
                sb.AppendLine(string.Join(',', s.Wn, s.Il, s.K, s.Channel.Name, pr[0], pr[1], F(target, "F2"), F(t.SnrDb), F(t.LowDb), F(t.HighDb), s.Frames, t.Note));
            }
        }

        return sb.ToString();
    }

    public static string Markdown(Layer3SweepResult r)
    {
        Layer3Spec s = r.Spec;
        var sb = new StringBuilder();
        sb.AppendLine($"### {s.Label}");
        sb.AppendLine();
        sb.AppendLine($"{s.Frames} frames per point (each a single burst of {s.PayloadBits} payload bits, one interleaver block), seeds fixed, identical fading and noise for every pairing. Ran in {r.Seconds:F0} s.");
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
        {
            double target = TargetFor(s.Channel);
            var ts = Thresholds(r);
            sb.AppendLine($"SNR at {target * 100:F0}% frame loss:");
            sb.AppendLine();
            foreach (Layer3Threshold t in ts)
            {
                string ci = t.HalfWidthDb is { } h ? $" (95% interval +/- {F(h)} dB)" : t.SnrDb is null ? "" : " (95% interval open-ended)";
                sb.AppendLine($"- {t.Pairing}: {(t.SnrDb is null ? "not bracketed, " + t.Note : F(t.SnrDb) + " dB" + ci)}");
            }

            var ag = Layer3Analysis.Agree(ts, ToleranceDb);
            sb.AppendLine($"- agreement: {(ag.Pass ? "PASS" : "FAIL")}, {ag.Message}");
            sb.AppendLine();
        }

        if (pairings.Length > 1)
        {
            var reference = r.Curve(pairings[0]);
            foreach (string p in pairings.Skip(1))
            {
                var d = Layer3Analysis.Discordance(reference, r.Curve(p));
                sb.AppendLine($"Paired by frame, {pairings[0]} against {p}: {d.Discordant} of {d.Total} frame decodes differ ({d.OnlyA} lost only by {pairings[0]}, {d.OnlyB} only by {p}).");
                var pd = Layer3Analysis.PairedDifference(reference, r.Curve(p), TargetFor(s.Channel));
                sb.AppendLine(pd.Diff is { } df
                    ? $"Threshold difference at {TargetFor(s.Channel) * 100:F0}% ({p} minus {pairings[0]}): {F(df)} dB, paired bootstrap 95% interval {F(pd.Low)} to {F(pd.High)} dB."
                    : $"Threshold difference at {TargetFor(s.Channel) * 100:F0}% ({p} minus {pairings[0]}): not computable (curve does not bracket the target).");
                sb.AppendLine();
            }
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
        File.WriteAllText(Path.Combine(dir, id + ".curves.csv"), Curves(r));
        File.WriteAllText(Path.Combine(dir, id + ".thresholds.csv"), ThresholdCsv(r));
        File.WriteAllText(Path.Combine(dir, id + ".md"), Markdown(r));
    }
}
