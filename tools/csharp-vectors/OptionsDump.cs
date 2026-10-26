// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later

// Dumps what the C# receiver does with each of its four instrument options switched on, for the C++
// receiver's options test (tests/receiver_test.cpp, docs/dev/receiver.md):
//   rx-options <ref-dir> <corpus-stem> <out-stem>
// Bursts: every fourth burst of the rx-corpus (its audio read from <corpus-stem>.i16le, made by the
// rx-corpus mode), and cut bursts that run into the signal-lost and signal-absent exits: a WN 2 and
// a WN 4 reference case cut half way, then 20 s of seeded noise (Wn0Dump.SharedNoise), with a
// steady tone on alternate trials. The cut bursts' audio is written to <out-stem>.f32le (32 bit
// float), so the C++ reads exactly what the C# was given, with two weak bursts (reference cases at a
// tenth of their level, so the input level gain engages); the results go to <out-stem>.txt.
// Variants: base (the defaults), rls (RlsForgettingFactor 0.98), ridge (TrackRidge 0.3), nseg-off
// and nseg-2s (TurboNsegMode "off" and "spike2s"), presence-off (PresenceReleaseOff).
// Each burst is fed in chunks of 2400 with 9600 zero samples after, through the public API only.
// Lines: "burst <label> source corpus|cut offset <n> samples <n>", then per variant
// "variant <name>", per block "block <index> <info n hash> <first-pass soft hash> <turbo soft hash or ->",
// "end <reason> <blocks> <payload bits>" (reason None when the burst did not end), and "stats" with the
// C#'s public counters after the run.

using System.Globalization;
using System.Text;
using M0LTE.Ms110d;

static class OptionsDump
{
    static readonly CultureInfo Inv = CultureInfo.InvariantCulture;

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

    static string BitHash(byte[] bits) => $"{bits.Length} {Fnv(bits.Select(b => (uint)b)):x16}";

    static string SoftHash(float[] v) => $"{Fnv(v.Select(BitConverter.SingleToUInt32Bits)):x16}";

    static readonly (string Name, Ms110dDemodOptions Options)[] Variants =
    [
        ("base", new Ms110dDemodOptions()),
        ("rls", new Ms110dDemodOptions { RlsForgettingFactor = 0.98f }),
        ("ridge", new Ms110dDemodOptions { TrackRidge = 0.3f }),
        ("nseg-off", new Ms110dDemodOptions { TurboNsegMode = "off" }),
        ("nseg-2s", new Ms110dDemodOptions { TurboNsegMode = "spike2s" }),
        ("presence-off", new Ms110dDemodOptions { PresenceReleaseOff = true }),
    ];

    static void Demodulate(float[] audio, Ms110dDemodOptions options, StringBuilder sb)
    {
        var demod = new Ms110dDemodulator(options);
        var first = new Dictionary<int, string>();
        var turbo = new Dictionary<int, string>();
        var blocks = new List<string>();
        string end = "end None 0 0";
        bool ended = false;
        demod.FirstPassBlockLlrs += (i, llrs) => { if (!ended) first[i] = SoftHash(llrs); };
        demod.TurboBlockLlrs += (i, llrs) => { if (!ended) turbo[i] = SoftHash(llrs); };
        demod.BlockDecoded += b =>
        {
            if (!ended)
            {
                blocks.Add($"block {b.Index} {BitHash(b.Bits)} {first[b.Index]} {(turbo.TryGetValue(b.Index, out string? t) ? t : "-")}");
            }
        };
        demod.BurstCompleted += bu =>
        {
            if (!ended)
            {
                end = $"end {bu.Reason} {bu.Blocks} {bu.PayloadBits.Length}";
                ended = true;
            }
        };
        var all = new float[audio.Length + 9600];
        audio.CopyTo(all, 0);
        const int chunk = 2400;
        for (int i = 0; i < all.Length; i += chunk)
        {
            demod.Process(all.AsSpan(i, Math.Min(chunk, all.Length - i)));
        }

        foreach (string b in blocks)
        {
            sb.Append(b).Append('\n');
        }

        sb.Append(end).Append('\n');
        sb.Append($"stats converged {demod.TurboConverged} reverted {demod.TurboReverted} salvaged {demod.TurboSalvaged} ");
        sb.Append($"aborted {demod.TurboAborted} skipped {demod.TurboSkipped} collapse {demod.CollapseResolves} ");
        sb.Append($"absent {demod.SignalAbsentReleases} agc {demod.AgcResolves} mfb_offered {demod.MfbOffered} mfb_selected {demod.MfbSelected}\n");
    }

    public static void Run(string refDir, string corpusStem, string outStem)
    {
        var sb = new StringBuilder();
        sb.Append("# Dumped from M0LTE.Ms110d 0.3.0 by tools/csharp-vectors (rx-options mode); format in OptionsDump.cs.\n");
        byte[] corpus = File.ReadAllBytes(corpusStem + ".i16le");
        int n = 0;
        foreach (string line in File.ReadLines(corpusStem + ".txt"))
        {
            if (!line.StartsWith("burst ", StringComparison.Ordinal))
            {
                continue;
            }

            if (n++ % 4 != 0)
            {
                continue;
            }

            string[] f = line.Split(' ');
            string label = f[1];
            long offset = long.Parse(f[Array.IndexOf(f, "offset") + 1], Inv);
            int samples = int.Parse(f[Array.IndexOf(f, "samples") + 1], Inv);
            var audio = new float[samples];
            for (int i = 0; i < samples; i++)
            {
                audio[i] = (float)(BitConverter.ToInt16(corpus, (int)(offset + (2 * i))) / 8192.0);
            }

            sb.Append($"burst {label} source corpus offset {offset} samples {samples}\n");
            foreach ((string name, Ms110dDemodOptions o) in Variants)
            {
                sb.Append($"variant {name}\n");
                Demodulate(audio, o, sb);
            }
        }

        using var bin = new BinaryWriter(File.Create(outStem + ".f32le"));
        long at = 0;
        foreach ((int wn, double snr) in new[] { (2, -3.0), (4, 3.0) })
        {
            string name = $"wn{wn}_short_k7_s2_p{(wn == 2 ? 448 : 1248)}";
            byte[] raw = File.ReadAllBytes(ReferencePaths.AudioPath(refDir, name));
            var baseAudio = new float[raw.Length / 4];
            Buffer.BlockCopy(raw, 0, baseAudio, 0, raw.Length);
            double power = baseAudio.Select(x => (double)x * x).Average();
            int cut = baseAudio.Length / 2;
            const int leadIn = 2400, leadOut = 20 * 9600;
            const double toneAmp = 0.1, toneHz = 2100.0;
            double sigma = Math.Sqrt(power / Math.Pow(10, snr / 10) * 4800.0 / 3000.0);
            for (int t = 0; t < 6; t++)
            {
                var noise = new Wn0Dump.SharedNoise((ulong)(70000 + (1000 * wn) + t));
                var audio = new float[leadIn + baseAudio.Length + leadOut];
                for (int i = 0; i < audio.Length; i++)
                {
                    int k = i - leadIn;
                    float b = k >= 0 && k < cut ? baseAudio[k] : 0f;
                    float tone = t % 2 == 1 && k >= cut ? (float)(toneAmp * Math.Cos(2.0 * Math.PI * toneHz * k / 9600.0)) : 0f;
                    audio[i] = b + tone + (float)(sigma * noise.Next());
                }

                foreach (float x in audio)
                {
                    bin.Write(x);
                }

                sb.Append($"burst wn{wn}cut{t} source cut offset {at} samples {audio.Length}\n");
                at += 4L * audio.Length;
                foreach ((string vname, Ms110dDemodOptions o) in Variants)
                {
                    if (vname is "base" or "presence-off")
                    {
                        sb.Append($"variant {vname}\n");
                        Demodulate(audio, o, sb);
                    }
                }
            }
        }

        // Weak bursts: a reference case at a tenth of its level, so the input level gain engages
        // (AgcResolves counts it). Written to the same audio file.
        foreach (string name in new[] { "wn3_short_k7_s1_p100", "wn6_short_k7_s1_p100" })
        {
            byte[] raw = File.ReadAllBytes(ReferencePaths.AudioPath(refDir, name));
            var audio = new float[2400 + (raw.Length / 4)];
            for (int i = 0; i < raw.Length / 4; i++)
            {
                audio[2400 + i] = 0.1f * BitConverter.ToSingle(raw, 4 * i);
            }

            foreach (float x in audio)
            {
                bin.Write(x);
            }

            sb.Append($"burst weak-{name} source cut offset {at} samples {audio.Length}\n");
            at += 4L * audio.Length;
            sb.Append("variant base\n");
            Demodulate(audio, Variants[0].Options, sb);
        }

        File.WriteAllText(outStem + ".txt", sb.ToString());
    }
}
