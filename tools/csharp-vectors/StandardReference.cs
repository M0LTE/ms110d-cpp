// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later

// Standard-pattern equivalents of the reference files' probe values and audio. For every case of
// the RefGen set (which holds the C# 0.2.1 default, selected-interleaver, pattern) this runs the C#
// modulator with the same settings and payload in its internal standard-probe mode and writes
//   <out>/<case>/10_probes.cf32   the probe values, in the layout of the reference file of that name
//   <out>/<case>/13_audio.f32     the audio, in the layout of the reference file of that name
// Everything else in a case (the symbols apart from the probes, the bits) is the same in both modes,
// and the tests take it from the reference files. Run (scripts/generate-standard-reference.sh does):
//   dotnet run -c Release --project tools/csharp-vectors -- standard-reference build/_reference/ref build/_reference/standard

using System.Reflection;
using M0LTE.Dsp;
using M0LTE.Ms110d;

static class StandardReference
{
    // BuildSymbols takes a ReadOnlySpan, which reflection cannot pass; a delegate type can.
    delegate Complex32[] BuildSymbolsDelegate(ReadOnlySpan<byte> payloadBits);

    public static void Run(string refDir, string outDir)
    {
        string[] cases = File.ReadAllLines(Path.Combine(refDir, "index.txt"))
            .Where(l => l.StartsWith("case=")).Select(l => l[5..].Trim()).ToArray();
        if (cases.Length == 0) throw new InvalidOperationException("no cases in " + refDir);

        MethodInfo build = typeof(Ms110dModulator).GetMethod("BuildSymbols", BindingFlags.NonPublic | BindingFlags.Instance)!;
        foreach (string name in cases)
        {
            string dir = Path.Combine(refDir, name);
            var m = File.ReadAllLines(Path.Combine(dir, "manifest.txt"))
                .Select(l => l.Split('=', 2)).Where(p => p.Length == 2).ToDictionary(p => p[0], p => p[1]);
            var settings = new Ms110dTxSettings
            {
                WaveformNumber = int.Parse(m["wn"]),
                Interleaver = Enum.Parse<Ms110dInterleaverKind>(m["interleaver"], ignoreCase: true),
                ConstraintLength = int.Parse(m["constraint_length"]),
                PreambleSuperframes = int.Parse(m["preamble_superframes"]),
                TlcBlocks = int.Parse(m["tlc_blocks"]),
                AppendEom = m["append_eom"] == "1",
                AppendEot = m["append_eot"] == "1",
                Amplitude = float.Parse(m["amplitude"], System.Globalization.CultureInfo.InvariantCulture),
                RollOff = double.Parse(m["roll_off"], System.Globalization.CultureInfo.InvariantCulture),
            };
            byte[] payload = File.ReadAllBytes(Path.Combine(dir, m["file.payload_bits"]));
            Ms110dModulator tx = StandardTx.Create(settings);
            var buildSymbols = (BuildSymbolsDelegate)Delegate.CreateDelegate(typeof(BuildSymbolsDelegate), tx, build);
            Complex32[] symbols = buildSymbols(payload);
            float[] audio = tx.Modulate(payload);

            // The probe symbols are where the reference stream has the marker 255.
            byte[] marks = File.ReadAllBytes(Path.Combine(dir, m["file.burst_symbols"]));
            if (marks.Length != symbols.Length)
            {
                throw new InvalidOperationException($"{name}: symbol count {symbols.Length} differs from the reference {marks.Length}");
            }

            string o = Path.Combine(outDir, name);
            Directory.CreateDirectory(o);
            using (var w = new BinaryWriter(File.Create(Path.Combine(o, "10_probes.cf32"))))
            {
                for (int i = 0; i < marks.Length; i++)
                {
                    if (marks[i] == 255)
                    {
                        w.Write(symbols[i].Re);
                        w.Write(symbols[i].Im);
                    }
                }
            }

            using (var w = new BinaryWriter(File.Create(Path.Combine(o, "13_audio.f32"))))
            {
                foreach (float s in audio) w.Write(s);
            }
        }
    }
}
