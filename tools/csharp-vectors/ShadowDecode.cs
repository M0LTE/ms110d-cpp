// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later

// The C# twin of tools/ms110d-decode, for the shadow receiver (docs/dev/shadow.md).
//   shadow decode <wav>       the pinned C# receiver over a 9600 Hz mono WAV (16-bit PCM or 32-bit float), as
//                             one JSON object per line in the same event format as the C++ tool, finished
//                             (see below). Feed it the file `ms110d-decode --write-resampled` wrote, so both
//                             decoders hear the very same samples.
//   shadow finish <jsonl>     the second pass for the C++ tool's output: IL2P deframing and the payload hash,
//                             which only exist in C#. Reads the raw events, writes the finished ones.
// Raw events carry each block's bits (bits_hex). Finishing runs the IL2P+CRC deframer of the pinned M0LTE.Il2p
// over the blocks of each burst, as the corpus tools do (CorpusDecode.cs), and adds to burst_completed: bytes and
// sha256 (the payload bits cut to payload_bits, packed MSB first, zero padded), good_frames, frames_sha256 (each
// good frame as a 4-byte little-endian length then its bytes, in order), corrected, crc_failures, rs_failures and
// error_free. Each block gets good_frames, the frames that completed in it. bits_hex is dropped.
// The audio is fed in chunks of 96 samples, then three seconds of silence, then a terminate if a burst is open,
// as CorpusDecode.Run does. cpu_ms is the processor time of this thread spent in Process() while a burst was open.

using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using M0LTE.Il2p;
using M0LTE.Ms110d;

public static class ShadowDecode
{
    [StructLayout(LayoutKind.Sequential)]
    struct Timespec { public long Sec; public long Nsec; }

    [DllImport("libc", EntryPoint = "clock_gettime")]
    static extern int ClockGettime(int clock, out Timespec ts);

    // CLOCK_THREAD_CPUTIME_ID is 3 on Linux. Falls back to the stopwatch where that is not available.
    static readonly bool HaveCpuClock = OperatingSystem.IsLinux();
    static double CpuMs()
    {
        if (HaveCpuClock && ClockGettime(3, out var ts) == 0) return ts.Sec * 1000.0 + ts.Nsec / 1e6;
        return Stopwatch.GetTimestamp() * 1000.0 / Stopwatch.Frequency;
    }

    // The slot's quiet floor and a burst's SNR over it, the same estimate as tools/ms110d-decode (and scripts/corpus/curate.py):
    // the receivers expose no SNR of their own, so the shadow report measures one from the audio the same way for both.
    static double NoiseFloorPower(float[] x)
    {
        const int n = 4800;
        int k = x.Length / n;
        if (k == 0) return 1e-18;
        var p = new double[k];
        for (int w = 0; w < k; w++)
        {
            double acc = 0;
            for (int i = 0; i < n; i++) acc += (double)x[w * n + i] * x[w * n + i];
            p[w] = acc / n;
        }
        Array.Sort(p);
        int m = Math.Max(1, k / 5);
        double med = (m % 2 == 1) ? p[m / 2] : 0.5 * (p[m / 2 - 1] + p[m / 2]);
        return med + 1e-18;
    }

    static double SnrDb(float[] x, long from, long to, double floor)
    {
        to = Math.Min(to, x.Length);
        if (to <= from) return -20.0;
        double acc = 0;
        for (long i = from; i < to; i++) acc += (double)x[i] * x[i];
        double sig = acc / (to - from);
        return sig <= floor ? -20.0 : 10.0 * Math.Log10(Math.Max(sig - floor, 1e-18) / floor);
    }

    static string Il(Ms110dInterleaverKind k) => k.ToString();

    public static IEnumerable<string> Decode(string wavPath)
    {
        float[] audio = CorpusDecode.ReadWav(wavPath);
        var lines = new List<string>();
        double floorPower = NoiseFloorPower(audio);
        var demod = new Ms110dDemodulator();
        long pos = 0;          // samples fed when the current chunk finishes
        int burst = -1, blockIndex = 0;
        bool open = false;
        int completedAt = -1;  // index of a burst_completed line waiting for its processor time
        long startedAt = 0;
        double burstCpu = 0, cpuInBursts = 0, cpuSearch = 0;
        int bursts = 0;

        static string Hex(byte[] bits)
        {
            var sb = new StringBuilder();
            for (int i = 0; i < bits.Length; i += 8)
            {
                int v = 0;
                for (int j = 0; j < 8; j++) v = (v << 1) | (i + j < bits.Length && bits[i + j] != 0 ? 1 : 0);
                sb.Append(v.ToString("x2"));
            }
            return sb.ToString();
        }
        static string F(double d) => d.ToString("0.###", System.Globalization.CultureInfo.InvariantCulture);

        demod.BlockDecoded += b =>
        {
            lines.Add($"{{\"ev\":\"block\",\"decoder\":\"cs\",\"burst\":{burst},\"index\":{blockIndex++},\"sample\":{pos},\"bits\":{b.Bits.Length},\"bits_hex\":\"{Hex(b.Bits)}\"}}");
        };
        demod.BurstCompleted += bu =>
        {
            var l = bu.Lock;
            lines.Add($"{{\"ev\":\"burst_completed\",\"decoder\":\"cs\",\"burst\":{burst},\"sample\":{pos},\"start_sample\":{startedAt},\"wn\":{l?.WaveformNumber ?? -1},\"il\":\"{(l is null ? "none" : Il(l.Interleaver))}\",\"k\":{l?.ConstraintLength ?? 0},\"blocks\":{bu.Blocks},\"reason\":\"{bu.Reason}\",\"payload_bits\":{bu.PayloadBits.Length},\"snr_db\":{SnrDb(audio, startedAt, pos, floorPower).ToString("0.0", System.Globalization.CultureInfo.InvariantCulture)},\"cpu_ms\":0}}");
            completedAt = lines.Count - 1;
            open = false;
        };

        const int chunk = 96;
        void Feed(ReadOnlySpan<float> s)
        {
            for (int i = 0; i < s.Length; i += chunk)
            {
                int n = Math.Min(chunk, s.Length - i);
                bool wasOpen = open;
                pos += n;
                double t0 = CpuMs();
                demod.Process(s.Slice(i, n));
                double dt = CpuMs() - t0;
                if (wasOpen) { burstCpu += dt; cpuInBursts += dt; } else { cpuSearch += dt; }
                if (completedAt >= 0)
                {
                    // The completed line is written inside Process(); now that the call is done its time is known.
                    lines[completedAt] = lines[completedAt].Replace("\"cpu_ms\":0}", $"\"cpu_ms\":{F(burstCpu)}}}");
                    completedAt = -1;
                }
                if (!open && demod.State == Ms110dRxState.Tracking && demod.Lock is { } l)
                {
                    open = true; burst++; bursts++; blockIndex = 0; startedAt = pos; burstCpu = 0;
                    lines.Add($"{{\"ev\":\"burst_started\",\"decoder\":\"cs\",\"burst\":{burst},\"sample\":{pos},\"wn\":{l.WaveformNumber},\"il\":\"{Il(l.Interleaver)}\",\"k\":{l.ConstraintLength},\"cfo_hz\":{F(l.CfoHz)}}}");
                }
            }
        }
        Feed(audio);
        Feed(new float[9600 * 3]);
        if (demod.State != Ms110dRxState.Searching)
        {
            demod.Terminate();
            Feed(new float[chunk]);
        }
        lines.Add($"{{\"ev\":\"summary\",\"decoder\":\"cs\",\"version\":\"{typeof(Ms110dDemodulator).Assembly.GetName().Version?.ToString(3)}\",\"audio_s\":{F(audio.Length / 9600.0)},\"bursts\":{bursts},\"cpu_ms_total\":{F(cpuInBursts + cpuSearch)},\"cpu_ms_in_bursts\":{F(cpuInBursts)},\"cpu_ms_search\":{F(cpuSearch)}}}");
        return Finish(lines);
    }

    static byte[] Unhex(string hex, int bits)
    {
        var b = new byte[bits];
        for (int i = 0; i < bits; i++) b[i] = (byte)((Convert.ToInt32(hex.Substring(2 * (i >> 3), 2), 16) >> (7 - (i & 7))) & 1);
        return b;
    }

    static byte[] Pack(IReadOnlyList<byte> bits, int count)
    {
        var r = new byte[(count + 7) / 8];
        for (int i = 0; i < count; i++) if (bits[i] != 0) r[i >> 3] |= (byte)(0x80 >> (i & 7));
        return r;
    }

    public static IEnumerable<string> Finish(IEnumerable<string> raw)
    {
        var outLines = new List<string>();
        int good = 0, corrected = 0;
        var framesHash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        Il2pDeframer? deframer = null;
        Il2pDeframer New() => new((frame, info) =>
        {
            good++;
            corrected += info.CorrectedSymbols;
            framesHash.AppendData(BitConverter.GetBytes(frame.Length));
            framesHash.AppendData(frame);
        }, crcMode: true);
        var payload = new List<byte>();

        foreach (string line in raw)
        {
            if (string.IsNullOrWhiteSpace(line)) continue;
            var o = JsonNode.Parse(line)!.AsObject();
            string ev = o["ev"]!.GetValue<string>();
            if (ev == "burst_started")
            {
                deframer = New(); good = corrected = 0; payload.Clear();
                framesHash.GetHashAndReset();
            }
            else if (ev == "block")
            {
                int bits = o["bits"]!.GetValue<int>();
                byte[] b = Unhex(o["bits_hex"]!.GetValue<string>(), bits);
                deframer ??= New();
                int before = good;
                foreach (byte bit in b) deframer.PushBit(bit);
                payload.AddRange(b);
                o.Remove("bits_hex");
                o["good_frames"] = good - before;
            }
            else if (ev == "burst_completed")
            {
                deframer ??= New();
                int cut = (int)Math.Min(o["payload_bits"]!.GetValue<long>(), payload.Count);
                byte[] packed = Pack(payload, cut);
                o["bytes"] = packed.Length;
                o["sha256"] = Convert.ToHexString(SHA256.HashData(packed)).ToLowerInvariant();
                o["good_frames"] = good;
                o["frames_sha256"] = Convert.ToHexString(framesHash.GetHashAndReset()).ToLowerInvariant();
                o["corrected"] = corrected;
                o["crc_failures"] = deframer.CrcFailures;
                o["rs_failures"] = deframer.RsFailures;
                o["error_free"] = good > 0 && deframer.CrcFailures == 0 && deframer.RsFailures == 0;
                deframer = null; payload.Clear(); good = corrected = 0;
            }
            outLines.Add(o.ToJsonString(new JsonSerializerOptions { Encoder = System.Text.Encodings.Web.JavaScriptEncoder.UnsafeRelaxedJsonEscaping }));
        }
        return outLines;
    }

    // Raw events (bits_hex still there) of the C# decode are finished by Decode itself; this entry is for the C++ file.
    public static string Run(string mode, string path)
    {
        var lines = mode == "decode" ? Decode(path) : Finish(File.ReadLines(path));
        var sb = new StringBuilder();
        foreach (var l in lines) sb.Append(l).Append('\n');
        return sb.ToString();
    }
}
