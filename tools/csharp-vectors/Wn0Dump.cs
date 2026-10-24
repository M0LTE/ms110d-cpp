// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later

// Dumps what the C# receiver does with WN 0 (Walsh) bursts, for the C++ receiver's tests:
//   wn0       <ref-dir>   every WN 0 reference case's audio, through the real demodulator
//                         -> tests/data/csharp-wn0-0.2.0.txt
//   wn0-awgn  <ref-dir>   one reference case with seeded AWGN added (the generator below,
//                         which the C++ test repeats) -> tests/data/csharp-wn0-awgn-0.2.0.txt
//   wn0-poor  <out-stem>  fresh bursts through the Poor channel (M0LTE.HfChannel), their
//                         audio as 16 bit samples and the C# results
//                         -> tests/data/csharp-wn0-poor-0.2.0.txt and .i16le
// Run from the repo root: dotnet run -c Release --project tools/csharp-vectors -- wn0 <ref-dir>
//
// The numbers come from the real M0LTE.Ms110d 0.2.0 demodulator. Its state is internal, so
// the burst's start (chip 0 position, timing, residual carrier, data start) is read from its
// private fields through reflection at the first Walsh symbol, and the soft values come from
// its public FirstPassBlockLlrs event; the per-symbol decisions and finger gains come from the
// text it passes to FrameDiagnostics. Nothing is recomputed here.

using System.Globalization;
using System.Reflection;
using System.Text;
using System.Text.RegularExpressions;
using M0LTE.Fec;
using M0LTE.HfChannel;
using M0LTE.Ms110d;
using M0LTE.Ms110d.Fec;

static class Wn0Dump
{
    static readonly CultureInfo Inv = CultureInfo.InvariantCulture;
    static readonly Regex SymbolLine = new(
        @"^walsh sym=(\d+) d\*=(\d) llr0=\S+ llr1=\S+ \|g\|=\[([^\]]*)\]", RegexOptions.Compiled);

    // Seeded Gaussian noise that the C++ tests reproduce exactly: SplitMix64 for uniforms,
    // Box-Muller for pairs (cos then sin), in double, one value per audio sample.
    sealed class SharedNoise(ulong seed)
    {
        ulong _state = seed;
        double _spare;
        bool _haveSpare;

        ulong NextU64()
        {
            ulong z = _state += 0x9E3779B97F4A7C15UL;
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9UL;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBUL;
            return z ^ (z >> 31);
        }

        double Uniform() => ((NextU64() >> 11) + 0.5) * (1.0 / 9007199254740992.0);

        public double Next()
        {
            if (_haveSpare)
            {
                _haveSpare = false;
                return _spare;
            }

            double r = Math.Sqrt(-2.0 * Math.Log(Uniform()));
            double a = 2.0 * Math.PI * Uniform();
            _spare = r * Math.Sin(a);
            _haveSpare = true;
            return r * Math.Cos(a);
        }
    }

    sealed class Run
    {
        public bool Locked;
        public double Chip0, Tau, Omega, Theta;
        public long DataStart;
        public int IlBits;
        public List<float[]> Blocks = [];
        public List<int> Dibits = [];
        public List<string> Gains = [];
    }

    static T Field<T>(object o, string name) =>
        (T)typeof(Ms110dDemodulator).GetField(name, BindingFlags.NonPublic | BindingFlags.Instance)!.GetValue(o)!;

    // Feeds the audio to a fresh demodulator and records the Walsh path.
    static Run Demodulate(float[] audio, int padSamples)
    {
        var demod = new Ms110dDemodulator();
        var run = new Run();
        demod.FrameDiagnostics += line =>
        {
            Match m = SymbolLine.Match(line);
            if (!m.Success)
            {
                return;
            }

            if (!run.Locked)
            {
                run.Locked = true;
                run.Chip0 = Field<double>(demod, "_chip0");
                run.Tau = Field<double>(demod, "_tau");
                run.Omega = Field<double>(demod, "_omega");
                run.Theta = Field<double>(demod, "_thetaBase");
                run.DataStart = Field<long>(demod, "_dataStartChip");
                run.IlBits = Field<Ms110dInterleaverParams>(demod, "_il").SizeBits;
            }

            run.Dibits.Add(int.Parse(m.Groups[2].Value, Inv));
            run.Gains.Add(m.Groups[3].Value);
        };
        demod.FirstPassBlockLlrs += (_, llrs) => run.Blocks.Add((float[])llrs.Clone());
        const int chunk = 2400;
        for (int i = 0; i < audio.Length; i += chunk)
        {
            demod.Process(audio.AsSpan(i, Math.Min(chunk, audio.Length - i)));
        }

        demod.Process(new float[padSamples]);
        return run;
    }

    static string R(float v) => v.ToString("R", Inv);
    static string R(double v) => v.ToString("R", Inv);

    static float[] ReadAudio(string path)
    {
        byte[] raw = File.ReadAllBytes(path);
        var a = new float[raw.Length / 4];
        Buffer.BlockCopy(raw, 0, a, 0, raw.Length);
        return a;
    }

    static Dictionary<string, string> Manifest(string path) =>
        File.ReadAllLines(path).Where(l => l.Contains('=')).ToDictionary(
            l => l[..l.IndexOf('=')], l => l[(l.IndexOf('=') + 1)..]);

    static void StartLine(StringBuilder sb, Run r) =>
        sb.Append($"start chip0 {R(r.Chip0)} tau {R(r.Tau)} omega {R(r.Omega)} theta {R(r.Theta)} " +
                  $"data_start {r.DataStart} il_bits {r.IlBits}\n");

    // ---------------------------------------------------------------- wn0

    public static string Clean(string refDir)
    {
        var sb = new StringBuilder();
        sb.Append("# Dumped from M0LTE.Ms110d 0.2.0 by tools/csharp-vectors (wn0 mode): the real demodulator on each WN 0 reference case's audio.\n");
        sb.Append("# start: the burst's start as read from the demodulator at its first Walsh symbol. sym: global symbol number, winning di-bit,\n");
        sb.Append("# the two soft values (first pass, wire order) and, for the first 12 symbols of a case, the 13 finger gain magnitudes (3 decimals).\n");
        foreach (string name in File.ReadAllLines(Path.Combine(refDir, "index.txt")).Where(l => l.StartsWith("case=wn0_")).Select(l => l[5..]))
        {
            var m = Manifest(Path.Combine(refDir, name, "manifest.txt"));
            int blocks = int.Parse(m["blocks"], Inv);
            Run r = Demodulate(ReadAudio(Path.Combine(refDir, name, "13_audio.f32")), 9600);
            sb.Append($"case {name} locked {(r.Locked ? 1 : 0)} blocks {r.Blocks.Count}\n");
            if (!r.Locked || r.Blocks.Count < blocks)
            {
                throw new InvalidOperationException($"{name}: the C# did not decode all {blocks} blocks");
            }

            StartLine(sb, r);
            int g = 0;
            for (int b = 0; b < blocks; b++)
            {
                for (int s = 0; s < r.IlBits / 2; s++, g++)
                {
                    sb.Append($"sym {g} {r.Dibits[g]} {R(r.Blocks[b][2 * s])} {R(r.Blocks[b][(2 * s) + 1])}");
                    if (g < 12)
                    {
                        sb.Append(" g ").Append(r.Gains[g]);
                    }

                    sb.Append('\n');
                }
            }
        }

        return sb.ToString();
    }

    // ---------------------------------------------------------------- wn0-awgn

    static readonly (string Name, double SnrDb)[] AwgnPoints = [("a", -8), ("b", -10)];
    const int AwgnTrials = 100;
    const int LeadIn = 2400;
    const int LeadOut = 4800;

    public static string Awgn(string refDir)
    {
        const string name = "wn0_short_k7_s1_p100";
        float[] baseAudio = ReadAudio(Path.Combine(refDir, name, "13_audio.f32"));
        double power = baseAudio.Select(x => (double)x * x).Average();
        var sb = new StringBuilder();
        sb.Append("# Dumped from M0LTE.Ms110d 0.2.0 by tools/csharp-vectors (wn0-awgn mode).\n");
        sb.Append($"# Base signal: reference case {name} audio ({baseAudio.Length} samples) with {LeadIn} zero samples before and {LeadOut} after,\n");
        sb.Append("# plus seeded Gaussian noise: sample = base + (float)(sigma * z), z from SplitMix64 + Box-Muller (see Wn0Dump.cs), seed per trial.\n");
        sb.Append("# SNR is signal power over noise power in 3000 Hz: sigma^2 = P / 10^(snr/10) * 4800 / 3000, P = mean square of the base audio.\n");
        sb.Append($"base {name} {baseAudio.Length} leadin {LeadIn} leadout {LeadOut}\n");
        foreach (var (label, snr) in AwgnPoints)
        {
            double sigma = Math.Sqrt(power / Math.Pow(10, snr / 10) * 4800.0 / 3000.0);
            sb.Append($"point {label} snr_db {R(snr)} sigma {R(sigma)} trials {AwgnTrials}\n");
            for (int t = 0; t < AwgnTrials; t++)
            {
                ulong seed = (ulong)(1000 * (1 + Array.IndexOf(AwgnPoints, (label, snr))) + t);
                var noise = new SharedNoise(seed);
                var audio = new float[LeadIn + baseAudio.Length + LeadOut];
                for (int i = 0; i < audio.Length; i++)
                {
                    float b = i >= LeadIn && i < LeadIn + baseAudio.Length ? baseAudio[i - LeadIn] : 0f;
                    audio[i] = b + (float)(sigma * noise.Next());
                }

                Run r = Demodulate(audio, 4800);
                bool ok = r.Locked && r.Blocks.Count >= 4;
                sb.Append($"trial {label} {seed} locked {(ok ? 1 : 0)}\n");
                if (!ok)
                {
                    continue;
                }

                StartLine(sb, r);
                sb.Append("dibits ").Append(string.Concat(r.Dibits.Take(4 * r.IlBits / 2))).Append('\n');
                sb.Append("bits ").Append(string.Concat(r.Blocks.Take(4).SelectMany(b => b).Select(l => l < 0 ? '1' : '0'))).Append('\n');
            }
        }

        return sb.ToString();
    }

    // ---------------------------------------------------------------- wn0-poor

    static readonly (string Name, double SnrDb)[] PoorPoints = [("a", -6), ("b", -3)];
    const int PoorBursts = 10;
    const double Scale = 8192;  // 16 bit audio: 1.0 = 8192, full scale +-4

    public static void Poor(string stem)
    {
        var il = Ms110dInterleaverParams.Get3k(0, Ms110dInterleaverKind.Short);
        var settings = new Ms110dTxSettings { WaveformNumber = 0, Interleaver = Ms110dInterleaverKind.Short };
        var tx = new Ms110dModulator(settings);
        var code = ConvolutionalCode.K7;
        var puncture = Ms110dPuncture.Get(code, tx.Mode.CodeRate);
        var interleaver = new Ms110dInterleaver(il.SizeBits, il.Increment);
        const int payloadBits = (2 * 40) - 32;  // two blocks with the EOM

        var text = new StringBuilder();
        text.Append("# Dumped from M0LTE.Ms110d 0.2.0 by tools/csharp-vectors (wn0-poor mode): WN 0 short interleaver K7 bursts of two blocks through\n");
        text.Append("# M0LTE.HfChannel 0.1.0 WattersonChannel.Poor (2 equal Rayleigh paths, 2 ms, 1 Hz), 2400 samples of noise either side, SNR in 3000 Hz.\n");
        text.Append("# The audio is stored as 16 bit little-endian samples (1.0 = 8192) in the .i16le file, trimmed to the data plus a margin; the C#\n");
        text.Append("# was run on the same quantised audio. start is relative to the trimmed audio. wire: the transmitted fetched bits; dibits: what the\n");
        text.Append("# C# decided per symbol; llrs: its first-pass soft values.\n");
        using var bin = new FileStream(stem + ".i16le", FileMode.Create);
        long byteOffset = 0;
        foreach (var (label, snr) in PoorPoints)
        {
            text.Append($"point {label} snr_db {R(snr)} bursts {PoorBursts}\n");
            for (int n = 0; n < PoorBursts; n++)
            {
                int seed = 7000 + (100 * Array.IndexOf(PoorPoints, (label, snr))) + n;
                var rng = new Random(seed);
                var payload = new byte[payloadBits];
                for (int i = 0; i < payload.Length; i++)
                {
                    payload[i] = (byte)rng.Next(2);
                }

                float[] audio = tx.Modulate(payload);
                var channel = new WattersonChannel(9600, seed + 1, WattersonChannel.Poor);
                float[] rx = channel.Apply(audio, snr, leadInSamples: LeadIn, leadOutSamples: LeadIn);
                var q = new short[rx.Length];
                var quantised = new float[rx.Length];
                for (int i = 0; i < rx.Length; i++)
                {
                    q[i] = (short)Math.Clamp(Math.Round(rx[i] * Scale), short.MinValue, short.MaxValue);
                    quantised[i] = (float)(q[i] / Scale);
                }

                Run r = Demodulate(quantised, 4800);
                bool ok = r.Locked && r.Blocks.Count >= 2;
                text.Append($"burst {label} {seed} locked {(ok ? 1 : 0)}\n");
                if (!ok)
                {
                    continue;
                }

                // Trim: from before the first sample the first symbol reads (a multiple of 16
                // audio samples so the mixer phase and the half-chip pairing are unchanged)
                // to after the last one.
                int symbols = 2 * il.SizeBits / 2;
                double firstPos = r.Chip0 + (2.0 * (r.DataStart - 6)) + r.Tau - 4;
                double lastPos = r.Chip0 + (2.0 * (r.DataStart + (32L * symbols) + 40)) + r.Tau + 8;
                int off = Math.Max(0, ((int)Math.Floor((2 * firstPos) - 400)) / 16 * 16);
                int end = Math.Min(rx.Length, (int)Math.Ceiling((2 * lastPos) + 400));
                var bytes = new byte[2];
                for (int i = off; i < end; i++)
                {
                    BitConverter.TryWriteBytes(bytes, q[i]);
                    bin.Write(bytes);
                }

                text.Append($"audio offset {byteOffset} samples {end - off}\n");
                byteOffset += 2L * (end - off);
                text.Append($"start chip0 {R(r.Chip0 - (off / 2.0))} tau {R(r.Tau)} omega {R(r.Omega)} theta {R(r.Theta)} " +
                            $"data_start {r.DataStart} il_bits {r.IlBits}\n");
                byte[] txBits = Ms110dFraming.BuildTxBits(payload, appendEom: true, il.InputBits);
                var wire = new StringBuilder();
                for (int b = 0; b < txBits.Length / il.InputBits; b++)
                {
                    byte[] fetched = Ms110dFraming.EncodeBlock(
                        code, puncture, interleaver, txBits.AsSpan(b * il.InputBits, il.InputBits));
                    wire.Append(string.Concat(fetched));
                }

                text.Append("wire ").Append(wire).Append('\n');
                text.Append("dibits ").Append(string.Concat(r.Dibits.Take(symbols))).Append('\n');
                text.Append("llrs ").Append(string.Join(' ', r.Blocks.Take(2).SelectMany(b => b).Select(R))).Append('\n');
            }
        }

        File.WriteAllText(stem + ".txt", text.ToString());
    }
}
