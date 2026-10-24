// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later

// Dumps what the C# demodulator's preamble search finds, for tests/data/csharp-detect-0.2.0.txt.
// Run from the repo root:
//   dotnet run -c Release --project tools/csharp-vectors -- detect build/_reference/ref > tests/data/csharp-detect-0.2.0.txt
//
// The search is not a public type: it is the first stage of Ms110dDemodulator, which goes on to
// demodulate the burst. The tool feeds one sample at a time and stops the moment the receiver
// has locked on a waveform (Lock.WaveformNumber >= 0), so nothing past the preamble is ever
// processed, then reads the carrier and timing state through reflection on the real private
// fields. FrameDiagnostics (public) is used to count accepted peaks and rejected candidates.
//
// Three kinds of line, all "kind name key=value ... -> result":
//   ref   the audio of a reference case exactly as written by the C# transmitter
//   scn   a reference case's baseband re-modulated with an impairment (carrier offset,
//         fractional delay, lead-in, seeded noise at a stated SNR), see Impair below
//   noise a seeded noise-only stream, for false alarms
// The impairment and the noise generator are specified in docs/dev/preamble-detector.md and
// implemented identically in tests/support/impair.cpp; the line carries every parameter, so the
// C++ test replays it without knowing the scenario list.

using System.Globalization;
using System.Reflection;
using System.Text;
using M0LTE.Ms110d;

static class DetectDump
{
    static readonly CultureInfo Inv = CultureInfo.InvariantCulture;

    public static string Run(string refDir)
    {
        var sb = new StringBuilder();
        sb.Append("# Dumped from M0LTE.Ms110d 0.2.0 by tools/csharp-vectors (detect mode): the preamble search of Ms110dDemodulator.\n");
        sb.Append("# Doubles are round-trip text. 'at' is input samples used when the lock completed.\n");

        var cases = new List<string>();
        foreach (string line in File.ReadAllLines(Path.Combine(refDir, "index.txt")))
        {
            if (line.StartsWith("case=", StringComparison.Ordinal)) cases.Add(line[5..]);
        }

        // 1. Every reference case, unimpaired.
        foreach (string name in cases)
        {
            float[] audio = ReadF32(Path.Combine(refDir, name, Manifest(refDir, name)["file.audio_f32le"]));
            sb.Append("ref ").Append(name).Append(" -> ").Append(Detect(audio, 0.32, 0.35)).Append('\n');
        }

        // 2. Impaired scenarios.
        foreach (var s in Scenarios(refDir, cases))
        {
            float[] audio = Impair.Build(refDir, s);
            sb.Append("scn ").Append(s.Describe()).Append(" -> ").Append(Detect(audio, s.Threshold, 0.35)).Append('\n');
        }

        // 3. Noise only.
        foreach (var n in NoiseStreams())
        {
            float[] audio = Impair.NoiseOnly(n.Seed, n.Samples, n.Sigma);
            sb.Append("noise seed=").Append(n.Seed.ToString(Inv)).Append(" samples=").Append(n.Samples.ToString(Inv))
              .Append(" sigma=").Append(R(n.Sigma)).Append(" thr=").Append(R(n.Threshold))
              .Append(" -> ").Append(Detect(audio, n.Threshold, 0.35)).Append('\n');
        }
        return sb.ToString();
    }

    // ---- scenario lists -------------------------------------------------------------------

    static IEnumerable<Scenario> Scenarios(string refDir, List<string> cases)
    {
        // One case per waveform number, the first in the index.
        var picks = new List<string>();
        foreach (int wn in new[] { 0, 1, 2, 3, 4, 5, 6, 7, 8, 13 })
        {
            string? c = cases.FirstOrDefault(x => x.StartsWith($"wn{wn}_", StringComparison.Ordinal));
            if (c is not null) picks.Add(c);
        }

        double[] snrs = [-9, -6, -3, 0, 3, 6, 12];
        double[] dfs = [-50, 0, 50];
        double[] delays = [0, 0.37, 0.5, 1.0, 1.61];
        int[] leads = [0, 1, 777, 1601];
        ulong seed = 1;
        for (int ci = 0; ci < picks.Count; ci++)
        {
            for (int si = 0; si < snrs.Length; si++)
            {
                for (int rep = 0; rep < 3; rep++)
                {
                    int k = ci + (si * 3) + rep;
                    yield return new Scenario(picks[ci], snrs[si], dfs[k % dfs.Length], delays[(k / 2) % delays.Length],
                        leads[(k + rep) % leads.Length], seed++, 0.32);
                }
            }
        }

        // Carrier offset sweep through the search grid and past its edge, at a moderate SNR.
        foreach (string c in picks.Take(3))
        {
            for (int df = -90; df <= 90; df += 10)
            {
                yield return new Scenario(c, 6, df, 0.25, 100, seed++, 0.32);
            }
        }

        // Timing sweep over two T/2 samples (four input samples) in eighths of an input sample.
        foreach (string c in picks.Skip(1).Take(3))
        {
            for (int i = 0; i <= 32; i++)
            {
                yield return new Scenario(c, 6, 0, i * 0.125, 64, seed++, 0.32);
            }
        }

        // A lower threshold, to make the search accept more candidates and so exercise the rejection gates.
        foreach (string c in picks)
        {
            for (int si = 0; si < 4; si++)
            {
                yield return new Scenario(c, snrs[si], dfs[si % 3], 0.5, 300, seed++, 0.22);
            }
        }
    }

    static IEnumerable<(ulong Seed, int Samples, double Sigma, double Threshold)> NoiseStreams()
    {
        for (int i = 0; i < 10; i++)
        {
            yield return ((ulong)(1000 + i), 9600 * 12, 0.1, 0.32);
        }

        for (int i = 0; i < 10; i++)
        {
            yield return ((ulong)(2000 + i), 9600 * 12, 0.1, 0.22);
        }

        for (int i = 0; i < 10; i++)
        {
            yield return ((ulong)(3000 + i), 9600 * 12, 0.1, 0.18);
        }
    }

    // ---- the C# detector ------------------------------------------------------------------

    static readonly FieldInfo Chip0 = Field("_chip0");
    static readonly FieldInfo Omega = Field("_omega");
    static readonly FieldInfo ThetaBase = Field("_thetaBase");
    static readonly FieldInfo DataStartChip = Field("_dataStartChip");
    static readonly FieldInfo Written = Field("_written");

    static FieldInfo Field(string name) =>
        typeof(Ms110dDemodulator).GetField(name, BindingFlags.NonPublic | BindingFlags.Instance)
        ?? throw new InvalidOperationException($"{name} not found");

    public static string Detect(float[] audio, double threshold, double rollOff)
    {
        var demod = new Ms110dDemodulator(new Ms110dDemodOptions { SyncThreshold = threshold, RollOff = rollOff });
        int peaks = 0, rejects = 0;
        demod.FrameDiagnostics += line =>
        {
            if (line.StartsWith("accept@", StringComparison.Ordinal)) peaks++;
            else if (line.StartsWith("reject@", StringComparison.Ordinal)) rejects++;
        };

        var one = new float[1];
        int used = 0;
        bool locked = false;
        for (int i = 0; i < audio.Length; i++)
        {
            one[0] = audio[i];
            demod.Process(one);
            used = i + 1;
            if (demod.Lock is { WaveformNumber: >= 0 })
            {
                locked = true;
                break;
            }
        }

        string tail = $" peaks={peaks} rejects={rejects} peakmetric={R(demod.PeakSearchMetric)}";
        if (!locked) return "det=0" + tail;

        var lk = demod.Lock!;
        double chip0 = (double)Chip0.GetValue(demod)!;
        long dsc = (long)DataStartChip.GetValue(demod)!;
        return $"det=1 wn={lk.WaveformNumber} il={(int)lk.Interleaver} k={lk.ConstraintLength}" +
               $" count={dsc / 576 - 1} at={used} t2={(long)Written.GetValue(demod)!}" +
               $" chip0={R(chip0)} data={R(chip0 + (2.0 * dsc))} cfo={R(lk.CfoHz)}" +
               $" omega={R((double)Omega.GetValue(demod)!)} theta={R((double)ThetaBase.GetValue(demod)!)}" + tail;
    }

    // ---- files ----------------------------------------------------------------------------

    public static Dictionary<string, string> Manifest(string refDir, string name)
    {
        var d = new Dictionary<string, string>();
        foreach (string line in File.ReadAllLines(Path.Combine(refDir, name, "manifest.txt")))
        {
            int eq = line.IndexOf('=');
            if (eq > 0) d[line[..eq]] = line[(eq + 1)..];
        }

        return d;
    }

    public static float[] ReadF32(string path)
    {
        byte[] b = File.ReadAllBytes(path);
        var f = new float[b.Length / 4];
        Buffer.BlockCopy(b, 0, f, 0, b.Length);
        return f;
    }

    public static string R(double v) => v.ToString("R", Inv);
}

readonly record struct Scenario(string Case, double SnrDb, double DfHz, double Delay, int Lead, ulong Seed, double Threshold)
{
    public string Describe() =>
        $"{Case} snr={DetectDump.R(SnrDb)} df={DetectDump.R(DfHz)} delay={DetectDump.R(Delay)} lead={Lead} seed={Seed} thr={DetectDump.R(Threshold)}";
}

// The deterministic noise source shared with the C++ tests: splitmix64, uniforms with 53 bits,
// and a unit-variance "Gaussian" as the sum of twelve uniforms less six (Irwin-Hall), which needs
// only exact double additions, so both languages produce the same stream.
sealed class SplitMix(ulong seed)
{
    ulong _state = seed;

    public ulong Next()
    {
        ulong z = unchecked(_state += 0x9E3779B97F4A7C15UL);
        z = unchecked((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9UL);
        z = unchecked((z ^ (z >> 27)) * 0x94D049BB133111EBUL);
        return z ^ (z >> 31);
    }

    public double Uniform() => (Next() >> 11) * (1.0 / 9007199254740992.0);

    public double Gauss()
    {
        double sum = 0;
        for (int i = 0; i < 12; i++) sum += Uniform();
        return sum - 6.0;
    }
}

static class Impair
{
    const int InterpHalf = 4;

    public static float[] NoiseOnly(ulong seed, int samples, double sigma)
    {
        var rng = new SplitMix(seed);
        var x = new float[samples];
        for (int n = 0; n < samples; n++) x[n] = (float)(sigma * rng.Gauss());
        return x;
    }

    // A reference case's complex baseband (9600 Hz, before the 1800 Hz mix), delayed by `Delay`
    // input samples (integer part by shifting, fractional part by the receiver's own 8-tap
    // windowed-sinc interpolator formula in double), put back on a carrier of 1800 + DfHz, with
    // `Lead` samples of lead-in, then seeded noise added at SnrDb measured in 3 kHz (noise power
    // sigma^2 * 3000 / 4800 against the mean signal power over the samples from the end of the
    // lead-in). The stream is long enough for the preamble plus 4000 samples. No noise when
    // SnrDb is NaN.
    public static float[] Build(string refDir, Scenario s)
    {
        var m = DetectDump.Manifest(refDir, s.Case);
        float[] bb = DetectDump.ReadF32(Path.Combine(refDir, s.Case, m["file.baseband_cf32le"]));
        int n = bb.Length / 2;
        int preambleSymbols = int.Parse(m["preamble_symbol_count"], CultureInfo.InvariantCulture);
        int di = (int)Math.Floor(s.Delay);
        double f = s.Delay - di;
        int total = s.Lead + di + (4 * preambleSymbols) + 4000;

        var sig = new double[total];
        for (int k = 0; k < total; k++)
        {
            int mIdx = k - s.Lead - di;
            double re, im;
            if (f == 0)
            {
                if (mIdx >= 0 && mIdx < n) { re = bb[2 * mIdx]; im = bb[(2 * mIdx) + 1]; }
                else { re = 0; im = 0; }
            }
            else
            {
                re = 0; im = 0;
                double pos = mIdx - f;
                long i0 = (long)Math.Floor(pos);
                double frac = pos - i0;
                for (int j = -InterpHalf + 1; j <= InterpHalf; j++)
                {
                    double u = j - frac;
                    double w = Math.Abs(u) < 1e-9
                        ? 1
                        : Math.Sin(Math.PI * u) / (Math.PI * u) * (0.5 + (0.5 * Math.Cos(Math.PI * u / InterpHalf)));
                    long idx = i0 + j;
                    if (idx >= 0 && idx < n)
                    {
                        re += bb[2 * idx] * w;
                        im += bb[(2 * idx) + 1] * w;
                    }
                }
            }

            double phase = 2.0 * Math.PI * (1800.0 + s.DfHz) * k / 9600.0;
            sig[k] = (re * Math.Cos(phase)) - (im * Math.Sin(phase));
        }

        double sigma = 0;
        if (!double.IsNaN(s.SnrDb))
        {
            double power = 0;
            for (int k = s.Lead; k < total; k++) power += sig[k] * sig[k];
            power /= total - s.Lead;
            sigma = Math.Sqrt(power / Math.Pow(10.0, s.SnrDb / 10.0) * (4800.0 / 3000.0));
        }

        var rng = new SplitMix(s.Seed);
        var x = new float[total];
        for (int k = 0; k < total; k++)
        {
            x[k] = (float)(sig[k] + (sigma > 0 ? sigma * rng.Gauss() : 0.0));
        }

        return x;
    }
}
