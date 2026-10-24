// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later

// Dumps encoder and puncturing outputs from the C# reference as text for
// tests/fec_vectors.inc. Run from the repo root:
//   dotnet run -c Release --project tools/csharp-vectors -- fec > /tmp/fec.txt
//   python3 tools/csharp-vectors/make_fec_inc.py /tmp/fec.txt > tests/fec_vectors.inc
// Public types only: TailBitingEncoder.Encode, Ms110dPuncture.Get and Apply.

using System.Text;
using M0LTE.Fec;
using M0LTE.Ms110d.Fec;

internal static class FecDump
{
    // Payload bits: xorshift32 seeded with `seed`, bit = (x >> 16) & 1 after each step.
    static byte[] Bits(uint seed, int n)
    {
        var b = new byte[n];
        uint x = seed;
        for (int i = 0; i < n; i++)
        {
            x ^= x << 13; x ^= x >> 17; x ^= x << 5;
            b[i] = (byte)((x >> 16) & 1);
        }
        return b;
    }
    static ulong Fnv(byte[] b)
    {
        ulong h = 0xcbf29ce484222325UL;
        foreach (var v in b) { h ^= v; h *= 0x100000001b3UL; }
        return h;
    }
    static string Hex(byte[] bits)
    {
        var sb = new System.Text.StringBuilder();
        for (int i = 0; i < bits.Length; i += 4)
        {
            int v = 0;
            for (int j = 0; j < 4; j++) v = (v << 1) | (i + j < bits.Length ? bits[i + j] : 0);
            sb.Append("0123456789abcdef"[v]);
        }
        return sb.ToString();
    }

    public static string Run()
    {
        var sb = new StringBuilder();
        void Emit(string l) => sb.Append(l).Append('\n');
        string[] rates = { "9/10", "8/9", "5/6", "4/5", "3/4", "2/3", "4/7", "9/16", "1/2", "2/5", "1/3", "2/7", "1/4", "1/6", "1/8", "1/12", "1/16" };
        var codes = new[] { ("K7", ConvolutionalCode.K7), ("K9", ConvolutionalCode.K9) };

        Emit("// ENCODE: code seed n fnv64(coded bits as 0/1 bytes)");
        foreach (var (name, code) in codes)
            foreach (int n in new[] { 9, 16, 33, 100, 360, 1000 })
            {
                uint seed = (uint)(0x1000 + n);
                var info = Bits(seed, n);
                var coded = new byte[2 * n];
                TailBitingEncoder.Encode(code, info, coded);
                Emit($"{name} {seed} {n} 0x{Fnv(coded):x16}");
            }

        Emit("// PUNCTURE: code rate seed n outlen fnv64(punctured)");
        foreach (var (name, code) in codes)
            foreach (var rate in rates)
                foreach (int n in new[] { 360, 720 })
                {
                    uint seed = (uint)(0x2000 + n);
                    var info = Bits(seed, n);
                    var coded = new byte[2 * n];
                    TailBitingEncoder.Encode(code, info, coded);
                    var spec = Ms110dPuncture.Get(code, rate);
                    var outb = new byte[spec.OutputLength(n)];
                    int w = Ms110dPuncture.Apply(spec, coded, outb);
                    if (w != outb.Length) throw new Exception("len");
                    Emit($"{name} {rate} {seed} {n} {w} 0x{Fnv(outb):x16}");
                }

        Emit("// FULL: code rate seed n info-hex coded-hex punctured-hex");
        foreach (var (name, code) in codes)
            foreach (var rate in new[] { "3/4", "2/3", "1/3", "1/2", "5/6" })
            {
                int n = 30; uint seed = 0x3000;
                var info = Bits(seed, n);
                var coded = new byte[2 * n];
                TailBitingEncoder.Encode(code, info, coded);
                var spec = Ms110dPuncture.Get(code, rate);
                var outb = new byte[spec.OutputLength(n)];
                Ms110dPuncture.Apply(spec, coded, outb);
                Emit($"{name} {rate} {seed} {n} {Hex(info)} {Hex(coded)} {Hex(outb)}");
            }
        return sb.ToString();
    }
}
