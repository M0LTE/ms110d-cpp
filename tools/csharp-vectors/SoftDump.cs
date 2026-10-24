// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later

// Dumps the soft decoders of the C# reference (M0LTE.Ms110d 0.2.1 and the M0LTE.Fec it pulls
// in) for tests/data/csharp-soft-0.2.1.txt. Run from the repo root:
//   dotnet run -c Release --project tools/csharp-vectors -- soft > tests/data/csharp-soft-0.2.1.txt
// TailBitingViterbiDecoder, TailBitingSisoDecoder and TailBitingEncoder are public. Ms110dBcjr
// and Ms110dChainBcjr are internal and take spans, which reflection cannot pass, so they are
// called through a DynamicMethod that skips visibility checks and converts arrays to spans.
//
// Inputs are not stored; both sides regenerate them from an integer generator and the dump
// carries an FNV-1a checksum of every input float's bits, so a generator that drifts fails
// loudly instead of comparing the wrong things. The generator is:
//   rng: xorshift32 (x ^= x << 13; x ^= x >> 17; x ^= x << 5), u16 = x >> 16.
//   bit = u16 & 1. gauss = (sum of 12 u16 draws - 393216) / 65536 (Irwin-Hall, mean 0, sd 1,
//   exact in double). Only + - * / on doubles after that, so the C# and C++ agree to the bit.
//   Mother LLRs: info bits from the rng, encoded, then for each coded bit i in order
//   y = (1 - 2 c) + sigma gauss, llr = (float)(2 y / (sigma sigma)).
// Output floats are the IEEE bits in hex (8 digits), decoded bits packed four per hex digit.

using System.Reflection;
using System.Reflection.Emit;
using System.Text;
using M0LTE.Fec;
using M0LTE.Ms110d;
using M0LTE.Ms110d.Fec;

internal static class SoftDump
{
    sealed class Rng
    {
        uint x;
        public Rng(uint seed) { x = seed; }
        uint Next() { x ^= x << 13; x ^= x >> 17; x ^= x << 5; return x; }
        public int U16() => (int)(Next() >> 16);
        public int Bit() => U16() & 1;
        public double Gauss()
        {
            int s = 0;
            for (int i = 0; i < 12; i++) s += U16();
            return (s - 393216) / 65536.0;
        }
    }

    sealed class Fnv
    {
        public ulong H = 0xcbf29ce484222325UL;
        public void Add(uint v) { for (int b = 0; b < 4; b++) { H ^= (v >> (8 * b)) & 0xFF; H *= 0x100000001b3UL; } }
        public void Add(float f) => Add(BitConverter.SingleToUInt32Bits(f));
        public void Add(byte v) { H ^= v; H *= 0x100000001b3UL; }
        public void Add(IEnumerable<float> fs) { foreach (var f in fs) Add(f); }
        public void Add(Complex32 c) { Add(c.Re); Add(c.Im); }
    }

    static string Hex(byte[] bits)
    {
        var sb = new StringBuilder();
        for (int i = 0; i < bits.Length; i += 4)
        {
            int v = 0;
            for (int j = 0; j < 4; j++) v = (v << 1) | (i + j < bits.Length ? bits[i + j] : 0);
            sb.Append("0123456789abcdef"[v]);
        }
        return sb.ToString();
    }

    static string Hex(IEnumerable<float> fs) =>
        string.Join(" ", fs.Select(f => BitConverter.SingleToUInt32Bits(f).ToString("x8")));

    static (byte[] info, byte[] coded, float[] llr) Block(ConvolutionalCode code, int n, double sigma, Rng rng)
    {
        var info = new byte[n];
        for (int i = 0; i < n; i++) info[i] = (byte)rng.Bit();
        var coded = new byte[2 * n];
        TailBitingEncoder.Encode(code, info, coded);
        var llr = new float[2 * n];
        for (int i = 0; i < 2 * n; i++)
        {
            double y = (1.0 - (2.0 * coded[i])) + (sigma * rng.Gauss());
            llr[i] = (float)((2.0 * y) / (sigma * sigma));
        }
        return (info, coded, llr);
    }

    static string D(double v) => v.ToString("R", System.Globalization.CultureInfo.InvariantCulture);

    public static string Run()
    {
        var sb = new StringBuilder();
        void Emit(string l) => sb.Append(l).Append('\n');
        Emit("# Soft decoder vectors from M0LTE.Ms110d 0.2.1 (tools/csharp-vectors, soft mode). Generator: see tools/csharp-vectors/SoftDump.cs.");
        var codes = new[] { ("K7", ConvolutionalCode.K7), ("K9", ConvolutionalCode.K9) };

        Emit("# DEC: code n variant sigma seed in-fnv viterbi-bits-hex siso-posteriors-hex-floats");
        uint seed = 0x4000;
        foreach (var (name, code) in codes)
        {
            var vit = new TailBitingViterbiDecoder(code);
            var siso = new TailBitingSisoDecoder(code);
            foreach (int n in new[] { 40, 100, 360 })
            {
                foreach (var (variant, sigma) in new[] { ("noisy", 1.0), ("noisy", 0.7), ("erased", 0.6), ("saturated", 0.1), ("zero", 0.5) })
                {
                    if (variant == "zero" && n != 40) continue;
                    seed++;
                    var (_, _, llr) = Block(code, n, sigma, new Rng(seed));
                    if (variant == "erased")
                        for (int i = 0; i < llr.Length; i++) if (i % 6 == 1 || i % 6 == 2) llr[i] = 0f;
                    if (variant == "zero") Array.Clear(llr);
                    var f = new Fnv(); f.Add(llr);
                    var bits = new byte[n];
                    vit.Decode(llr, bits);
                    var post = new float[2 * n];
                    siso.Decode(llr, post);
                    Emit($"DEC {name} {n} {variant} {D(sigma)} {seed} 0x{f.H:x16} {Hex(bits)} {Hex(post)}");
                }
            }
        }

        Emit("# BER: code n sigma seed blocks info-errors-viterbi coded-errors-siso-sign in-fnv viterbi-fnv siso-sign-fnv");
        foreach (var (name, code) in codes)
        {
            var vit = new TailBitingViterbiDecoder(code);
            var siso = new TailBitingSisoDecoder(code);
            int n = 120, blocks = 1000;
            foreach (double sigma in new[] { 1.0, 0.9, 0.8, 0.75, 0.7, 0.65, 0.6 })
            {
                seed++;
                var rng = new Rng(seed);
                int infoErr = 0, codedErr = 0;
                var fin = new Fnv(); var fv = new Fnv(); var fs = new Fnv();
                var bits = new byte[n]; var post = new float[2 * n];
                for (int b = 0; b < blocks; b++)
                {
                    var (info, coded, llr) = Block(code, n, sigma, rng);
                    fin.Add(llr);
                    vit.Decode(llr, bits);
                    siso.Decode(llr, post);
                    for (int i = 0; i < n; i++) { if (bits[i] != info[i]) infoErr++; fv.Add(bits[i]); }
                    for (int i = 0; i < 2 * n; i++)
                    {
                        byte hard = (byte)(post[i] > 0f ? 0 : 1);
                        if (hard != coded[i]) codedErr++;
                        fs.Add(hard);
                    }
                }
                Emit($"BER {name} {n} {D(sigma)} {seed} {blocks} {infoErr} {codedErr} 0x{fin.H:x16} 0x{fv.H:x16} 0x{fs.H:x16}");
            }
        }

        var bcjr = Bind("M0LTE.Ms110d.Ms110dBcjr");
        var chain = Bind("M0LTE.Ms110d.Ms110dChainBcjr");

        Emit("# BCJR: delay n sigma seed in-fnv llr-hex-floats");
        foreach (var (delay, n, sigma) in new[] { (1, 150, 0.5), (2, 150, 0.9), (3, 150, 0.5), (3, 150, 0.9), (4, 150, 0.7), (6, 80, 0.6), (3, 4, 0.5) })
        {
            seed++;
            var rng = new Rng(seed);
            var (rx, h1, h2) = Channel(rng, n, delay, sigma, null, null);
            var f = new Fnv();
            foreach (var c in rx) f.Add(c);
            foreach (var c in h1) f.Add(c);
            foreach (var c in h2) f.Add(c);
            var llrs = (float[])bcjr.Invoke(null, new object?[] { rx, h1, h2, delay, (float)(sigma * sigma) })!;
            Emit($"BCJR {delay} {n} {D(sigma)} {seed} 0x{f.H:x16} {Hex(llrs)}");
        }

        Emit("# CHAIN: m delay n flags sigma seed in-fnv llr-hex-floats   (flags: 1 priors, 2 per-symbol noise, 4 no preceding)");
        foreach (var (m, delay, n, flags, sigma) in new[] {
            (2, 1, 150, 0, 0.6), (2, 3, 151, 3, 0.7), (2, 2, 120, 4, 0.5), (4, 2, 120, 0, 0.5), (4, 5, 123, 1, 0.6),
            (4, 3, 100, 2, 0.4), (8, 4, 100, 1, 0.4), (8, 7, 100, 4, 0.5), (8, 2, 99, 3, 0.35), (2, 8, 5, 0, 0.5) })
        {
            seed++;
            var rng = new Rng(seed);
            var (cons, labels) = Constellation(m);
            int bps = m == 2 ? 1 : m == 4 ? 2 : 3;
            var xs = new int[n + delay];
            for (int i = 0; i < xs.Length; i++) xs[i] = rng.U16() % m;
            var (rx, h1, h2) = Channel(rng, n, delay, sigma, cons, xs);
            var f = new Fnv();
            foreach (var c in rx) f.Add(c);
            foreach (var c in h1) f.Add(c);
            foreach (var c in h2) f.Add(c);
            Complex32[] preceding = (flags & 4) != 0 ? Array.Empty<Complex32>() : Enumerable.Range(0, delay).Select(k => cons[xs[k]]).ToArray();
            float[]? priors = null;
            if ((flags & 1) != 0)
            {
                priors = new float[n * m];
                for (int i = 0; i < priors.Length; i++) priors[i] = (float)(-(rng.U16() % 2000) / 250.0);
                f.Add(priors);
            }
            float[]? nvps = null;
            if ((flags & 2) != 0)
            {
                nvps = new float[n];
                for (int t = 0; t < n; t++) nvps[t] = (float)(sigma * sigma * (0.5 + ((t * 5) % 9) / 8.0));
                f.Add(nvps);
            }
            var llrs = new float[n * bps];
            chain.Invoke(null, new object?[] { rx, h1, h2, delay, (float)(sigma * sigma), cons, labels, bps, preceding, llrs, priors, nvps });
            Emit($"CHAIN {m} {delay} {n} {flags} {D(sigma)} {seed} 0x{f.H:x16} {Hex(llrs)}");
        }
        return sb.ToString();
    }

    // BPSK/QPSK/8PSK constellations and Gray-ish labels; the C++ test has the same literals.
    static (Complex32[] cons, byte[]? labels) Constellation(int m)
    {
        const float c = 0.70710678f;
        if (m == 2) return (new[] { new Complex32(1f, 0f), new Complex32(-1f, 0f) }, null);
        if (m == 4) return (new[] { new Complex32(c, c), new Complex32(-c, c), new Complex32(-c, -c), new Complex32(c, -c) }, new byte[] { 0, 1, 3, 2 });
        return (new[] {
            new Complex32(1f, 0f), new Complex32(c, c), new Complex32(0f, 1f), new Complex32(-c, c),
            new Complex32(-1f, 0f), new Complex32(-c, -c), new Complex32(0f, -1f), new Complex32(c, -c) },
            new byte[] { 0, 1, 3, 2, 7, 6, 4, 5 });
    }

    // Two path channel with slowly varying gains. With `cons` null the symbols are BPSK from the rng
    // bits with the pre block symbols all +1 (state zero); otherwise xs holds n + delay symbol
    // indices, the first `delay` being the pre block symbols.
    static (Complex32[] rx, Complex32[] h1, Complex32[] h2) Channel(Rng rng, int n, int delay, double sigma, Complex32[]? cons, int[]? xs)
    {
        var rx = new Complex32[n]; var h1 = new Complex32[n]; var h2 = new Complex32[n];
        double[] xr = new double[n + delay], xi = new double[n + delay];
        for (int i = 0; i < n + delay; i++)
        {
            if (cons is null) { xr[i] = i < delay ? 1.0 : 1.0 - (2.0 * rng.Bit()); xi[i] = 0.0; }
            else { xr[i] = cons[xs![i]].Re; xi[i] = cons[xs[i]].Im; }
        }
        for (int t = 0; t < n; t++)
        {
            double u = ((t * 7) % 13) / 13.0;
            double a1r = 1.0 - (0.3 * u), a1i = 0.2 * u;
            double a2r = 0.5 - (0.2 * u), a2i = -0.3 + (0.4 * u);
            double x0r = xr[t + delay], x0i = xi[t + delay], x1r = xr[t], x1i = xi[t];
            double yr = (a1r * x0r) - (a1i * x0i) + ((a2r * x1r) - (a2i * x1i));
            double yi = (a1r * x0i) + (a1i * x0r) + ((a2r * x1i) + (a2i * x1r));
            yr += sigma * rng.Gauss();
            yi += sigma * rng.Gauss();
            rx[t] = new Complex32((float)yr, (float)yi);
            h1[t] = new Complex32((float)a1r, (float)a1i);
            h2[t] = new Complex32((float)a2r, (float)a2i);
        }
        return (rx, h1, h2);
    }

    // A callable for an internal static method whose span parameters become arrays.
    static DynamicMethod Bind(string typeName)
    {
        var type = typeof(Complex32).Assembly.GetType(typeName) ?? throw new InvalidOperationException(typeName);
        var method = type.GetMethod("Equalize", BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static)!;
        var ps = method.GetParameters();
        var argTypes = new Type[ps.Length];
        var il0 = new List<MethodInfo?>();
        for (int i = 0; i < ps.Length; i++)
        {
            var pt = ps[i].ParameterType;
            if (pt.IsGenericType && (pt.GetGenericTypeDefinition() == typeof(ReadOnlySpan<>) || pt.GetGenericTypeDefinition() == typeof(Span<>)))
            {
                var elem = pt.GetGenericArguments()[0];
                argTypes[i] = elem.MakeArrayType();
                il0.Add(pt.GetMethod("op_Implicit", new[] { argTypes[i] }));
            }
            else { argTypes[i] = pt; il0.Add(null); }
        }
        var dm = new DynamicMethod("call_" + type.Name, method.ReturnType, argTypes, typeof(SoftDump).Module, true);
        var il = dm.GetILGenerator();
        for (int i = 0; i < ps.Length; i++)
        {
            il.Emit(OpCodes.Ldarg, (short)i);
            if (il0[i] is { } conv) il.Emit(OpCodes.Call, conv);
        }
        il.Emit(OpCodes.Call, method);
        il.Emit(OpCodes.Ret);
        return dm;
    }
}
