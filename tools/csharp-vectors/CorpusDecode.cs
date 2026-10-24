// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later

// Layer 4 corpus support: runs the C# receiver over 9600 Hz 16-bit mono WAV files.
//   corpus find   <wav>   one line per burst with its position in the file (used by curation); both read 16-bit PCM or 32-bit float WAV
//   corpus decode <wav>   one line per burst without positions (the expected results)
// Line format: "burst start=<sample> end=<sample> wn=<n> il=<kind> k=<n> blocks=<n> reason=<r> bytes=<n> sha256=<hex> good_frames=<n> frames_sha256=<hex> corrected=<n> crc_failures=<n> rs_failures=<n>"
// (decode omits start and end). good_frames counts the IL2P+CRC frames that passed (the deframer of M0LTE.Il2p, fed the
// decoded blocks as the modem does); frames_sha256 hashes them (4-byte LE length then bytes, in order). Payload bits are packed MSB first, a short last byte is zero padded.
// With CORPUS_DUMP_PAYLOAD_DIR set, each burst's packed payload is also written there as <n>.bin (for diagnosis).
// The same code is linked into tests/interop, so the test and the tool cannot disagree.

using System.Globalization;
using System.Security.Cryptography;
using System.Text;
using M0LTE.Il2p;
using M0LTE.Ms110d;

public static class CorpusDecode
{
    public sealed record Found(long Start, long End, int Wn, string Il, int K, int Blocks, string Reason, int Bytes, string Sha256,
        int GoodFrames, string FramesSha256, int Corrected, long CrcFailures, long RsFailures,
        bool AllFramesPassCrc = true)
    {
        // Every IL2P frame the deframer found passed its CRC, and nothing failed Reed-Solomon or CRC.
        public bool ErrorFree => GoodFrames > 0 && CrcFailures == 0 && RsFailures == 0;
    }

    public static float[] ReadWav(string path)
    {
        byte[] b = File.ReadAllBytes(path);
        if (b.Length < 44 || Encoding.ASCII.GetString(b, 0, 4) != "RIFF" || Encoding.ASCII.GetString(b, 8, 4) != "WAVE")
            throw new InvalidDataException(path + ": not a RIFF/WAVE file");
        int pos = 12;
        bool isFloat = false;
        while (pos + 8 <= b.Length)
        {
            string id = Encoding.ASCII.GetString(b, pos, 4);
            int len = BitConverter.ToInt32(b, pos + 4);
            if (id == "fmt ")
            {
                int fmt = BitConverter.ToInt16(b, pos + 8), ch = BitConverter.ToInt16(b, pos + 10);
                int rate = BitConverter.ToInt32(b, pos + 12), bits = BitConverter.ToInt16(b, pos + 22);
                if (!((fmt == 1 && bits == 16) || (fmt == 3 && bits == 32)) || ch != 1 || rate != 9600)
                    throw new InvalidDataException(path + ": need 9600 Hz mono 16-bit PCM or 32-bit float");
                isFloat = fmt == 3;
            }
            else if (id == "data")
            {
                int w = isFloat ? 4 : 2;
                int n = Math.Min(len, b.Length - pos - 8) / w;
                var s = new float[n];
                for (int i = 0; i < n; i++)
                    s[i] = isFloat ? BitConverter.ToSingle(b, pos + 8 + 4 * i) : BitConverter.ToInt16(b, pos + 8 + 2 * i) / 32768f;
                return s;
            }
            pos += 8 + len + (len & 1);
        }
        throw new InvalidDataException(path + ": no data chunk");
    }

    // Feeds the audio in small chunks so burst positions are good to 10 ms. Start is the sample at
    // which the receiver left Searching (the preamble is already some seconds old by then).
    public static List<Found> Run(float[] audio)
    {
        var demod = new Ms110dDemodulator();
        var outList = new List<Found>();
        long fed = 0;
        long start = -1;
        // The mailcast framing: IL2P+CRC frames back to back in the burst's decoded bits. Frames that
        // fail Reed-Solomon or the CRC are dropped and counted, as in the modem.
        int goodFrames = 0, corrected = 0;
        var framesHash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        Il2pDeframer? deframer = null;
        Il2pDeframer NewDeframer() => new((frame, info) =>
        {
            goodFrames++;
            corrected += info.CorrectedSymbols;
            framesHash.AppendData(BitConverter.GetBytes(frame.Length));
            framesHash.AppendData(frame);
        }, crcMode: true);
        deframer = NewDeframer();
        demod.BlockDecoded += b => { foreach (byte bit in b.Bits) deframer.PushBit(bit); };
        demod.BurstCompleted += bu =>
        {
            byte[] packed = Pack(bu.PayloadBits);
            string? dump = Environment.GetEnvironmentVariable("CORPUS_DUMP_PAYLOAD_DIR");
            if (dump is not null) File.WriteAllBytes(Path.Combine(dump, $"{outList.Count}.bin"), packed);
            string sha = Convert.ToHexString(SHA256.HashData(packed)).ToLowerInvariant();
            var l = bu.Lock;
            outList.Add(new Found(start, fed, l?.WaveformNumber ?? -1, l?.Interleaver.ToString() ?? "none",
                l?.ConstraintLength ?? 0, bu.Blocks, bu.Reason.ToString(), packed.Length, sha,
                goodFrames, Convert.ToHexString(framesHash.GetHashAndReset()).ToLowerInvariant(), corrected,
                deframer.CrcFailures, deframer.RsFailures));
            start = -1;
            goodFrames = corrected = 0;
            deframer = NewDeframer();
        };
        const int chunk = 96;
        void Feed(ReadOnlySpan<float> s)
        {
            for (int i = 0; i < s.Length; i += chunk)
            {
                int n = Math.Min(chunk, s.Length - i);
                demod.Process(s.Slice(i, n));
                fed += n;
                if (start < 0 && demod.State != Ms110dRxState.Searching) start = fed;
            }
        }
        Feed(audio);
        Feed(new float[9600 * 3]);
        if (demod.State != Ms110dRxState.Searching)
        {
            demod.Terminate();
            Feed(new float[chunk]);
        }
        return outList;
    }

    static byte[] Pack(byte[] bits)
    {
        var r = new byte[(bits.Length + 7) / 8];
        for (int i = 0; i < bits.Length; i++) if (bits[i] != 0) r[i >> 3] |= (byte)(0x80 >> (i & 7));
        return r;
    }

    public static string Line(Found f, bool positions)
    {
        var inv = CultureInfo.InvariantCulture;
        var sb = new StringBuilder("burst");
        if (positions) sb.Append(inv, $" start={f.Start} end={f.End}");
        sb.Append(inv, $" wn={f.Wn} il={f.Il} k={f.K} blocks={f.Blocks} reason={f.Reason} bytes={f.Bytes} sha256={f.Sha256} good_frames={f.GoodFrames} frames_sha256={f.FramesSha256} corrected={f.Corrected} crc_failures={f.CrcFailures} rs_failures={f.RsFailures}");
        return sb.ToString();
    }

    public static string Report(string mode, string wavPath)
    {
        var sb = new StringBuilder();
        foreach (var f in Run(ReadWav(wavPath))) sb.Append(Line(f, mode == "find")).Append('\n');
        return sb.ToString();
    }
}
