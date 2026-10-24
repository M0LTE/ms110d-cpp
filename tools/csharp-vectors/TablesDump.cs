// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later

// Dumps the C# mode, interleaver, constant and constellation tables as text for
// tests/data/csharp-tables-0.2.0.txt. Run from the repo root:
//   dotnet run -c Release --project tools/csharp-vectors -- tables > tests/data/csharp-tables-0.2.0.txt
// Everything comes from public types except the BPSK/QPSK/8PSK/16QAM bit-to-symbol
// transcodes, which are private in Ms110dModulator; those are evaluated for every input
// through reflection on the real method, so the dump holds what the C# computes.

using System.Globalization;
using System.Reflection;
using System.Text;
using M0LTE.Ms110d;

static class TablesDump
{
    public static string Run()
    {
        var sb = new StringBuilder();
        var inv = CultureInfo.InvariantCulture;
        string F(float f) => f.ToString("R", inv);
        void Bytes(string n, ReadOnlySpan<byte> b) { sb.Append(n); foreach (var x in b) sb.Append(' ').Append(x); sb.Append('\n'); }
        void Cx(string n, Complex32[] c) { sb.Append(n); foreach (var x in c) sb.Append(' ').Append(F(x.Re)).Append(' ').Append(F(x.Im)); sb.Append('\n'); }

        sb.Append("# Dumped from M0LTE.Ms110d 0.2.0 by tools/csharp-vectors (tables mode): Ms110dTables, Ms110dMode, Ms110dInterleaverParams, MiniProbe and the modulator's transcodes.\n");
        sb.Append("eom ").Append(Ms110dTables.Eom).Append('\n');
        sb.Append("preamble_chips_per_symbol ").Append(Ms110dTables.PreambleChipsPerSymbol).Append('\n');
        sb.Append("symbol_rate ").Append(Ms110dTables.SymbolRate).Append('\n');
        sb.Append("sub_carrier_hz ").Append(Ms110dTables.SubCarrierHz).Append('\n');
        Bytes("fixed_pn", Ms110dTables.FixedPn); Bytes("cnt_pn", Ms110dTables.CntPn); Bytes("wid_pn", Ms110dTables.WidPn);
        for (int i = 0; i < 4; i++) Bytes("walsh" + i, Ms110dTables.Walsh[i]);
        Bytes("fixed_dibits", Ms110dTables.FixedDibits);
        Cx("psk8", Ms110dTables.Psk8); Cx("qam16", Ms110dTables.Qam16);
        Cx("base13", Ms110dTables.Base13); Cx("base16", Ms110dTables.Base16); Cx("base19", Ms110dTables.Base19); Cx("base25", Ms110dTables.Base25);
        foreach (int k in new[] { 24, 32, 36, 48 })
        {
            Cx($"probe{k}", MiniProbe.Get(k, false));
            Cx($"probe{k}_boundary", MiniProbe.Get(k, true));
        }

        // Transcodes: evaluate Ms110dModulator.Transcode (private) for every input value. The
        // fetched bits are given MSB first, so input v of n bits is bits (v >> (n-1)) ... (v & 1).
        MethodInfo transcode = typeof(Ms110dModulator).GetMethod("Transcode", BindingFlags.NonPublic | BindingFlags.Instance)!;
        void Transcodes(string name, int wn, int bits)
        {
            var modulator = new Ms110dModulator(new Ms110dTxSettings { WaveformNumber = wn });
            sb.Append(name);
            for (int v = 0; v < (1 << bits); v++)
            {
                var fetched = new byte[bits];
                for (int b = 0; b < bits; b++) fetched[b] = (byte)((v >> (bits - 1 - b)) & 1);
                object?[] args = [fetched, 0];
                int result = (int)transcode.Invoke(modulator, args)!;
                sb.Append(' ').Append(result);
            }
            sb.Append('\n');
        }
        Transcodes("transcode_bpsk", 1, 1);
        Transcodes("transcode_qpsk", 6, 2);
        Transcodes("transcode_8psk", 7, 3);

        foreach (int wn in new[] { 0, 1, 2, 3, 4, 5, 6, 7, 8, 13 })
        {
            var m = Ms110dMode.Mode3k(wn);
            sb.Append($"mode {m.Wn} {m.Modulation} {m.BitsPerSymbol} {m.U} {m.K} {m.CodeRate} {m.Bps}\n");
            foreach (var kind in Enum.GetValues<Ms110dInterleaverKind>())
            {
                if (!Ms110dInterleaverParams.Has3k(wn, kind)) { sb.Append($"il {wn} {kind} none\n"); continue; }
                var p = Ms110dInterleaverParams.Get3k(wn, kind);
                sb.Append($"il {wn} {kind} {p.Frames} {p.SizeBits} {p.InputBits} {p.Increment}\n");
            }
        }
        foreach (int wn in new[] { 9, 10, 11, 12, 14 })
            sb.Append($"nomode {wn} {Ms110dInterleaverParams.Has3k(wn, Ms110dInterleaverKind.Short)}\n");
        return sb.ToString();
    }
}
