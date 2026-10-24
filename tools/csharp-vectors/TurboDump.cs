// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later

// Dumps what the shipped C# receiver (turbo on) does with whole bursts, for the C++ turbo stage,
// MFB decoder and probe pattern tests (docs/dev/dfe-receiver.md):
//   rx-turbo   <ref-dir>   every WN 1 to 8 and 13 reference case, every interleaver
//                          -> tests/data/csharp-rx-turbo-0.2.1.txt
//   rx-corpus  <out-stem>  fresh bursts, never committed (scripts/generate-rx-corpus.sh): the
//                          Poor channel (M0LTE.HfChannel) on every equaliser mode, and bursts
//                          sent with the D.5.2.2 standard probe pattern through the C#
//                          modulator's internal test switch, clean and in noise. Audio as 16 bit
//                          samples in <stem>.i16le, results in <stem>.txt.
// Run from the repo root: dotnet run -c Release --project tools/csharp-vectors -- rx-turbo <ref-dir>
//
// Per frame the tracking state is folded into a hash (and the last frame of each block written
// out); per block the first-pass and the last turbo iterate's soft values (hash, signs, sum of
// magnitudes), the decode, and what the turbo stage did, from the public counters.

using System.Globalization;
using System.Reflection;
using System.Text;
using System.Text.RegularExpressions;
using M0LTE.HfChannel;
using M0LTE.Ms110d;

static class TurboDump
{
    static readonly CultureInfo Inv = CultureInfo.InvariantCulture;

    static object? Get(object o, string name) =>
        o.GetType().GetField(name, BindingFlags.NonPublic | BindingFlags.Instance)!.GetValue(o);

    static string R(double v) => v.ToString("R", Inv);

    sealed class Fnv
    {
        public ulong H = 14695981039346656037UL;

        public void Add(uint v)
        {
            for (int b = 0; b < 4; b++)
            {
                H ^= (v >> (8 * b)) & 0xFF;
                H *= 1099511628211UL;
            }
        }

        public void Add(double d)
        {
            ulong v = BitConverter.DoubleToUInt64Bits(d);
            Add((uint)v);
            Add((uint)(v >> 32));
        }
    }

    sealed class Block
    {
        public long AtT2;
        public float[] First = [];
        public float[]? Turbo;
        public byte[] Info = [];
        public string Outcome = "?";
        public int MfbOffered, MfbSelected;
        public ulong FrameHash;
        public int Frames;
        public string LastFrame = "";
    }

    sealed class Run
    {
        public List<Block> Blocks = [];
        public string Reason = "None";
        public int EndBlocks;
        public int PayloadBits = -1;
        public long EndAtT2;
        public int Waveform = -1;
        public int TotalFrames;
        public byte[] Payload = [];
    }

    static string Signs(float[] llrs)
    {
        var s = new StringBuilder((llrs.Length + 3) / 4);
        for (int i = 0; i < llrs.Length; i += 4)
        {
            int nib = 0;
            for (int j = 0; j < 4 && i + j < llrs.Length; j++)
            {
                nib |= (llrs[i + j] < 0 ? 1 : 0) << j;
            }

            s.Append("0123456789abcdef"[nib]);
        }

        return s.ToString();
    }

    static string Soft(float[] llrs)
    {
        var f = new Fnv();
        foreach (float x in llrs)
        {
            f.Add(BitConverter.SingleToUInt32Bits(x));
        }

        return $"{f.H:x16} {Signs(llrs)} {R(llrs.Sum(x => (double)Math.Abs(x)))}";
    }

    static string Bits(byte[] bits) => string.Concat(bits.Select(b => b == 0 ? '0' : '1'));

    // A bit sequence as its length and an FNV-1a hash of one 32 bit word per bit.
    static string BitHash(IEnumerable<byte> bits)
    {
        var f = new Fnv();
        int n = 0;
        foreach (byte b in bits)
        {
            f.Add(b);
            n++;
        }

        return $"{n} {f.H:x16}";
    }

    // Soft values as the hash of their float bits, the hash of their signs and the sum of magnitudes.
    static string SoftHashed(float[] llrs)
    {
        var f = new Fnv();
        foreach (float x in llrs)
        {
            f.Add(BitConverter.SingleToUInt32Bits(x));
        }

        return $"{f.H:x16} {BitHash(llrs.Select(x => (byte)(x < 0 ? 1 : 0)))} {R(llrs.Sum(x => (double)Math.Abs(x)))}";
    }

    static Run Demodulate(float[] audio)
    {
        var demod = new Ms110dDemodulator();
        var run = new Run();
        var frameHash = new Fnv();
        int frames = 0;
        string lastFrame = "";
        double presence = 0, plain = 0;
        float[]? first = null, turbo = null;
        int conv = 0, rev = 0, salv = 0, abort = 0, skip = 0, offered = 0, selected = 0;
        demod.PresenceTraced += (_, p, q) =>
        {
            plain = p;
            presence = q;
        };
        demod.FrameDiagnostics += line =>
        {
            if (!line.StartsWith("frame@", StringComparison.Ordinal) || run.Reason != "None")
            {
                return;
            }

            if (run.Waveform < 0)
            {
                run.Waveform = ((Ms110dLockInfo)Get(demod, "_lock")!).WaveformNumber;
            }

            var dfe = (Dfe)Get(demod, "_dfe")!;
            double omega = (double)Get(demod, "_omega")!, theta = (double)Get(demod, "_thetaBase")!, tau = (double)Get(demod, "_tau")!;
            double gainRef = (double)Get(demod, "_probeGainRef")!, energyRef = (double)Get(demod, "_probeEnergyRef")!;
            double mse = (double)Get(demod, "_probeMse")!, floor = (double)Get(demod, "_fadeFloor")!;
            bool latched = (bool)Get(demod, "_fadingLatched")!;
            double evidence = (double)Get(Get(demod, "_probePattern")!, "_evidence")!;
            foreach (double d in new[] { omega, theta, tau, gainRef, energyRef, mse, floor, presence, plain, evidence })
            {
                frameHash.Add(d);
            }

            foreach (Complex32 t in dfe.SnapshotTaps())
            {
                frameHash.Add(BitConverter.SingleToUInt32Bits(t.Re));
                frameHash.Add(BitConverter.SingleToUInt32Bits(t.Im));
            }

            frameHash.Add(latched ? 1u : 0u);
            frames++;
            run.TotalFrames++;
            lastFrame = $"omega {R(omega)} theta {R(theta)} tau {R(tau)} gainref {R(gainRef)} presence {R(presence)} evidence {R(evidence)} latched {(latched ? 1 : 0)}";
        };
        demod.FirstPassBlockLlrs += (_, l) =>
        {
            if (run.Reason == "None")
            {
                first = (float[])l.Clone();
                turbo = null;
            }
        };
        demod.TurboBlockLlrs += (_, l) =>
        {
            if (run.Reason == "None")
            {
                turbo = (float[])l.Clone();
            }
        };
        demod.BlockDecoded += b =>
        {
            if (run.Reason != "None")
            {
                return;
            }

            string outcome =
                demod.TurboSalvaged > salv ? "salvaged" :
                demod.TurboConverged > conv ? "converged" :
                demod.TurboReverted > rev ? "reverted" :
                demod.TurboAborted > abort ? "aborted" :
                demod.TurboSkipped > skip ? "skipped" : "?";
            var block = new Block
            {
                AtT2 = (long)Get(demod, "_written")!,
                First = first!,
                Turbo = turbo,
                Info = b.Bits,
                Outcome = outcome,
                MfbOffered = demod.MfbOffered - offered,
                MfbSelected = demod.MfbSelected - selected,
                FrameHash = frameHash.H,
                Frames = frames,
                LastFrame = lastFrame,
            };
            run.Blocks.Add(block);
            conv = demod.TurboConverged;
            rev = demod.TurboReverted;
            salv = demod.TurboSalvaged;
            abort = demod.TurboAborted;
            skip = demod.TurboSkipped;
            offered = demod.MfbOffered;
            selected = demod.MfbSelected;
            frameHash = new Fnv();
            frames = 0;
        };
        demod.BurstCompleted += bu =>
        {
            if (run.Reason != "None")
            {
                return;
            }

            run.Reason = bu.Reason.ToString();
            run.EndBlocks = bu.Blocks;
            run.PayloadBits = bu.PayloadBits.Length;
            run.Payload = bu.PayloadBits;
            run.EndAtT2 = (long)Get(demod, "_written")!;
        };
        const int chunk = 2400;
        for (int i = 0; i < audio.Length; i += chunk)
        {
            demod.Process(audio.AsSpan(i, Math.Min(chunk, audio.Length - i)));
        }

        return run;
    }

    static void Lines(StringBuilder sb, Run r)
    {
        sb.Append($"wn {r.Waveform} frames {r.TotalFrames} blocks {r.Blocks.Count}\n");
        for (int b = 0; b < r.Blocks.Count; b++)
        {
            Block k = r.Blocks[b];
            sb.Append($"block {b} {k.AtT2} {k.Outcome} mfb {k.MfbOffered} {k.MfbSelected} frames {k.Frames} fhash {k.FrameHash:x16}\n");
            sb.Append($"last {k.LastFrame}\n");
            sb.Append($"first {SoftHashed(k.First)}\n");
            sb.Append(k.Turbo is null ? "turbo -\n" : $"turbo {Soft(k.Turbo)}\n");
            sb.Append($"info {BitHash(k.Info)}\n");
        }

        sb.Append($"end {r.Reason} {r.EndBlocks} {r.PayloadBits} {r.EndAtT2} {BitHash(r.Payload)}\n");
    }

    static float[] ReadAudio(string path)
    {
        byte[] raw = File.ReadAllBytes(path);
        var a = new float[raw.Length / 4];
        Buffer.BlockCopy(raw, 0, a, 0, raw.Length);
        return a;
    }

    // ---------------------------------------------------------------- rx-turbo

    public static string Clean(string refDir)
    {
        var sb = new StringBuilder();
        sb.Append("# Dumped from M0LTE.Ms110d 0.2.1 by tools/csharp-vectors (rx-turbo mode): the shipped demodulator (turbo on) on every\n");
        sb.Append("# WN 1 to 8 and 13 reference case, fed in chunks of 2400 with 9600 zero samples after. Format in TurboDump.cs.\n");
        foreach (string name in File.ReadAllLines(Path.Combine(refDir, "index.txt"))
                     .Where(l => l.StartsWith("case=wn", StringComparison.Ordinal) && !l.StartsWith("case=wn0_", StringComparison.Ordinal))
                     .Select(l => l[5..]))
        {
            float[] audio = ReadAudio(Path.Combine(refDir, name, "13_audio.f32"));
            float[] padded = new float[audio.Length + 9600];
            audio.CopyTo(padded, 0);
            sb.Append($"case {name} ");
            Lines(sb, Demodulate(padded));
        }

        return sb.ToString();
    }

    // ---------------------------------------------------------------- rx-corpus

    static readonly int[] Waveforms = [1, 2, 3, 4, 5, 6, 7, 8, 13];

    // Table D-LXIV AWGN mask SNR per waveform (dB), as the C# probe pattern tests use.
    static double MaskSnr(int wn) => wn switch
    {
        1 => -3, 2 => 0, 3 => 3, 4 => 5, 5 => 6, 6 => 9, 7 => 13, 8 => 16, 13 => 6,
        _ => throw new ArgumentOutOfRangeException(nameof(wn)),
    };

    const double Scale = 8192;  // 16 bit audio: 1.0 = 8192

    public static void Corpus(string stem)
    {
        var text = new StringBuilder();
        text.Append("# Generated from M0LTE.Ms110d 0.2.1 by tools/csharp-vectors (rx-corpus mode); see TurboDump.cs. Each burst: its audio in the\n");
        text.Append("# .i16le file (1.0 = 8192, the C# ran on the same quantised audio), the payload sent, then the shipped receiver's results.\n");
        using var bin = new FileStream(stem + ".i16le", FileMode.Create);
        long offset = 0;

        void Burst(string label, int wn, Ms110dInterleaverKind kind, bool standard, string channel, double snr, int seed, int blocks)
        {
            Ms110dInterleaverParams il = Ms110dInterleaverParams.Get3k(wn, kind);
            var rng = new Random(seed);
            var payload = new byte[Math.Max(1, (blocks * il.InputBits) - 32 - 8)];
            for (int i = 0; i < payload.Length; i++)
            {
                payload[i] = (byte)rng.Next(2);
            }

            var tx = new Ms110dModulator(new Ms110dTxSettings { WaveformNumber = wn, Interleaver = kind });
            typeof(Ms110dModulator).GetProperty("StandardProbePattern", BindingFlags.NonPublic | BindingFlags.Instance)!
                .SetValue(tx, standard);
            float[] audio = tx.Modulate(payload);
            float[] rx = channel switch
            {
                "clean" => [.. new float[1500], .. audio, .. new float[9600]],
                "awgn" => new WattersonChannel(9600, seed + 1).Apply(audio, snr, leadInSamples: 1500, leadOutSamples: 9600),
                _ => new WattersonChannel(9600, seed + 1, WattersonChannel.Poor).Apply(audio, snr, leadInSamples: 1500, leadOutSamples: 9600),
            };
            var q = new short[rx.Length];
            var quantised = new float[rx.Length];
            for (int i = 0; i < rx.Length; i++)
            {
                q[i] = (short)Math.Clamp(Math.Round(rx[i] * Scale), short.MinValue, short.MaxValue);
                quantised[i] = (float)(q[i] / Scale);
            }

            var bytes = new byte[2 * q.Length];
            Buffer.BlockCopy(q, 0, bytes, 0, bytes.Length);
            bin.Write(bytes);
            Run r = Demodulate(quantised);
            text.Append($"burst {label} wn {wn} il {(int)kind} standard {(standard ? 1 : 0)} channel {channel} snr {R(snr)} seed {seed} ");
            text.Append($"offset {offset} samples {q.Length}\n");
            text.Append($"payload {Bits(payload)}\n");
            Lines(text, r);
            offset += bytes.Length;
        }

        // The Poor channel: every equaliser mode, short and ultra-short, two bursts each, at the
        // AWGN mask plus 8 and plus 12 dB (where fading leaves the turbo stage real work), and the
        // medium interleaver for the 256-symbol modes.
        foreach (int wn in Waveforms)
        {
            foreach (Ms110dInterleaverKind kind in new[] { Ms110dInterleaverKind.UltraShort, Ms110dInterleaverKind.Short })
            {
                for (int n = 0; n < 2; n++)
                {
                    Burst($"poor-wn{wn}-{(int)kind}-{n}", wn, kind, false, "poor", MaskSnr(wn) + (n == 0 ? 8 : 12), 20000 + (100 * wn) + (10 * (int)kind) + n, 2);
                }
            }

            if (wn >= 5)
            {
                Burst($"poor-wn{wn}-2", wn, Ms110dInterleaverKind.Medium, false, "poor", MaskSnr(wn) + 10, 21000 + wn, 1);
            }
        }

        // The standard probe pattern: every mode, ultra-short, short and medium, clean; short in
        // noise at the mask plus 3 dB. A current-pattern twin of each noisy one for comparison.
        foreach (int wn in Waveforms)
        {
            foreach (Ms110dInterleaverKind kind in new[] { Ms110dInterleaverKind.UltraShort, Ms110dInterleaverKind.Short, Ms110dInterleaverKind.Medium })
            {
                Burst($"std-wn{wn}-{(int)kind}", wn, kind, true, "clean", 0, 30000 + (10 * wn) + (int)kind, 2);
            }

            Burst($"std-awgn-wn{wn}", wn, Ms110dInterleaverKind.Short, true, "awgn", MaskSnr(wn) + 3, 31000 + wn, 2);
            Burst($"cur-awgn-wn{wn}", wn, Ms110dInterleaverKind.Short, false, "awgn", MaskSnr(wn) + 3, 31000 + wn, 2);
        }

        File.WriteAllText(stem + ".txt", text.ToString());
    }
}
