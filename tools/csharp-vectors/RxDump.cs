// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later

// Dumps what the C# receiver does after the preamble for the equaliser waveforms (WN 1 and up),
// for the C++ DfeReceiver's tests (docs/dev/dfe-receiver.md):
//   rx       <ref-dir>   every WN 1 to 5 ultra-short and short reference case's audio through the
//                        real demodulator -> tests/data/csharp-rx-0.3.0.txt
//   rx-awgn  <ref-dir>   short-interleaver cases with seeded AWGN added (the generator the C++
//                        test repeats) -> tests/data/csharp-rx-awgn-0.3.0.txt
// Run from the repo root: dotnet run -c Release --project tools/csharp-vectors -- rx <ref-dir>
//
// Each burst is demodulated twice: with the turbo pass off (DisableTurbo, what the C++ port
// does so far) and with the shipped default. The first-pass soft values must be identical in the
// two (the turbo pass saves and restores every piece of tracking state); the tool fails if they
// are not. The tracking state per frame is read from the demodulator's private fields through
// reflection at its FrameDiagnostics point, the presence statistic from PresenceTraced, the
// soft values from FirstPassBlockLlrs and the decoded bits from BlockDecoded. Nothing is
// recomputed here.

using System.Globalization;
using System.Reflection;
using System.Text;
using System.Text.RegularExpressions;
using M0LTE.Ms110d;

static class RxDump
{
    static readonly CultureInfo Inv = CultureInfo.InvariantCulture;
    static readonly Regex FrameLine = new(
        @"^frame@(-?\d+): gain=(\S+) .*tapChange=(\S+) .*preMse=(\S+) .*fresh=(\S+) ", RegexOptions.Compiled);

    sealed class Frame
    {
        public long FrameChip, AtT2;
        public double Omega, Theta, Tau, GainRef, EnergyRef, ProbeMse, FadeFloor, Presence, Plain, TapEnergy;
        public bool Fading, Latched, Fresh;
        public int Bad;
        public string Gain = "", TapChange = "", PreMse = "";
        public ulong TapHash;
    }

    sealed class Block
    {
        public long AtT2;
        public float[] Llrs = [];
        public byte[] Info = [];
    }

    sealed class Run
    {
        public List<Frame> Frames = [];
        public List<Block> Blocks = [];
        public string Reason = "None";
        public int EndBlocks;
        public int PayloadBits = -1;
        public long EndAtT2;
        public float Agc = 1;
        public int Waveform = -1;
    }

    static object? Get(object o, string name) =>
        o.GetType().GetField(name, BindingFlags.NonPublic | BindingFlags.Instance)!.GetValue(o);

    static ulong Fnv(IEnumerable<uint> values)
    {
        ulong h = 14695981039346656037UL;
        foreach (uint v in values)
        {
            for (int b = 0; b < 4; b++)
            {
                h ^= (v >> (8 * b)) & 0xFF;
                h *= 1099511628211UL;
            }
        }

        return h;
    }

    static Run Demodulate(float[] audio, bool disableTurbo)
    {
        var demod = new Ms110dDemodulator(new Ms110dDemodOptions { DisableTurbo = disableTurbo });
        var run = new Run();
        double lastPresence = 0, lastPlain = 0;
        demod.PresenceTraced += (_, plain, presence) =>
        {
            lastPlain = plain;
            lastPresence = presence;
        };
        demod.FrameDiagnostics += line =>
        {
            Match m = FrameLine.Match(line);
            if (!m.Success || run.Reason != "None")
            {
                return;
            }

            var dfe = (Dfe)Get(demod, "_dfe")!;
            Complex32[] taps = dfe.SnapshotTaps();
            double tapEnergy = 0;
            foreach (Complex32 t in taps)
            {
                tapEnergy += (double)t.Re * t.Re + (double)t.Im * t.Im;
            }

            if (run.Frames.Count == 0)
            {
                run.Agc = (float)Get(demod, "_agcGain")!;
                run.Waveform = ((Ms110dLockInfo)Get(demod, "_lock")!).WaveformNumber;
            }

            run.Frames.Add(new Frame
            {
                FrameChip = (long)Get(demod, "_frameChip")!,
                AtT2 = (long)Get(demod, "_written")!,
                Omega = (double)Get(demod, "_omega")!,
                Theta = (double)Get(demod, "_thetaBase")!,
                Tau = (double)Get(demod, "_tau")!,
                GainRef = (double)Get(demod, "_probeGainRef")!,
                EnergyRef = (double)Get(demod, "_probeEnergyRef")!,
                ProbeMse = (double)Get(demod, "_probeMse")!,
                FadeFloor = (double)Get(demod, "_fadeFloor")!,
                Fading = (bool)Get(demod, "_fading")!,
                Latched = (bool)Get(demod, "_fadingLatched")!,
                Bad = (int)Get(demod, "_badProbes")!,
                Presence = lastPresence,
                Plain = lastPlain,
                Gain = m.Groups[2].Value,
                TapChange = m.Groups[3].Value,
                PreMse = m.Groups[4].Value,
                Fresh = m.Groups[5].Value == "True",
                TapHash = Fnv(taps.SelectMany(t => new[] { BitConverter.SingleToUInt32Bits(t.Re), BitConverter.SingleToUInt32Bits(t.Im) })),
                TapEnergy = tapEnergy,
            });
        };
        demod.FirstPassBlockLlrs += (_, llrs) =>
        {
            if (run.Reason == "None")
            {
                run.Blocks.Add(new Block
        {
            AtT2 = (long)Get(demod, "_written")!,
                Llrs = (float[])llrs.Clone(),
                });
            }
        };
        demod.BlockDecoded += b =>
        {
            if (run.Reason == "None")
            {
                run.Blocks[b.Index].Info = b.Bits;
            }
        };
        demod.BurstCompleted += bu =>
        {
            if (run.Reason != "None")
            {
                return;  // the first burst only
            }

            run.Reason = bu.Reason.ToString();
            run.EndBlocks = bu.Blocks;
            run.PayloadBits = bu.PayloadBits.Length;
            run.EndAtT2 = (long)Get(demod, "_written")!;
        };
        const int chunk = 2400;
        for (int i = 0; i < audio.Length; i += chunk)
        {
            demod.Process(audio.AsSpan(i, Math.Min(chunk, audio.Length - i)));
        }

        return run;
    }

    static string R(double v) => v.ToString("R", Inv);
    static string R(float v) => v.ToString("R", Inv);

    static string Bits(byte[] bits)
    {
        var sb = new StringBuilder(bits.Length);
        foreach (byte b in bits)
        {
            sb.Append(b == 0 ? '0' : '1');
        }

        return sb.ToString();
    }

    static string Base64(float[] values)
    {
        var raw = new byte[values.Length * 4];
        Buffer.BlockCopy(values, 0, raw, 0, raw.Length);
        return Convert.ToBase64String(raw);
    }

    static float[] ReadAudio(string path)
    {
        byte[] raw = File.ReadAllBytes(path);
        var a = new float[raw.Length / 4];
        Buffer.BlockCopy(raw, 0, a, 0, raw.Length);
        return a;
    }

    // Both runs, the first-pass check, and the pair as one result: frames, soft values and the end
    // from the turbo-off run, and the turbo-on decode next to it.
    static (Run NoTurbo, Run Turbo) Both(float[] audio, string what)
    {
        Run a = Demodulate(audio, disableTurbo: true);
        Run b = Demodulate(audio, disableTurbo: false);
        if (a.Blocks.Count != b.Blocks.Count && a.Reason == b.Reason)
        {
            throw new InvalidOperationException($"{what}: block counts differ with and without turbo");
        }

        for (int i = 0; i < Math.Min(a.Blocks.Count, b.Blocks.Count); i++)
        {
            if (!a.Blocks[i].Llrs.AsSpan().SequenceEqual(b.Blocks[i].Llrs))
            {
                throw new InvalidOperationException($"{what}: block {i} first-pass soft values differ with turbo on");
            }
        }

        return (a, b);
    }

    static void FrameLines(StringBuilder sb, Run r)
    {
        for (int i = 0; i < r.Frames.Count; i++)
        {
            Frame f = r.Frames[i];
            sb.Append($"frame {i} {f.FrameChip} {f.AtT2} omega {R(f.Omega)} theta {R(f.Theta)} tau {R(f.Tau)} ");
            sb.Append($"gainref {R(f.GainRef)} energyref {R(f.EnergyRef)} probemse {R(f.ProbeMse)} floor {R(f.FadeFloor)} ");
            sb.Append($"fading {(f.Fading ? 1 : 0)} latched {(f.Latched ? 1 : 0)} fresh {(f.Fresh ? 1 : 0)} bad {f.Bad} ");
            sb.Append($"presence {R(f.Presence)} plain {R(f.Plain)} ");
            sb.Append($"gain {f.Gain} tapchange {f.TapChange} premse {f.PreMse} taps {f.TapHash:x16} tapenergy {R(f.TapEnergy)}\n");
        }
    }

    static void EndLine(StringBuilder sb, Run r) =>
        sb.Append($"end {r.Reason} {r.EndBlocks} {r.PayloadBits} {r.EndAtT2}\n");

    // ---------------------------------------------------------------- rx

    public static string Clean(string refDir)
    {
        var sb = new StringBuilder();
        sb.Append("# Dumped from M0LTE.Ms110d 0.3.0 by tools/csharp-vectors (rx mode): the real demodulator on each WN 1 to 5 ultra-short and short\n");
        sb.Append("# reference case's audio (fed in chunks of 2400 samples), turbo off; see RxDump.cs. frame: index, first data chip, T/2 samples\n");
        sb.Append("# received, then the private tracking state after the frame (round-trip doubles), the presence statistic,\n");
        sb.Append("# three values from the diagnostic line (3 or 4 decimals) and an FNV-1a hash and the energy of the taps.\n");
        sb.Append("# block: index, T/2 samples received, decoded bits (turbo off), decoded bits (turbo on), first-pass soft values as float32 LE base64.\n");
        sb.Append("# end: reason, blocks, payload bits, T/2 samples received.\n");
        foreach (string name in File.ReadAllLines(Path.Combine(refDir, "index.txt"))
                     .Where(l => Regex.IsMatch(l, @"^case=wn[1-5]_(short|ultrashort)_")).Select(l => l[5..]))
        {
            float[] audio = ReadAudio(ReferencePaths.AudioPath(refDir, name));
            // Room after the burst for the last block to complete.
            float[] padded = new float[audio.Length + 9600];
            audio.CopyTo(padded, 0);
            (Run r, Run t) = Both(padded, name);
            sb.Append($"case {name} wn {r.Waveform} agc {R(r.Agc)} frames {r.Frames.Count} blocks {r.Blocks.Count}\n");
            FrameLines(sb, r);
            for (int b = 0; b < r.Blocks.Count; b++)
            {
                Block k = r.Blocks[b];
                string turbo = b < t.Blocks.Count ? Bits(t.Blocks[b].Info) : "-";
                sb.Append($"block {b} {k.AtT2} {Bits(k.Info)} {turbo} {Base64(k.Llrs)}\n");
            }

            EndLine(sb, r);
        }

        return sb.ToString();
    }

    // ---------------------------------------------------------------- rx-awgn

    // Per waveform: two SNRs in 3 kHz, one near where the first pass starts to need its code.
    static readonly (int Wn, double[] Snr)[] AwgnPoints =
    [
        (1, [-9.0, -6.0]),
        (2, [-6.0, -3.0]),
        (3, [-3.0, 0.0]),
        (4, [0.0, 3.0]),
        (5, [3.0, 6.0]),
    ];

    const int AwgnTrials = 20;
    const int LeadIn = 2400;
    const int LeadOut = 9600;

    public static string Awgn(string refDir)
    {
        var sb = new StringBuilder();
        sb.Append("# Dumped from M0LTE.Ms110d 0.3.0 by tools/csharp-vectors (rx-awgn mode), turbo off unless stated; see RxDump.cs.\n");
        sb.Append($"# Base signal: the reference case's audio with {LeadIn} zero samples before and {LeadOut} after, plus seeded Gaussian noise:\n");
        sb.Append("# sample = base + (float)(sigma * z), z from SplitMix64 + Box-Muller (Wn0Dump.SharedNoise), seed per trial.\n");
        sb.Append("# SNR is signal power over noise power in 3000 Hz: sigma^2 = P / 10^(snr/10) * 4800 / 3000, P = mean square of the base audio.\n");
        sb.Append("# trial: label, seed, frames. last: the tracking state after the last frame. block: index, T/2 samples, decoded bits (turbo off),\n");
        sb.Append("# decoded bits (turbo on), FNV-1a of the soft values' float bits, signs ('1' for negative) as hex nibbles, sum of |soft value|.\n");
        foreach ((int wn, double[] snrs) in AwgnPoints)
        {
            string name = $"wn{wn}_short_k7_s1_p100";
            float[] baseAudio = ReadAudio(ReferencePaths.AudioPath(refDir, name));
            double power = baseAudio.Select(x => (double)x * x).Average();
            sb.Append($"base {name} {baseAudio.Length} leadin {LeadIn} leadout {LeadOut}\n");
            for (int p = 0; p < snrs.Length; p++)
            {
                double snr = snrs[p];
                double sigma = Math.Sqrt(power / Math.Pow(10, snr / 10) * 4800.0 / 3000.0);
                string label = $"wn{wn}{(char)('a' + p)}";
                sb.Append($"point {label} snr_db {R(snr)} sigma {R(sigma)} trials {AwgnTrials}\n");
                for (int t = 0; t < AwgnTrials; t++)
                {
                    ulong seed = (ulong)((10000 * wn) + (1000 * (1 + p)) + t);
                    var noise = new Wn0Dump.SharedNoise(seed);
                    var audio = new float[LeadIn + baseAudio.Length + LeadOut];
                    for (int i = 0; i < audio.Length; i++)
                    {
                        int k = i - LeadIn;
                        float b = k >= 0 && k < baseAudio.Length ? baseAudio[k] : 0f;
                        audio[i] = b + (float)(sigma * noise.Next());
                    }

                    (Run r, Run tr) = Both(audio, $"{label} trial {t}");
                    sb.Append($"trial {label} {seed} wn {r.Waveform} frames {r.Frames.Count} blocks {r.Blocks.Count}\n");
                    if (r.Frames.Count > 0)
                    {
                        Frame f = r.Frames[^1];
                        sb.Append($"last omega {R(f.Omega)} theta {R(f.Theta)} tau {R(f.Tau)} gainref {R(f.GainRef)} ");
                        sb.Append($"presence {R(f.Presence)} latched {(f.Latched ? 1 : 0)} taps {f.TapHash:x16} tapenergy {R(f.TapEnergy)}\n");
                    }

                    for (int b = 0; b < r.Blocks.Count; b++)
                    {
                        Block k = r.Blocks[b];
                        string turbo = b < tr.Blocks.Count ? Bits(tr.Blocks[b].Info) : "-";
                        ulong hash = Fnv(k.Llrs.Select(BitConverter.SingleToUInt32Bits));
                        var signs = new StringBuilder();
                        for (int i = 0; i < k.Llrs.Length; i += 4)
                        {
                            int nib = 0;
                            for (int j = 0; j < 4 && i + j < k.Llrs.Length; j++)
                            {
                                nib |= (k.Llrs[i + j] < 0 ? 1 : 0) << j;
                            }

                            signs.Append("0123456789abcdef"[nib]);
                        }

                        double sumAbs = k.Llrs.Sum(x => (double)Math.Abs(x));
                        sb.Append($"block {b} {k.AtT2} {Bits(k.Info)} {turbo} {hash:x16} {signs} {R(sumAbs)}\n");
                    }

                    EndLine(sb, r);
                }
            }
        }

        // Cut bursts: the signal stops half way through and 20 s of noise follow (with a steady tone
        // on a probe line in odd trials), so the receiver runs into its signal-lost or signal-absent
        // exit and the presence statistic's carrier cancellation is exercised.
        sb.Append("# cut: the base audio is kept only before sample 'cut', then noise alone (plus, in odd trials, a tone of amplitude 'tone' at 'tone_hz')\n");
        foreach ((int wn, double snr) in new[] { (2, -3.0), (4, 3.0) })
        {
            string name = $"wn{wn}_short_k7_s2_p{(wn == 2 ? 448 : 1248)}";
            float[] baseAudio = ReadAudio(ReferencePaths.AudioPath(refDir, name));
            double power = baseAudio.Select(x => (double)x * x).Average();
            int cut = baseAudio.Length / 2;
            const int cutLeadOut = 20 * 9600;
            const double toneAmp = 0.1, toneHz = 2100.0;
            double sigma = Math.Sqrt(power / Math.Pow(10, snr / 10) * 4800.0 / 3000.0);
            string label = $"wn{wn}cut";
            sb.Append($"cutbase {name} {baseAudio.Length} leadin {LeadIn} leadout {cutLeadOut} cut {cut} tone {R(toneAmp)} tone_hz {R(toneHz)}\n");
            sb.Append($"point {label} snr_db {R(snr)} sigma {R(sigma)} trials 6\n");
            for (int t = 0; t < 6; t++)
            {
                ulong seed = (ulong)(70000 + (1000 * wn) + t);
                var noise = new Wn0Dump.SharedNoise(seed);
                var audio = new float[LeadIn + baseAudio.Length + cutLeadOut];
                for (int i = 0; i < audio.Length; i++)
                {
                    int k = i - LeadIn;
                    float b = k >= 0 && k < cut ? baseAudio[k] : 0f;
                    float tone = t % 2 == 1 && k >= cut ? (float)(toneAmp * Math.Cos(2.0 * Math.PI * toneHz * k / 9600.0)) : 0f;
                    audio[i] = b + tone + (float)(sigma * noise.Next());
                }

                (Run r, Run tr) = Both(audio, $"{label} trial {t}");
                sb.Append($"trial {label} {seed} wn {r.Waveform} frames {r.Frames.Count} blocks {r.Blocks.Count}\n");
                if (r.Frames.Count > 0)
                {
                    Frame f = r.Frames[^1];
                    sb.Append($"last omega {R(f.Omega)} theta {R(f.Theta)} tau {R(f.Tau)} gainref {R(f.GainRef)} ");
                    sb.Append($"presence {R(f.Presence)} latched {(f.Latched ? 1 : 0)} taps {f.TapHash:x16} tapenergy {R(f.TapEnergy)}\n");
                }

                for (int b = 0; b < r.Blocks.Count; b++)
                {
                    Block k = r.Blocks[b];
                    string turbo = b < tr.Blocks.Count ? Bits(tr.Blocks[b].Info) : "-";
                    ulong hash = Fnv(k.Llrs.Select(BitConverter.SingleToUInt32Bits));
                    var signs = new StringBuilder();
                    for (int i = 0; i < k.Llrs.Length; i += 4)
                    {
                        int nib = 0;
                        for (int j = 0; j < 4 && i + j < k.Llrs.Length; j++)
                        {
                            nib |= (k.Llrs[i + j] < 0 ? 1 : 0) << j;
                        }

                        signs.Append("0123456789abcdef"[nib]);
                    }

                    double sumAbs = k.Llrs.Sum(x => (double)Math.Abs(x));
                    sb.Append($"block {b} {k.AtT2} {Bits(k.Info)} {turbo} {hash:x16} {signs} {R(sumAbs)}\n");
                }

                EndLine(sb, r);
            }
        }

        return sb.ToString();
    }
}
