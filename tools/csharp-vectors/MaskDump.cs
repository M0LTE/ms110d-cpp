// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later

// Makes the performance mask audio for the C++ stand-alone mask tests (docs/dev/masks.md), the way the C# repo's
// Ms110dMaskTests do: random payload bursts, modulated by the real M0LTE.Ms110d transmitter, through the
// M0LTE.HfChannel Watterson channel (Appendix E) at the SNR of Table D-LXIV (AWGN and Poor) or on the D-LXV
// three path static channel, then the C# receiver on that same audio for the C# error count.
//   masks list  <profile>                 the point ids, one per line
//   masks write <profile> <dir> [id ...]  <dir>/<id>.txt (manifest), <id>.i16le (audio), <id>.bits (payloads)
// Profiles: "ci" (the budget the pull request job runs) and "long" (the full counts, by manual dispatch).
// Everything is seeded, so the files are reproducible for a given thread count (the thread count does not
// change the numbers: a burst's seeds depend on its index only). Never committed; made by scripts/generate-masks.sh.
//
// Conditions as in the C# tests (D.6.1): Long interleaver, constraint length 7, 20 super-frame preamble, SNR in a
// 3 kHz noise bandwidth, bursts of about 90 s with the EOM, 2400 samples of channel noise before and after.

using System.Globalization;
using System.Text;
using M0LTE.Fec;
using M0LTE.HfChannel;
using M0LTE.Ms110d;
using M0LTE.Ms110d.Fec;

static class MaskDump
{
    static readonly CultureInfo Inv = CultureInfo.InvariantCulture;
    const double Scale = 4096;       // 16 bit audio: 1.0 = 4096, full scale +-8 (a clipped sample is a generator error)
    const int LeadIn = 2400;
    const int LeadOut = 2400;

    enum Kind { Awgn, Poor, Static }

    sealed record Point(string Id, Kind Kind, int Wn, double SnrDb, int SeedBase)
    {
        public WattersonPath[] Paths => Kind switch
        {
            Kind.Awgn => [],
            Kind.Poor => WattersonChannel.Poor,
            // Table D-LXV, 3 kHz, WID 2: 3-path (0.0, 3.0, 9.0 ms), equal power, no fading.
            _ => [new(0), new(3.0), new(9.0)],
        };
    }

    // Table D-LXIV 3 kHz rows, the waveforms the standard gives (WN 0 to 8 and 13; WN 9 to 12 are not in the
    // modem yet). The Static point is the C# repo's Static_Wid2_Gate rig at its house SNR (the D-LXV SNR column
    // is not transcribed: the standard says "not yet standardized"), see docs/dev/masks.md.
    static readonly (int Wn, double Awgn, double Poor)[] Masks =
    [
        (0, -6, -1), (1, -3, 3), (2, 0, 5), (3, 3, 7), (4, 5, 10), (5, 6, 11), (6, 9, 14), (7, 13, 19), (8, 16, 23), (13, 6, 11),
    ];

    static IEnumerable<Point> Points()
    {
        foreach (var (wn, awgn, poor) in Masks)
        {
            yield return new Point($"awgn-wn{wn}", Kind.Awgn, wn, awgn, 100 + wn);
        }

        foreach (var (wn, awgn, poor) in Masks)
        {
            yield return new Point($"poor-wn{wn}", Kind.Poor, wn, poor, 500 + wn);
        }

        yield return new Point("static-wn2", Kind.Static, 2, 9, 900);

        // Controls below the mask, where the C# makes errors: the comparison with the C# means little at zero
        // errors on both sides, so these show it holds when there are some. Never mask points.
        yield return new Point("below-awgn-wn2", Kind.Awgn, 2, -3, 1100);
        yield return new Point("below-awgn-wn6", Kind.Awgn, 6, 6, 1106);
        yield return new Point("below-poor-wn3", Kind.Poor, 3, 3, 1203);
        yield return new Point("below-poor-wn5", Kind.Poor, 5, 7, 1205);
    }

    // What a point is asked for: payload bits, the least simulated seconds, and the most (0 for no limit).
    // The C# tests use 3,000,000 payload bits per point (6,000,000 for Poor WN 2, 5 and 6, whose rates sit close to the
    // mask) and 600 s simulated for the fading points; the "long" profile is exactly that. The "ci" profile is sized for a pull request job: 370,000 bits, the least that
    // decides the 1e-5 mask with no errors (the 97.5 % Poisson bound of zero errors is 3.69 over the bits), and
    // at least 640 s simulated (8 bursts), but no more than 1300 s (14 bursts) because the slow waveforms
    // (WN 0 is 75 bit/s) would need hours of audio. So WN 3 and up decide the mask in the pull request job
    // and WN 0 to WN 2 (about 88, 172 and 344 kbit) compare with the C# only; the long profile decides them all.
    static (long Bits, double MinSimSeconds, double MaxSimSeconds) Budget(string profile, Point p) => profile switch
    {
        "long" => (p.Kind == Kind.Poor && p.Wn is 2 or 5 or 6 ? 6_000_000 : 3_000_000, p.Kind == Kind.Poor ? 600 : 0, 0),
        "ci" => (370_000, 640, 1300),
        _ => throw new ArgumentException($"unknown profile {profile}"),
    };

    sealed class BurstResult
    {
        public int Index;
        public int ChannelSeed;
        public short[] Q = [];
        public byte[] Payload = [];
        public int Decoded;
        public long Errors;
        public string End = "None";
        public int Clipped;
    }

    public static string List(string profile)
    {
        _ = Budget(profile, Points().First());
        return string.Concat(Points().Select(p => p.Id + "\n"));
    }

    public static void Write(string profile, string dir, string[] ids)
    {
        Directory.CreateDirectory(dir);
        foreach (Point p in Points().Where(p => ids.Length == 0 || ids.Contains(p.Id)))
        {
            WritePoint(profile, dir, p);
        }
    }

    static void WritePoint(string profile, string dir, Point p)
    {
        var settings = new Ms110dTxSettings
        {
            WaveformNumber = p.Wn,
            Interleaver = Ms110dInterleaverKind.Long,
            ConstraintLength = 7,
            PreambleSuperframes = 20,
        };
        var tx = new Ms110dModulator(settings);
        Ms110dInterleaverParams il = Ms110dInterleaverParams.Get3k(p.Wn, Ms110dInterleaverKind.Long);
        double blockSeconds = p.Wn == 0
            ? il.Frames * 32.0 / 2400
            : il.Frames * (tx.Mode.U + tx.Mode.K) / 2400.0;
        int blocksPerBurst = Math.Max(1, (int)(90 / blockSeconds));
        int payloadBits = (blocksPerBurst * il.InputBits) - 32;
        var (targetBits, minSim, maxSim) = Budget(profile, p);

        // A burst's length is fixed per point, so the burst count is too.
        double burstSeconds = tx.Modulate(new byte[payloadBits]).Length / 9600.0;
        int bursts = (int)Math.Max(Math.Ceiling((double)targetBits / payloadBits), Math.Ceiling(minSim / burstSeconds));
        if (maxSim > 0)
        {
            bursts = Math.Min(bursts, (int)Math.Max(1, Math.Floor(maxSim / burstSeconds)));
        }

        bursts = Math.Max(bursts, 1);
        int threads = int.TryParse(Environment.GetEnvironmentVariable("MS110D_MASK_THREADS"), out int t) ? Math.Max(1, t) : 2;

        var text = new StringBuilder();
        text.Append("# Generated from M0LTE.Ms110d 0.2.1 and M0LTE.HfChannel by tools/csharp-vectors (masks mode), see docs/dev/masks.md.\n");
        text.Append($"point {p.Id} profile {profile} channel {p.Kind.ToString().ToLowerInvariant()} wn {p.Wn} snr_db {R(p.SnrDb)} " +
                    $"interleaver long payload_bits {payloadBits} bursts {bursts} burst_seconds {R(burstSeconds)} scale {R(Scale)}\n");
        using var audioFile = new FileStream(Path.Combine(dir, p.Id + ".i16le"), FileMode.Create);
        using var bitsFile = new FileStream(Path.Combine(dir, p.Id + ".bits"), FileMode.Create);
        long audioOffset = 0, bitsOffset = 0, totalErrors = 0;
        int acquisitionFailures = 0;
        var bitsPacked = new byte[(payloadBits + 7) / 8];
        for (int first = 0; first < bursts; first += threads)
        {
            int count = Math.Min(threads, bursts - first);
            var batch = new BurstResult[count];
            Parallel.For(0, count, i => batch[i] = Burst(p, settings, il, payloadBits, first + i));
            foreach (BurstResult r in batch)
            {
                if (r.Clipped > 0)
                {
                    throw new InvalidOperationException($"{p.Id} burst {r.Index}: {r.Clipped} samples clip at scale {Scale}");
                }

                var bytes = new byte[r.Q.Length * 2];
                Buffer.BlockCopy(r.Q, 0, bytes, 0, bytes.Length);
                audioFile.Write(bytes);
                Array.Clear(bitsPacked);
                for (int i = 0; i < r.Payload.Length; i++)
                {
                    bitsPacked[i >> 3] |= (byte)(r.Payload[i] << (7 - (i & 7)));
                }

                bitsFile.Write(bitsPacked);
                text.Append($"burst {r.Index} seed {r.ChannelSeed} audio_offset {audioOffset} samples {r.Q.Length} bits_offset {bitsOffset} " +
                            $"cs_decoded {r.Decoded} cs_errors {r.Errors} cs_end {r.End}\n");
                audioOffset += bytes.Length;
                bitsOffset += bitsPacked.Length;
                totalErrors += r.Errors;
                acquisitionFailures += r.Decoded == 0 ? 1 : 0;
            }

            Console.Error.WriteLine($"  {p.Id}: {first + count}/{bursts} bursts, C# errors so far {totalErrors}");
        }

        text.Append($"total bits {(long)bursts * payloadBits} cs_errors {totalErrors} cs_acquisition_failures {acquisitionFailures}\n");
        File.WriteAllText(Path.Combine(dir, p.Id + ".txt"), text.ToString());
    }

    // One burst: the C# test's RunPointWorker body, with the per-burst payload seeded by its index so bursts
    // can be made in any order and on any number of threads.
    static BurstResult Burst(Point p, Ms110dTxSettings settings, Ms110dInterleaverParams il, int payloadBits, int index)
    {
        var tx = new Ms110dModulator(settings);
        var random = new Random(p.SeedBase + (1000 * index));
        var payload = new byte[payloadBits];
        for (int i = 0; i < payload.Length; i++)
        {
            payload[i] = (byte)random.Next(2);
        }

        float[] audio = tx.Modulate(payload);
        int channelSeed = p.SeedBase + (1000 * index) + 1;
        var channel = new WattersonChannel(9600, channelSeed, p.Paths);
        float[] rx = channel.Apply(audio, p.SnrDb, leadInSamples: LeadIn, leadOutSamples: LeadOut);
        var q = new short[rx.Length];
        var quantised = new float[rx.Length];
        int clipped = 0;
        for (int i = 0; i < rx.Length; i++)
        {
            double v = Math.Round(rx[i] * Scale);
            if (v > short.MaxValue || v < short.MinValue)
            {
                clipped++;
            }

            q[i] = (short)Math.Clamp(v, short.MinValue, short.MaxValue);
            quantised[i] = (float)(q[i] / Scale);
        }

        // The C# receiver on the quantised audio, in the chunks the C# test uses (all at once).
        var decoded = new List<byte>(payloadBits + 64);
        var demod = new Ms110dDemodulator(new Ms110dDemodOptions());
        string end = "None";
        demod.BurstCompleted += bu => end = bu.Reason.ToString();
        demod.BlockDecoded += b => decoded.AddRange(b.Bits);
        demod.Process(quantised);

        byte[] txBits = Ms110dFraming.BuildTxBits(payload, appendEom: true, il.InputBits);
        long errors = 0;
        if (decoded.Count == 0)
        {
            errors = payload.Length;
        }
        else
        {
            int compared = Math.Min(decoded.Count, payload.Length);
            for (int i = 0; i < compared; i++)
            {
                if (decoded[i] != payload[i])
                {
                    errors++;
                }
            }

            errors += payload.Length - compared;
            errors += Math.Max(0, decoded.Count - txBits.Length);
        }

        return new BurstResult
        {
            Index = index, ChannelSeed = channelSeed, Q = q, Payload = payload,
            Decoded = decoded.Count, Errors = errors, End = end, Clipped = clipped,
        };
    }

    static string R(double v) => v.ToString("R", Inv);
}
