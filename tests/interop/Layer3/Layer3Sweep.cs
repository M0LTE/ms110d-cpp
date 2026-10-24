// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later

using M0LTE.Ms110d;

namespace Ms110dInterop.Tests.Layer3;

/// <summary>One cell of the matrix: a mode, a channel, a ladder and how many frames per point.</summary>
public sealed record Layer3Spec(int Wn, int Il, int K, Layer3Channel Channel, double[] SnrLadder, int Frames, int PreambleSuperframes = 3)
{
    public string Id => $"wn{Wn}-il{Il}-k{K}-{Channel.Name}";

    public string Label => $"WN{Wn} {(Ms110dInterleaverKind)Il} interleaver K{K} {Channel.Name}";

    /// <summary>Info bits in one interleaver block.</summary>
    public int BlockBits => Ms110dInterleaverParams.Get3k(Wn, (Ms110dInterleaverKind)Il).InputBits;

    /// <summary>A frame is a single burst of exactly one interleaver block, the EOM ending where the block ends.</summary>
    public int PayloadBits => TxCrossCheckTests.EdgeLengths(BlockBits)[0];

    public Ms110dTxSettings TxSettings => new()
    {
        WaveformNumber = Wn,
        Interleaver = (Ms110dInterleaverKind)Il,
        ConstraintLength = K,
        PreambleSuperframes = PreambleSuperframes,
    };

    /// <summary>Payload seed for frame f. Depends on the mode and frame only, never on the pairing or SNR.</summary>
    public int PayloadSeed(int f) => TxCrossCheckTests.Seed(10, Wn, Il, K, f);

    /// <summary>Channel seed for frame f: fixes the fading and the noise. The same for every pairing and every SNR.</summary>
    public int ChannelSeed(int f) => TxCrossCheckTests.Seed(11 + (Channel.Index * 7), Wn, Il, K, f);
}

/// <summary>Result of one (transmitter, receiver) pairing at one SNR.</summary>
public sealed class Layer3Point(string tx, string rx, double snrDb, bool[] lost, long bits, long bitErrors)
{
    public string Tx { get; } = tx;

    public string Rx { get; } = rx;

    public double SnrDb { get; } = snrDb;

    /// <summary>Per frame: true when the frame was lost (no burst, or the payload differs).</summary>
    public bool[] Lost { get; } = lost;

    public int Frames => Lost.Length;

    public int FrameErrors => Lost.Count(l => l);

    public double FrameLoss => (double)FrameErrors / Frames;

    public long Bits { get; } = bits;

    public long BitErrors { get; } = bitErrors;

    public string Pairing => $"{Tx}->{Rx}";
}

public sealed record Layer3SweepResult(Layer3Spec Spec, IReadOnlyList<Layer3Point> Points, IReadOnlyList<string> Skipped, double Seconds)
{
    public IEnumerable<string> Pairings => Points.Select(p => p.Pairing).Distinct();

    public IReadOnlyList<Layer3Point> Curve(string pairing) => [.. Points.Where(p => p.Pairing == pairing).OrderBy(p => p.SnrDb)];
}

public static class Layer3Sweep
{
    /// <summary>Threads used for frames. MS110D_LAYER3_THREADS, else all cores.</summary>
    public static int Threads =>
        int.TryParse(Environment.GetEnvironmentVariable("MS110D_LAYER3_THREADS"), out int t) && t > 0 ? t : Environment.ProcessorCount;

    /// <summary>
    /// Runs one cell. For each frame: one seeded payload; each transmitter modulates it once; for each SNR the
    /// channel with the frame's seed is applied to each transmitter's audio; each available receiver decodes it.
    /// The channel seed depends only on the frame, so every pairing sees the same fading and the same noise.
    /// </summary>
    public static Layer3SweepResult Run(Layer3Spec spec, IReadOnlyList<ILayer3Transmitter> txs, IReadOnlyList<ILayer3Receiver> rxs)
    {
        var sw = System.Diagnostics.Stopwatch.StartNew();
        var skipped = new List<string>();
        var liveRx = new List<ILayer3Receiver>();
        foreach (ILayer3Receiver rx in rxs)
        {
            if (rx.IsAvailable)
            {
                liveRx.Add(rx);
            }
            else
            {
                foreach (ILayer3Transmitter tx in txs)
                {
                    skipped.Add($"{tx.Name}->{rx.Name}: not yet available ({rx.UnavailableReason})");
                }
            }
        }

        int nSnr = spec.SnrLadder.Length;
        int nPair = txs.Count * liveRx.Count;
        var lost = new bool[nPair, nSnr][];
        var bitErrs = new long[nPair, nSnr];
        for (int p = 0; p < nPair; p++)
        {
            for (int s = 0; s < nSnr; s++)
            {
                lost[p, s] = new bool[spec.Frames];
            }
        }

        int payloadBits = spec.PayloadBits;
        Ms110dTxSettings settings = spec.TxSettings;
        Parallel.For(0, spec.Frames, new ParallelOptions { MaxDegreeOfParallelism = Threads }, f =>
        {
            byte[] payload = TxCrossCheckTests.RandomBits(new Random(spec.PayloadSeed(f)), payloadBits);
            float[][] audio = [.. txs.Select(t => t.Modulate(settings, payload))];
            int chSeed = spec.ChannelSeed(f);
            for (int s = 0; s < nSnr; s++)
            {
                for (int t = 0; t < txs.Count; t++)
                {
                    float[] noisy = spec.Channel.Create(chSeed).Apply(audio[t], spec.SnrLadder[s], leadInSamples: 2400, leadOutSamples: 4800);
                    for (int r = 0; r < liveRx.Count; r++)
                    {
                        int p = (t * liveRx.Count) + r;
                        byte[]? got = liveRx[r].Decode(noisy).Payload;
                        int errs = BitErrors(payload, got);
                        lost[p, s][f] = errs != 0 || got is null;
                        Interlocked.Add(ref bitErrs[p, s], errs);
                    }
                }
            }
        });

        var points = new List<Layer3Point>();
        for (int t = 0; t < txs.Count; t++)
        {
            for (int r = 0; r < liveRx.Count; r++)
            {
                int p = (t * liveRx.Count) + r;
                for (int s = 0; s < nSnr; s++)
                {
                    points.Add(new Layer3Point(txs[t].Name, liveRx[r].Name, spec.SnrLadder[s], lost[p, s], (long)payloadBits * spec.Frames, bitErrs[p, s]));
                }
            }
        }

        return new Layer3SweepResult(spec, points, skipped, sw.Elapsed.TotalSeconds);
    }

    /// <summary>Differing bits; a missing or shorter or longer decode counts every bit it lacks or adds.</summary>
    public static int BitErrors(byte[] sent, byte[]? got)
    {
        if (got is null)
        {
            return sent.Length;
        }

        int n = Math.Min(sent.Length, got.Length);
        int errs = Math.Abs(sent.Length - got.Length);
        for (int i = 0; i < n; i++)
        {
            if (sent[i] != got[i])
            {
                errs++;
            }
        }

        return errs;
    }

    /// <summary>The D-LXIV AWGN mask SNRs (3 kHz) by waveform number: where the search for the target point starts from.</summary>
    private static readonly Dictionary<int, double> AwgnMask = new()
    {
        [0] = -6, [1] = -3, [2] = 0, [3] = 3, [4] = 5, [5] = 6, [6] = 9, [7] = 13, [8] = 16, [13] = 6,
    };

    /// <summary>
    /// Finds a ladder for a cell: the reference pairing (first transmitter, first available receiver) is run on a
    /// handful of frames at rising SNR, two dB at a time from well below the AWGN mask, until it loses at most one
    /// of them. The ladder is then 0.5 dB steps from 2 dB below that point to 2.5 dB above. Deterministic: the same
    /// seeds give the same frames. Every pairing then runs the same ladder.
    /// </summary>
    public static double[] LocateLadder(int wn, int il, int k, Layer3Channel channel, ILayer3Transmitter tx, ILayer3Receiver rx)
    {
        const int probeFrames = 6;
        double snr = AwgnMask[wn] - 8;
        for (; snr < 45; snr += 2)
        {
            var probe = new Layer3Spec(wn, il, k, channel, [snr], probeFrames);
            if (Run(probe, [tx], [rx]).Points[0].FrameErrors <= 1)
            {
                break;
            }
        }

        return [.. Enumerable.Range(0, 10).Select(i => snr - 2 + (0.5 * i))];
    }

    /// <summary>
    /// Locates a ladder and runs the cell. If a pairing's curve does not bracket the target (the located
    /// point was a lucky six frames), the ladder is moved 3 dB the way the curve says and the cell is run again,
    /// at most twice. The shifts are returned so the report can say so.
    /// </summary>
    public static (Layer3SweepResult Result, int Shifts) RunLocated(int wn, int il, int k, Layer3Channel channel, int frames,
        IReadOnlyList<ILayer3Transmitter> txs, IReadOnlyList<ILayer3Receiver> rxs)
    {
        ILayer3Receiver firstRx = rxs.First(r => r.IsAvailable);
        double[] ladder = LocateLadder(wn, il, k, channel, txs[0], firstRx);
        int shifts = 0;
        while (true)
        {
            Layer3SweepResult r = Run(new Layer3Spec(wn, il, k, channel, ladder, frames), txs, rxs);
            var ts = Layer3Report.Thresholds(r);
            bool above = ts.Any(t => t.SnrDb is null && t.Note.StartsWith("still above", StringComparison.Ordinal));
            bool below = ts.Any(t => t.SnrDb is null && t.Note.StartsWith("already below", StringComparison.Ordinal));
            if ((!above && !below) || shifts == 2)
            {
                return (r, shifts);
            }

            shifts++;
            double d = above ? 3 : -3;
            ladder = [.. ladder.Select(x => x + d)];
        }
    }
}
