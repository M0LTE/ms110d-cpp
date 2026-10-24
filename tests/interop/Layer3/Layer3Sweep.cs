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
    /// <summary>False when the reference curve never reached the target frame loss (a floor, or off the ladder).</summary>
    public bool Reached { get; init; } = true;

    /// <summary>Why the target was not reached, with the floor; empty when it was.</summary>
    public string Note { get; init; } = "";

    /// <summary>How many times the ladder was moved to bracket the target.</summary>
    public int Shifts { get; init; }

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
        try
        {
        Parallel.For(0, spec.Frames, new ParallelOptions { MaxDegreeOfParallelism = Threads }, f =>
        {
            byte[] payload = TxCrossCheckTests.RandomBits(new Random(spec.PayloadSeed(f)), payloadBits);
            float[][] audio = [.. txs.Select(t => t.Modulate(settings, payload))];
            if (audio.Any(a => a.Length != audio[0].Length))
            {
                // The channel draws its fading and noise for a length; with unequal lengths the pairings would no
                // longer see the same realisation and a difference could be luck, not the modems.
                throw new InvalidOperationException($"{spec.Label} frame {f}: transmitters produced different audio lengths ("
                    + string.Join(", ", txs.Select((t, i) => $"{t.Name} {audio[i].Length}")) + ")");
            }

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
        }
        catch (AggregateException ae)
        {
            // Report the real fault, not Parallel's wrapper.
            System.Runtime.ExceptionServices.ExceptionDispatchInfo.Capture(ae.Flatten().InnerExceptions[0]).Throw();
        }

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
    /// Finds where the reference pairing crosses the target frame loss, from a short pilot: 12 frames at rising SNR,
    /// 2 dB at a time from well below the AWGN mask, until the loss is at or below the target; the crossing is
    /// then interpolated (log loss) between that point and the one before. Null when the target is not reached by
    /// 40 dB. Deterministic: the same seeds give the same frames.
    /// </summary>
    public static double? PilotCrossing(int wn, int il, int k, Layer3Channel channel, ILayer3Transmitter tx, ILayer3Receiver rx)
    {
        const int pilotFrames = 12;
        double target = Layer3Report.TargetFor(channel);
        double floor = 0.5 / pilotFrames;
        double? prevSnr = null;
        double prevLoss = 1;
        for (double snr = AwgnMask[wn] - (channel.Fading ? 8 : 10); snr <= 40; snr += 2)
        {
            double loss = Run(new Layer3Spec(wn, il, k, channel, [snr], pilotFrames), [tx], [rx]).Points[0].FrameLoss;
            if (loss <= target)
            {
                if (prevSnr is not { } ps)
                {
                    return snr - 2;
                }

                double a = Math.Log(Math.Max(prevLoss, floor));
                double b = Math.Log(Math.Max(loss, floor));
                double t = a == b ? 1 : (a - Math.Log(target)) / (a - b);
                return ps + (t * (snr - ps));
            }

            prevSnr = snr;
            prevLoss = loss;
        }

        return null;
    }

    /// <summary>A ladder of seven points centred on the target crossing: 0.5 dB apart on AWGN, 1 dB on fading.</summary>
    public static double[] CentredLadder(double centre, Layer3Channel channel)
    {
        double step = channel.Fading ? 1.0 : 0.5;
        double c = Math.Round(centre * 2) / 2;
        return [.. Enumerable.Range(-3, 7).Select(i => c + (i * step))];
    }

    /// <summary>
    /// Pilots the crossing, centres a ladder on it and runs the cell. If the reference curve does not bracket the target
    /// (the pilot was off) the ladder is moved by its half span the way the curve says and the cell run again, at most
    /// three times. A target that the REFERENCE never reaches (a floor, for example a high rate mode on Poor) is
    /// reported, with its floor, not failed: <see cref="Layer3SweepResult.Reached"/> is false. A pairing that does
    /// not reach it when the reference does is not excused: the ladder is extended upward by 10 dB for every pairing
    /// and, if it still does not reach it, the cell comparison fails.
    /// </summary>
    public static Layer3SweepResult RunLocated(int wn, int il, int k, Layer3Channel channel, int frames,
        IReadOnlyList<ILayer3Transmitter> txs, IReadOnlyList<ILayer3Receiver> rxs)
    {
        ILayer3Receiver firstRx = rxs.First(r => r.IsAvailable);
        double? centre = PilotCrossing(wn, il, k, channel, txs[0], firstRx);
        if (centre is null)
        {
            Layer3SweepResult top = Run(new Layer3Spec(wn, il, k, channel, [40], frames), txs, rxs);
            string floors = string.Join(", ", top.Pairings.Select(p => $"{p} {top.Curve(p)[0].FrameLoss * 100:F1}%"));
            return top with { Reached = false, Note = $"target {Layer3Report.TargetFor(channel) * 100:F0}% not reached by 40 dB; frame loss at 40 dB: {floors}" };
        }

        double[] ladder = CentredLadder(centre.Value, channel);
        double half = channel.Fading ? 3 : 1.5;
        int shifts = 0;
        Layer3SweepResult r;
        while (true)
        {
            r = Run(new Layer3Spec(wn, il, k, channel, ladder, frames), txs, rxs);
            // Only the reference decides whether the target is reachable: a pairing that is worse is a finding, not a reason to move.
            Layer3Threshold rt = Layer3Report.Thresholds(r)[0];
            if (rt.SnrDb is not null)
            {
                break;
            }

            bool above = rt.Note.StartsWith("still above", StringComparison.Ordinal);
            if (shifts == 3)
            {
                string floor = above
                    ? "reference target not reached on the ladder (floor): " + $"{r.Pairings.First()} lowest loss {r.Curve(r.Pairings.First()).Min(x => x.FrameLoss) * 100:F1}%"
                    : "target already met at the lowest SNR of the ladder";
                return r with { Reached = false, Shifts = shifts, Note = floor };
            }

            shifts++;
            double d = above ? half : -half;
            ladder = [.. ladder.Select(x => x + d)];
        }

        // A pairing that does not reach the target on the ladder may just be several dB worse (or dead). Look further
        // up before concluding anything: extra points above the ladder, run for every pairing so they stay paired.
        bool needsMore = Layer3Report.Thresholds(r).Skip(1).Any(t => t.SnrDb is null && t.Note.StartsWith("still above", StringComparison.Ordinal));
        if (needsMore)
        {
            double top = ladder.Max();
            double[] extra = [.. Enumerable.Range(1, ExtensionPoints).Select(i => top + (ExtensionStepDb * i)).Where(x => x <= 40)];
            if (extra.Length > 0)
            {
                Layer3SweepResult more = Run(new Layer3Spec(wn, il, k, channel, extra, frames), txs, rxs);
                r = new Layer3SweepResult(
                    r.Spec with { SnrLadder = [.. ladder, .. extra] },
                    [.. r.Points, .. more.Points], r.Skipped, r.Seconds + more.Seconds) { Shifts = shifts };
            }
        }

        return r with { Shifts = shifts };
    }

    /// <summary>How far above the ladder a worse pairing is looked for: 5 points, 2 dB apart (10 dB).</summary>
    public const int ExtensionPoints = 5;

    public const double ExtensionStepDb = 2.0;
}
