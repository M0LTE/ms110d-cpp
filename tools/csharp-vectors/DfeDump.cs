// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later

// Drives the C# Dfe (public in the 0.4.1 package) through scripted receive scenarios and dumps
// its outputs as text, for tests/data/csharp-dfe-0.4.1.txt. Run from the repo root:
//   dotnet run -c Release --project tools/csharp-vectors -- dfe [reference-dir] > tests/data/csharp-dfe-0.4.1.txt
// The reference directory (default build/_reference/ref, made by scripts/generate-reference.sh)
// supplies the real burst symbol streams of the "real" scenarios; the synthetic ones need nothing.
//
// The inputs are NOT in the dump: the C++ test regenerates them bit for bit. The generator uses
// only integer arithmetic and IEEE add, multiply and square root in double (no libm), so both
// languages make identical samples; each scenario records a hash of its samples, and the C++
// refuses to compare if its own differ. The generator and the script below are mirrored in
// tests/support/eq_scenario.cpp; keep them in step. docs/dev/equaliser.md describes both.
//
// Floats are written as the hex of their bits (8 digits for float, 16 for double) so that the C++
// can compare exactly or with a tolerance, whichever the check wants.

using System.Globalization;
using System.Text;
using M0LTE.Ms110d;

static class DfeDump
{
    // ------------------------------------------------------------------ the generator

    sealed class Rng(ulong seed)
    {
        private ulong _s = seed;

        public ulong Next()
        {
            _s += 0x9E3779B97F4A7C15UL;
            ulong z = _s;
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9UL;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBUL;
            return z ^ (z >> 31);
        }

        public double Uniform() => (Next() >> 11) * (1.0 / 9007199254740992.0);

        // Sum of twelve uniforms less six: unit variance, built from exact additions only.
        public double Gaussian()
        {
            double a = 0.0;
            for (int i = 0; i < 12; i++)
            {
                a += Uniform();
            }

            return a - 6.0;
        }
    }

    // Channel impulse responses at T/2 spacing (re, im pairs).
    static readonly double[][] Channels =
    [
        // 0: multipath, about 3 symbols of ISI
        [0.15, 0.05, 1.0, 0.0, 0.5, 0.3, -0.1, 0.25, 0.2, -0.1, 0.05, 0.05],
        // 1: the same plus a strong echo 5 symbols later (T/2 index 10)
        [0.15, 0.05, 1.0, 0.0, 0.5, 0.3, -0.1, 0.25, 0.2, -0.1, 0.05, 0.05, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.45, -0.2],
        // 2: no distortion at all (a single tap at T/2 index 2, the cursor)
        [0.0, 0.0, 0.0, 0.0, 1.0, 0.0],
        // 3: a weak cursor and a strong echo 8 symbols later, beyond the feed-forward window (the
        // case channel shortening is for): T/2 taps 2 and 3 are weak, tap 18 is strong
        [0.0, 0.0, 0.0, 0.0, 0.15, 0.0, 0.1, 0.05, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
         0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 1.0, -0.3],
        // 4: no cursor at all, only the echo 8 symbols later (the cursor gain of the shortening
        // solve collapses)
        [0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
         0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 1.0, -0.3],
    ];

    // Rotation per T/2 sample as a literal (no trig): about 0.002 rad.
    const double StepRe = 0.999998;
    const double StepIm = 0.002;

    sealed class Scenario
    {
        public string Name = "";
        public string Source = "synthetic"; // "synthetic" or a reference case directory name
        public int K, U, Frames, Ff, Fb, Lead;
        public float InitRidge, TrackRidge, Lambda, Mu = 0.05f;
        public double NoiseVar;
        public ulong Seed;
        public int Chan;
        public string Pass = "static"; // static (3 pass), fading (trajectory), nlms
        public bool Extras;            // snapshot/restore and soft-label rows
        public bool Quiet;             // only the symbol error count
        public int TrainLen = 192;
    }

    sealed class Burst
    {
        public Complex32[] Tx = [];   // train + probe0, then frames of U data + K probe
        public Complex32[] Rotor = [];
        public sbyte[] Bit = [];      // +1 / -1 per data symbol
        public int TrainLen;
        public int Start;             // index of the first frame
    }

    static Burst MakeBurst(Scenario s, string refDir)
    {
        var b = new Burst { TrainLen = s.TrainLen };
        int total = s.TrainLen + s.K + (s.Frames * (s.U + s.K));
        b.Tx = new Complex32[total];
        b.Rotor = new Complex32[s.Frames * s.U];
        b.Bit = new sbyte[s.Frames * s.U];
        b.Start = s.TrainLen + s.K;
        if (s.Source == "synthetic")
        {
            var rng = new Rng(s.Seed);
            for (int i = 0; i < s.TrainLen; i++)
            {
                b.Tx[i] = Ms110dTables.Psk8[(int)(rng.Next() & 7)];
            }

            Complex32[] p0 = MiniProbe.Get(s.K, false);
            for (int i = 0; i < s.K; i++)
            {
                b.Tx[s.TrainLen + i] = p0[i];
            }

            for (int f = 0; f < s.Frames; f++)
            {
                int bas = b.Start + (f * (s.U + s.K));
                for (int u = 0; u < s.U; u++)
                {
                    ulong v = rng.Next();
                    int idx = (int)(v & 7);
                    sbyte bit = ((v >> 33) & 1) != 0 ? (sbyte)-1 : (sbyte)1;
                    Complex32 rot = Ms110dTables.Psk8[idx];
                    b.Rotor[(f * s.U) + u] = rot;
                    b.Bit[(f * s.U) + u] = bit;
                    b.Tx[bas + u] = rot * (float)bit;
                }

                Complex32[] pr = MiniProbe.Get(s.K, f % 4 == 3);
                for (int i = 0; i < s.K; i++)
                {
                    b.Tx[bas + s.U + i] = pr[i];
                }
            }
        }
        else
        {
            string dir = Path.Combine(refDir, s.Source);
            byte[] sym = File.ReadAllBytes(Path.Combine(dir, "09_symbols.sym"));
            byte[] probes = File.ReadAllBytes(Path.Combine(dir, "10_probes.cf32"));
            byte[] scr = File.ReadAllBytes(Path.Combine(dir, "07_scramble.sym"));
            // The preamble is 1728 symbols, then the initial probe, then frames. Known training
            // is the last TrainLen preamble symbols plus that probe.
            int pre = 1728;
            int probeAt = 0;
            Complex32 Probe()
            {
                int o = probeAt++ * 8;
                return new Complex32(BitConverter.ToSingle(probes, o), BitConverter.ToSingle(probes, o + 4));
            }

            int n = 0;
            for (int i = pre - s.TrainLen; i < pre; i++)
            {
                b.Tx[n++] = Ms110dTables.Psk8[sym[i]];
            }

            int p = pre;
            for (int i = 0; i < s.K; i++)
            {
                if (sym[p++] != 255)
                {
                    throw new InvalidOperationException("expected the initial probe");
                }

                b.Tx[n++] = Probe();
            }

            int du = 0;
            for (int f = 0; f < s.Frames; f++)
            {
                for (int u = 0; u < s.U; u++)
                {
                    int idx = sym[p++];
                    int rotIdx = scr[du];
                    int diff = (idx - rotIdx + 8) & 7;
                    if (diff != 0 && diff != 4)
                    {
                        throw new InvalidOperationException("data symbol is not BPSK over the scrambler");
                    }

                    b.Rotor[(f * s.U) + u] = Ms110dTables.Psk8[rotIdx];
                    b.Bit[(f * s.U) + u] = diff == 0 ? (sbyte)1 : (sbyte)-1;
                    b.Tx[n++] = Ms110dTables.Psk8[idx];
                    du++;
                }

                for (int i = 0; i < s.K; i++)
                {
                    if (sym[p++] != 255)
                    {
                        throw new InvalidOperationException("expected a probe");
                    }

                    b.Tx[n++] = Probe();
                }
            }
        }

        return b;
    }

    // T/2 samples through the channel, the rotation and the noise, in double, then to float.
    static Complex32[] MakeSamples(Scenario s, Burst b)
    {
        double[] c = Channels[s.Chan];
        int taps = c.Length / 2;
        int count = (2 * b.Tx.Length) + 64;
        var x = new Complex32[count];
        var noise = new Rng(s.Seed + 1);
        double sd = Math.Sqrt(s.NoiseVar / 2.0);
        double rr = 1.0, ri = 0.0;
        for (int m = 0; m < count; m++)
        {
            double ar = 0.0, ai = 0.0;
            for (int k = 0; k < taps; k++)
            {
                int q = m - k;
                if (q < 0 || (q & 1) != 0 || (q >> 1) >= b.Tx.Length)
                {
                    continue;
                }

                Complex32 t = b.Tx[q >> 1];
                double sr = t.Re, si = t.Im;
                double cr = c[2 * k], ci = c[(2 * k) + 1];
                ar += (cr * sr) - (ci * si);
                ai += (cr * si) + (ci * sr);
            }

            double yr = (ar * rr) - (ai * ri);
            double yi = (ar * ri) + (ai * rr);
            if (s.NoiseVar > 0)
            {
                yr += noise.Gaussian() * sd;
                yi += noise.Gaussian() * sd;
            }

            x[m] = new Complex32((float)yr, (float)yi);
            double nr = (rr * StepRe) - (ri * StepIm);
            double ni = (rr * StepIm) + (ri * StepRe);
            rr = nr;
            ri = ni;
        }

        return x;
    }

    // ------------------------------------------------------------------ formatting

    static string F(float v) => BitConverter.SingleToUInt32Bits(v).ToString("x8", CultureInfo.InvariantCulture);
    static string D(double v) => BitConverter.DoubleToUInt64Bits(v).ToString("x16", CultureInfo.InvariantCulture);
    static string C(Complex32 v) => F(v.Re) + " " + F(v.Im);

    static ulong Fnv(IEnumerable<Complex32> v)
    {
        ulong h = 14695981039346656037UL;
        foreach (Complex32 c in v)
        {
            foreach (uint w in new[] { BitConverter.SingleToUInt32Bits(c.Re), BitConverter.SingleToUInt32Bits(c.Im) })
            {
                for (int i = 0; i < 4; i++)
                {
                    h ^= (w >> (8 * i)) & 0xFF;
                    h *= 1099511628211UL;
                }
            }
        }

        return h;
    }

    // ------------------------------------------------------------------ the script

    static void Window(Complex32[] x, int symbol, int lead, Span<Complex32> window)
    {
        for (int i = 0; i < window.Length; i++)
        {
            int m = (2 * symbol) + lead - i;
            window[i] = m >= 0 && m < x.Length ? x[m] : Complex32.Zero;
        }
    }

    static void Push(Complex32[] decisions, Complex32 d)
    {
        for (int j = decisions.Length - 1; j > 0; j--)
        {
            decisions[j] = decisions[j - 1];
        }

        if (decisions.Length > 0)
        {
            decisions[0] = d;
        }
    }

    static Complex32 Slice(Complex32 v) => new(v.Re >= 0 ? 1f : -1f, 0f);

    const float DdGate = 0.5f;

    static void Run(Scenario s, string refDir, StringBuilder sb)
    {
        Burst b = MakeBurst(s, refDir);
        Complex32[] x = MakeSamples(s, b);
        sb.Append("scenario name=").Append(s.Name).Append(" source=").Append(s.Source)
            .Append(" k=").Append(s.K).Append(" u=").Append(s.U).Append(" frames=").Append(s.Frames)
            .Append(" ff=").Append(s.Ff).Append(" fb=").Append(s.Fb).Append(" lead=").Append(s.Lead)
            .Append(" initridge=").Append(F(s.InitRidge)).Append(" trackridge=").Append(F(s.TrackRidge))
            .Append(" lambda=").Append(F(s.Lambda)).Append(" mu=").Append(F(s.Mu))
            .Append(" noisevar=").Append(D(s.NoiseVar)).Append(" seed=").Append(s.Seed)
            .Append(" chan=").Append(s.Chan).Append(" pass=").Append(s.Pass)
            .Append(" extras=").Append(s.Extras ? 1 : 0).Append(" quiet=").Append(s.Quiet ? 1 : 0)
            .Append(" trainlen=").Append(s.TrainLen)
            .Append(" txhash=").Append(Fnv(b.Tx).ToString("x16", CultureInfo.InvariantCulture))
            .Append(" xhash=").Append(Fnv(x).ToString("x16", CultureInfo.InvariantCulture)).Append('\n');

        var dfe = new Dfe(s.Ff, s.Fb);
        int ff = s.Ff, fb = s.Fb, k = s.K, u = s.U;
        var window = new Complex32[ff];
        var past = new Complex32[fb];

        // Initial solve over the known training and the first probe.
        int knownLen = s.TrainLen + k;
        dfe.BeginTraining();
        for (int n = 64; n < knownLen; n++)
        {
            Window(x, n, s.Lead, window);
            for (int j = 0; j < fb; j++)
            {
                past[j] = b.Tx[n - 1 - j];
            }

            dfe.AddTrainingRow(window, past, b.Tx[n]);
        }

        bool rls = s.Pass != "nlms";
        if (rls)
        {
            dfe.BeginRls(s.Lambda, pInit: 1.0f);
            dfe.SeedRlsFromTraining(s.InitRidge, pFallback: 1.0f, ffNoisePower: 0f);
        }

        bool ok0 = dfe.SolveTraining(regularization: s.InitRidge, ffNoisePower: 0f);
        dfe.BeginTraining();
        var decisions = new Complex32[fb];
        for (int j = 0; j < fb; j++)
        {
            decisions[j] = b.Tx[knownLen - 1 - j];
        }

        double mse0 = 0;
        for (int i = 0; i < k; i++)
        {
            int n = s.TrainLen + i;
            Window(x, n, s.Lead, window);
            for (int j = 0; j < fb; j++)
            {
                past[j] = b.Tx[n - 1 - j];
            }

            Complex32 y = dfe.Equalize(window, past);
            Complex32 err = y - b.Tx[n];
            mse0 += err.MagnitudeSquared();
        }

        sb.Append("init solved=").Append(ok0 ? 1 : 0).Append(" mse=").Append(D(mse0 / k))
            .Append(" ffe=").Append(F(dfe.FfEnergy)).Append('\n');

        long errors = 0, counted = 0;
        float rlsWeight = s.Pass == "fading" ? 1.0f : 0.1f;
        int nTaps = ff + fb;
        var tapsPrev = new Complex32[nTaps];
        var tapsCur = new Complex32[nTaps];
        var probe = new Complex32[k];
        var yOut = new Complex32[u];
        var pass1 = new Complex32[u];
        var pass2 = new Complex32[u];
        var frameStartDecisions = new Complex32[fb];

        for (int f = 0; f < s.Frames; f++)
        {
            int bas = b.Start + (f * (u + k));
            int probeAt = bas + u;
            for (int i = 0; i < k; i++)
            {
                probe[i] = b.Tx[probeAt + i];
            }

            Complex32[] startTaps = dfe.SnapshotTaps();
            var probePhase = Complex32.Zero;
            double mse = 0;
            int rows = 0;
            for (int i = fb; i < k; i++)
            {
                Window(x, probeAt + i, s.Lead, window);
                for (int j = 0; j < fb; j++)
                {
                    past[j] = probe[i - 1 - j];
                }

                Complex32 y = dfe.Equalize(window, past);
                probePhase += y * probe[i].Conj();
                mse += (y - probe[i]).MagnitudeSquared();
                rows++;
                dfe.AddTrainingRow(window, past, probe[i], weight: 6f);
            }

            if (s.Extras && f == 1)
            {
                // Nested re-training that must not disturb the rows gathered for this solve.
                dfe.SnapshotTraining();
                Complex32[] keep = dfe.SnapshotTaps();
                dfe.BeginTraining();
                for (int i = fb; i < k; i++)
                {
                    Window(x, probeAt + i, s.Lead, window);
                    for (int j = 0; j < fb; j++)
                    {
                        past[j] = probe[i - 1 - j];
                    }

                    dfe.AddTrainingRow(window, past, probe[i], weight: 1f);
                }

                bool nested = dfe.SolveTraining(regularization: 0.01f, anchorToCurrentTaps: true);
                sb.Append("nested solved=").Append(nested ? 1 : 0).Append(" ffe=").Append(F(dfe.FfEnergy)).Append('\n');
                dfe.LoadTaps(keep);
                dfe.RestoreTraining();
            }

            if (rls)
            {
                dfe.SeedRlsFromTraining(s.TrackRidge, pFallback: 1.0f, ffNoisePower: 0f);
            }

            bool solved = dfe.SolveTraining(regularization: s.TrackRidge, anchorToCurrentTaps: true, ffNoisePower: 0f);

            var postPhase = Complex32.Zero;
            for (int i = fb; i < k; i++)
            {
                Window(x, probeAt + i, s.Lead, window);
                for (int j = 0; j < fb; j++)
                {
                    past[j] = probe[i - 1 - j];
                }

                postPhase += dfe.Equalize(window, past) * probe[i].Conj();
            }

            float ppAbs = postPhase.Abs();
            if (ppAbs / rows >= 0.10f)
            {
                // Rotor from the phasor itself (the demodulator takes the angle; this is the same
                // rotation without trig, so no libm sits between the two languages).
                dfe.RotateTaps(new Complex32(postPhase.Re / ppAbs, -postPhase.Im / ppAbs));
            }

            Complex32[] endTaps = dfe.SnapshotTaps();
            if (!s.Quiet)
            {
                sb.Append("probe f=").Append(f).Append(" solved=").Append(solved ? 1 : 0)
                    .Append(" corr=").Append(C(probePhase)).Append(" err=").Append(D(mse)).Append(" rows=").Append(rows)
                    .Append(" mse=").Append(D(mse / rows))
                    .Append(" post=").Append(C(postPhase)).Append(" ffe=").Append(F(dfe.FfEnergy)).Append('\n');
            }

            float rw = rlsWeight;
            decisions.CopyTo(frameStartDecisions, 0);
            dfe.BeginTraining();
            if (s.Pass == "fading")
            {
                for (int uu = 0; uu < u; uu++)
                {
                    float a = (uu + (k / 2f)) / (u + k);
                    float inv = 1 - a;
                    for (int i = 0; i < nTaps; i++)
                    {
                        tapsCur[i] = (startTaps[i] * inv) + (endTaps[i] * a);
                    }

                    if (uu == 0)
                    {
                        dfe.LoadTaps(tapsCur);
                    }
                    else
                    {
                        dfe.TranslateTaps(tapsPrev, tapsCur);
                    }

                    tapsCur.CopyTo(tapsPrev, 0);
                    Complex32 rotor = b.Rotor[(f * u) + uu];
                    Window(x, bas + uu, s.Lead, window);
                    Complex32 y = dfe.Equalize(window, decisions);
                    Complex32 descr = y * rotor.Conj();
                    Complex32 clean = Slice(descr);
                    yOut[uu] = descr;
                    if ((descr - clean).MagnitudeSquared() < DdGate)
                    {
                        dfe.RlsUpdate(window, decisions, clean * rotor, weight: rw);
                        dfe.AddTrainingRow(window, decisions, clean * rotor, weight: 0.25f);
                    }

                    Push(decisions, clean * rotor);
                }
            }
            else if (s.Pass == "nlms")
            {
                for (int uu = 0; uu < u; uu++)
                {
                    Complex32 rotor = b.Rotor[(f * u) + uu];
                    Window(x, bas + uu, s.Lead, window);
                    Complex32 y = dfe.Equalize(window, decisions);
                    Complex32 descr = y * rotor.Conj();
                    Complex32 clean = Slice(descr);
                    yOut[uu] = descr;
                    dfe.Nlms(window, decisions, clean * rotor, s.Mu);
                    if ((descr - clean).MagnitudeSquared() < DdGate)
                    {
                        dfe.AddTrainingRow(window, decisions, clean * rotor, weight: 0.25f);
                    }

                    Push(decisions, clean * rotor);
                }
            }
            else
            {
                for (int uu = 0; uu < u; uu++)
                {
                    Complex32 rotor = b.Rotor[(f * u) + uu];
                    Window(x, bas + uu, s.Lead, window);
                    Complex32 y = dfe.Equalize(window, decisions);
                    Complex32 descr = y * rotor.Conj();
                    Complex32 clean = Slice(descr);
                    pass1[uu] = descr;
                    dfe.RlsUpdate(window, decisions, clean * rotor, weight: rw);
                    if ((descr - clean).MagnitudeSquared() < DdGate)
                    {
                        dfe.AddTrainingRow(window, decisions, clean * rotor, weight: 0.25f);
                    }

                    Push(decisions, clean * rotor);
                }

                dfe.LoadTaps(startTaps);
                frameStartDecisions.CopyTo(decisions, 0);
                for (int uu = 0; uu < u; uu++)
                {
                    Complex32 rotor = b.Rotor[(f * u) + uu];
                    Window(x, bas + uu, s.Lead, window);
                    Complex32 y = dfe.Equalize(window, decisions);
                    Complex32 descr = y * rotor.Conj();
                    Complex32 clean = Slice(descr);
                    pass2[uu] = descr;
                    dfe.RlsUpdate(window, decisions, clean * rotor, weight: rw);
                    Push(decisions, clean * rotor);
                }

                var mid = new Complex32[nTaps];
                for (int i = 0; i < nTaps; i++)
                {
                    mid[i] = (startTaps[i] + endTaps[i]) * 0.5f;
                }

                dfe.LoadTaps(mid);
                frameStartDecisions.CopyTo(decisions, 0);
                for (int uu = 0; uu < u; uu++)
                {
                    Complex32 rotor = b.Rotor[(f * u) + uu];
                    Window(x, bas + uu, s.Lead, window);
                    Complex32 y = dfe.Equalize(window, decisions);
                    Complex32 descr = y * rotor.Conj();
                    Complex32 clean = Slice(descr);
                    yOut[uu] = (pass1[uu] + pass2[uu] + descr) * (1f / 3f);
                    dfe.RlsUpdate(window, decisions, clean * rotor, weight: rw);
                    Push(decisions, clean * rotor);
                }
            }

            if (s.Extras && f == 2)
            {
                // Soft-label rows (expectations with a variance) into the accumulation for the next solve.
                var pv = new float[fb];
                for (int j = 0; j < fb; j++)
                {
                    pv[j] = 0.05f;
                }

                for (int uu = 8; uu < 16 && uu < u; uu++)
                {
                    Window(x, bas + uu, s.Lead, window);
                    for (int j = 0; j < fb; j++)
                    {
                        past[j] = b.Tx[bas + uu - 1 - j] * 0.9f;
                    }

                    dfe.AddTrainingRow(window, past, pv, b.Tx[bas + uu], weight: 0.5f);
                }
            }

            dfe.SymmetrizeP(pMax: 10f);
            dfe.LoadTaps(endTaps);
            for (int i = 0; i < k; i++)
            {
                Push(decisions, probe[i]);
            }

            var line = new StringBuilder();
            var dline = new StringBuilder();
            for (int uu = 0; uu < u; uu++)
            {
                Complex32 yy = yOut[uu];
                if (!s.Quiet)
                {
                    line.Append(' ').Append(C(yy));
                    dline.Append(yy.Re >= 0 ? '+' : '-');
                }

                sbyte truth = b.Bit[(f * u) + uu];
                if (f >= 2)
                {
                    counted++;
                    if ((yy.Re >= 0 ? 1 : -1) != truth)
                    {
                        errors++;
                    }
                }
            }

            if (!s.Quiet)
            {
                sb.Append("y f=").Append(f).Append(line).Append('\n');
                sb.Append("d f=").Append(f).Append(' ').Append(dline).Append('\n');
            }
        }

        if (!s.Quiet)
        {
            sb.Append("taps");
            foreach (Complex32 t in dfe.SnapshotTaps())
            {
                sb.Append(' ').Append(C(t));
            }

            sb.Append('\n');
        }

        sb.Append("result errors=").Append(errors).Append(" counted=").Append(counted).Append('\n');
    }

    // ------------------------------------------------------------------ target impulse response

    static void RunTir(string name, int chan, double noiseVar, bool allowPair, int secondOnlyLag, StringBuilder sb)
    {
        var s = new Scenario
        {
            Name = name, K = 32, U = 64, Frames = 6, Ff = 24, Fb = 12, Lead = 13, TrackRidge = 0.15f,
            NoiseVar = noiseVar, Seed = 77, Chan = chan, Pass = "static", TrainLen = 192,
        };
        Burst b = MakeBurst(s, "");
        Complex32[] x = MakeSamples(s, b);
        sb.Append("tir name=").Append(name).Append(" chan=").Append(chan).Append(" noisevar=").Append(D(noiseVar))
            .Append(" pair=").Append(allowPair ? 1 : 0).Append(" onlylag=").Append(secondOnlyLag)
            .Append(" xhash=").Append(Fnv(x).ToString("x16", CultureInfo.InvariantCulture)).Append('\n');
        var dfe = new Dfe(s.Ff, s.Fb);
        var window = new Complex32[s.Ff];
        var hist = new Complex32[s.Fb];
        for (int pass = 0; pass < (secondOnlyLag > 0 ? 2 : 1); pass++)
        {
            dfe.BeginTraining();
            // Rows over the whole span of four frames (the turbo pass re-trains on the decided
            // data as well as the probes), with the symbol history in the feedback columns.
            for (int f = 0; f < 4; f++)
            {
                int frameAt = b.Start + (f * (s.U + s.K));
                for (int i = s.Fb; i < s.U + s.K; i++)
                {
                    Window(x, frameAt + i, s.Lead, window);
                    for (int j = 0; j < s.Fb; j++)
                    {
                        hist[j] = b.Tx[frameAt + i - 1 - j];
                    }

                    dfe.AddTrainingRow(window, hist, b.Tx[frameAt + i], weight: 1.0f);
                }
            }

            int only = pass == 1 ? secondOnlyLag : 0;
            Dfe.TirSolve t = dfe.SolveTrainingTir(1e-3f, 0f, Math.Min(s.Fb, s.U / 2), allowPair, only);
            sb.Append("tirsolve pass=").Append(pass).Append(" solved=").Append(t.Solved ? 1 : 0).Append(" lag=").Append(t.Lag)
                .Append(" c1=").Append(C(t.Coefficient)).Append(" lag2=").Append(t.Lag2).Append(" c2=").Append(C(t.Coefficient2))
                .Append(" ssenull=").Append(F(t.SseNull)).Append(" ssetir=").Append(F(t.SseTir))
                .Append(" ffe=").Append(F(dfe.FfEnergy)).Append('\n');
        }

        // The frame after the trained probes, equalised feedback-free as the turbo pass does.
        var zero = new Complex32[s.Fb];
        int bas = b.Start + (4 * (s.U + s.K));
        sb.Append("tiry");
        for (int uu = 0; uu < s.U; uu++)
        {
            Window(x, bas + uu, s.Lead, window);
            sb.Append(' ').Append(C(dfe.Equalize(window, zero)));
        }

        sb.Append('\n');
    }

    // ------------------------------------------------------------------ paths the scenarios skip

    static void Toks(StringBuilder sb, string label, IEnumerable<Complex32> v)
    {
        sb.Append("misc ").Append(label);
        foreach (Complex32 c in v)
        {
            sb.Append(' ').Append(C(c));
        }

        sb.Append('\n');
    }

    static void MiscRows(Dfe dfe, Burst b, Complex32[] x, int lead, int ff, int fb, int from, int to, float w)
    {
        var window = new Complex32[ff];
        var past = new Complex32[fb];
        for (int n = from; n < to; n++)
        {
            Window(x, n, lead, window);
            for (int j = 0; j < fb; j++)
            {
                past[j] = b.Tx[n - 1 - j];
            }

            dfe.AddTrainingRow(window, past, b.Tx[n], w);
        }
    }

    static void MiscUpdates(Dfe dfe, Burst b, Complex32[] x, int lead, int ff, int fb, int from, int to)
    {
        var window = new Complex32[ff];
        var past = new Complex32[fb];
        for (int n = from; n < to; n++)
        {
            Window(x, n, lead, window);
            for (int j = 0; j < fb; j++)
            {
                past[j] = b.Tx[n - 1 - j];
            }

            dfe.RlsUpdate(window, past, b.Tx[n], 1f);
        }
    }

    // Genie noise power in the solves, interpolated taps, the cap on P, and the training target
    // energy after a snapshot and restore (seen through the shortening solve's residuals).
    static void RunMisc(StringBuilder sb)
    {
        var s = new Scenario
        {
            Name = "misc", K = 32, U = 64, Frames = 4, Ff = 12, Fb = 4, Lead = 7, NoiseVar = 0.05, Seed = 91, Chan = 0, TrainLen = 192,
        };
        Burst b = MakeBurst(s, "");
        Complex32[] x = MakeSamples(s, b);
        sb.Append("misc-header xhash=").Append(Fnv(x).ToString("x16", CultureInfo.InvariantCulture)).Append('\n');
        int ff = s.Ff, fb = s.Fb, lead = s.Lead;

        // Noise power (the genie term) in the seed and the solve.
        var dfe = new Dfe(ff, fb);
        dfe.BeginTraining();
        MiscRows(dfe, b, x, lead, ff, fb, 64, 224, 1f);
        dfe.BeginRls(0.98f, 50f);
        dfe.SeedRlsFromTraining(1e-3f, 1f, 0.05f);
        bool ok = dfe.SolveTraining(1e-3f, false, 0.05f);
        Toks(sb, "noisesolve-ok", [new Complex32(ok ? 1f : 0f, 0f)]);
        Complex32[] a = dfe.SnapshotTaps();
        Toks(sb, "noisesolve", a);
        MiscUpdates(dfe, b, x, lead, ff, fb, 230, 270);
        Complex32[] c2 = dfe.SnapshotTaps();
        Toks(sb, "rls", c2);

        // Interpolation between the two snapshots.
        dfe.LoadInterpolatedTaps(a, c2, 0.3f);
        Toks(sb, "interp", dfe.SnapshotTaps());

        // The cap on P: a rank-deficient seed makes P large, so SymmetrizeP(10) bites.
        for (int variant = 0; variant < 2; variant++)
        {
            var d = new Dfe(ff, fb);
            d.BeginTraining();
            MiscRows(d, b, x, lead, ff, fb, 100, 116, 1f);
            d.BeginRls(0.98f, 1f);
            d.SeedRlsFromTraining(1e-6f, 1f, 0f);
            MiscUpdates(d, b, x, lead, ff, fb, 130, 140);
            if (variant == 1)
            {
                d.SymmetrizeP(10f);
            }

            MiscUpdates(d, b, x, lead, ff, fb, 140, 150);
            Toks(sb, variant == 0 ? "p-uncapped" : "p-capped", d.SnapshotTaps());
        }

        // Target energy after a restore: rows, snapshot, nested rows, restore, more rows, shortening solve.
        var t = new Dfe(ff, fb);
        t.BeginTraining();
        MiscRows(t, b, x, lead, ff, fb, 64, 128, 1f);
        t.SnapshotTraining();
        t.BeginTraining();
        MiscRows(t, b, x, lead, ff, fb, 128, 140, 3f);
        t.RestoreTraining();
        MiscRows(t, b, x, lead, ff, fb, 200, 230, 1f);
        Dfe.TirSolve r = t.SolveTrainingTir(1e-3f, 0.05f, 4, true, 0);
        Toks(sb, "tir-after-restore",
        [
            new Complex32(r.Solved ? 1f : 0f, r.Lag), r.Coefficient, new Complex32(r.Lag2, 0f), r.Coefficient2,
            new Complex32(r.SseNull, r.SseTir), new Complex32(t.FfEnergy, 0f),
        ]);
    }

    // ------------------------------------------------------------------ the list

    public static string Run(string refDir)
    {
        var sb = new StringBuilder();
        sb.Append("# Dumped from M0LTE.Ms110d 0.4.1 by tools/csharp-vectors (dfe mode): the public Dfe driven through scripted receive scenarios. Floats are hex bits.\n");

        Scenario S(string name, int k, int u, int frames, int ff, int fb, int lead, float ir, float tr, float lambda,
                   double nv, ulong seed, int chan, string pass, bool extras = false) => new()
        {
            Name = name, K = k, U = u, Frames = frames, Ff = ff, Fb = fb, Lead = lead, InitRidge = ir, TrackRidge = tr,
            Lambda = lambda, NoiseVar = nv, Seed = seed, Chan = chan, Pass = pass, Extras = extras,
        };

        var list = new List<Scenario>
        {
            // K = 48 class: 32 FF / 22 FB.
            S("k48-clean-static", 48, 48, 10, 32, 22, 16, 1.0f, 8.0f, 0.952f, 1e-4, 11, 0, "static", extras: true),
            S("k48-moderate-fading", 48, 48, 10, 32, 22, 16, 1.0f, 8.0f, 0.952f, 0.05, 12, 0, "fading", extras: true),
            S("k48-noisy-static", 48, 48, 10, 32, 22, 16, 1.0f, 8.0f, 0.952f, 0.5, 13, 0, "static"),
            // K = 32 class: 24 FF / 12 FB.
            S("k32-clean-fading", 32, 96, 8, 24, 12, 13, 1e-3f, 0.15f, 0.976f, 1e-4, 21, 0, "fading", extras: true),
            S("k32-moderate-static", 32, 96, 8, 24, 12, 13, 1e-3f, 0.15f, 0.976f, 0.05, 22, 0, "static"),
            S("k32-moderate-nlms", 32, 96, 8, 24, 12, 13, 1e-3f, 0.15f, 0.976f, 0.05, 23, 0, "nlms"),
            S("k32-noisy-fading", 32, 96, 8, 24, 12, 13, 1e-3f, 0.15f, 0.976f, 0.5, 24, 0, "fading"),
            S("k32-echo-static", 32, 96, 8, 24, 12, 13, 1e-3f, 0.15f, 0.976f, 0.02, 25, 1, "static"),
            S("k32-long-fading", 32, 256, 4, 24, 12, 13, 1e-3f, 0.15f, 0.991f, 0.05, 26, 0, "fading"),
            // K = 24 class: 16 FF / 6 FB.
            S("k24-moderate-static", 24, 48, 8, 16, 6, 8, 1e-3f, 0.15f, 0.952f, 0.05, 31, 0, "static"),
            S("k24-nodist-fading", 24, 48, 8, 16, 6, 8, 1e-3f, 0.15f, 0.952f, 0.02, 32, 2, "fading"),
        };

        // Real burst streams: the C# reference files' symbols, through the same channel.
        list.Add(new Scenario { Name = "real-wn1-moderate", Source = "wn1_short_k7_s1_p100", K = 48, U = 48, Frames = 10, Ff = 32, Fb = 22, Lead = 16, InitRidge = 1.0f, TrackRidge = 8.0f, Lambda = 0.952f, NoiseVar = 0.05, Seed = 41, Chan = 0, Pass = "static" });
        list.Add(new Scenario { Name = "real-wn3-moderate", Source = "wn3_long_k9_s1_p100", K = 32, U = 96, Frames = 8, Ff = 24, Fb = 12, Lead = 13, InitRidge = 1e-3f, TrackRidge = 0.15f, Lambda = 0.976f, NoiseVar = 0.05, Seed = 42, Chan = 0, Pass = "fading" });
        list.Add(new Scenario { Name = "real-wn5-moderate", Source = "wn5_short_k7_s1_p100", K = 32, U = 256, Frames = 4, Ff = 24, Fb = 12, Lead = 13, InitRidge = 1e-3f, TrackRidge = 0.15f, Lambda = 0.991f, NoiseVar = 0.05, Seed = 43, Chan = 0, Pass = "static" });

        foreach (Scenario s in list)
        {
            Run(s, refDir, sb);
        }

        // Target impulse response: an echo-free channel, a channel with the echo (pair allowed, pair
        // suppressed, constrained to a lag).
        RunTir("tir-null", 0, 0.02, true, 0, sb);
        RunTir("tir-echo-pair", 3, 0.1, true, 0, sb);
        RunTir("tir-echo-single", 3, 0.1, false, 8, sb);
        // Acceptance threshold: the same echo buried in noise is rejected; a constrained lag that is
        // not the echo's is rejected; with no cursor the gain of the shortening solve collapses.
        RunTir("tir-echo-buried", 3, 6.0, true, 0, sb);
        RunTir("tir-echo-wronglag", 3, 0.1, true, 5, sb);
        RunTir("tir-echo-only", 4, 0.1, true, 8, sb);
        RunMisc(sb);

        // Symbol error curve on the multipath channel: K = 32 class, fading pass, many frames.
        double[] ladder = [0.02, 0.05, 0.1, 0.2, 0.35, 0.5, 0.8];
        foreach (double nv in ladder)
        {
            var s = S("ser", 32, 96, 202, 24, 12, 13, 1e-3f, 0.15f, 0.976f, nv, 1000, 0, "fading");
            s.Quiet = true;
            Run(s, refDir, sb);
        }

        return sb.ToString();
    }
}
