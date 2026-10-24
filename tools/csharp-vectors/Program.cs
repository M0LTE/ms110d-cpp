// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later

// Dumps scrambler outputs and interleaver permutations from the C# reference (public API
// only) as text. Run from the repo root: dotnet run -c Release --project tools/csharp-vectors > out.txt, then tools/csharp-vectors/make_header.py
// out.txt turns it into tests/csharp_vectors.hpp. Not built by CMake.
//
// Interleaver load locations: the float indices 0..size-1 are pushed through
// Ms110dInterleaver.Interleave, so fetched[loc] = n; inverting that gives load[n] = loc, the
// wire position of punctured bit n (what the C++ load_location(n) returns).

using System.Text;
using M0LTE.Ms110d;
using M0LTE.Ms110d.Fec;

// "tables" mode: the mode/table dump for tests/data/csharp-tables-0.2.0.txt (see TablesDump.cs).
if (args.Length > 0 && args[0] == "tables")
{
    Console.Out.Write(TablesDump.Run());
    return;
}

if (args.Length > 0 && args[0] == "fec")
{
    Console.Out.Write(FecDump.Run());
    return;
}

// "soft" mode: Viterbi, SISO and BCJR vectors for tests/data/csharp-soft-0.2.0.txt (see SoftDump.cs).
if (args.Length > 0 && args[0] == "soft")
{
    Console.Out.Write(SoftDump.Run());
    return;
}

// "framing" mode: waveform ID and downcount di-bits for tests/data/csharp-framing-0.2.0.txt (see FramingDump.cs).
if (args.Length > 0 && args[0] == "framing")
{
    Console.Out.Write(FramingDump.Run());
    return;
}

// "detect" mode: what the C# preamble search finds, for tests/data/csharp-detect-0.2.0.txt (see DetectDump.cs).
if (args.Length > 1 && args[0] == "detect")
{
    Console.Out.Write(DetectDump.Run(args[1]));
    return;
}

static ulong Fnv(IEnumerable<uint> values)
{
    ulong h = 14695981039346656037UL;
    foreach (uint v in values)
        for (int b = 0; b < 4; b++) { h ^= (v >> (8 * b)) & 0xFF; h *= 1099511628211UL; }
    return h;
}
var sb = new StringBuilder();
string Head(IEnumerable<int> v, int n) => string.Join(", ", v.Take(n));

// Data scrambler: 1024 outputs from reset, input symbol 0 (so output = register bits).
var s = new Ms110dScrambler();
var psk = Enumerable.Range(0, 1024).Select(_ => s.NextPsk(0)).ToArray();
sb.AppendLine($"psk0 head32 {{{Head(psk, 32)}}} fnv 0x{Fnv(psk.Select(x => (uint)x)):X16}");
// PSK with a nonzero symbol stream: symbol i % 8.
s.Reset();
var psk2 = Enumerable.Range(0, 1024).Select(i => s.NextPsk(i % 8)).ToArray();
sb.AppendLine($"pskmix fnv 0x{Fnv(psk2.Select(x => (uint)x)):X16}");
foreach (int bits in new[] { 4, 5, 6, 8 })
{
    s.Reset();
    var q = Enumerable.Range(0, 1024).Select(_ => s.NextQam(0, bits)).ToArray();
    s.Reset();
    var q2 = Enumerable.Range(0, 1024).Select(i => s.NextQam(i & ((1 << bits) - 1), bits)).ToArray();
    sb.AppendLine($"qam{bits} head32 {{{Head(q, 32)}}} fnv 0x{Fnv(q.Select(x => (uint)x)):X16} mixfnv 0x{Fnv(q2.Select(x => (uint)x)):X16}");
}
// Trinomial: 4200 symbols (wraps twice past 2048).
var t = new Trinomial159Scrambler();
var tri = Enumerable.Range(0, 4200).Select(_ => t.Next()).ToArray();
sb.AppendLine($"tri head64 {{{Head(tri, 64)}}} fnv4200 0x{Fnv(tri.Select(x => (uint)x)):X16} fnv2048 0x{Fnv(tri.Take(2048).Select(x => (uint)x)):X16} sym2048..2079 {{{string.Join(", ", tri.Skip(2048).Take(32))}}}");

// Interleaver: every (wn, kind) the C# defines.
foreach (int wn in new[] { 0, 1, 2, 3, 4, 5, 6, 7, 8, 13 })
    foreach (var kind in Enum.GetValues<Ms110dInterleaverKind>())
    {
        if (!Ms110dInterleaverParams.Has3k(wn, kind)) continue;
        var p = Ms110dInterleaverParams.Get3k(wn, kind);
        var il = new Ms110dInterleaver(p.SizeBits, p.Increment);
        var inb = Enumerable.Range(0, p.SizeBits).Select(i => (byte)(i & 0xFF)).ToArray();
        // Index-valued bytes can't hold > 255, so use float indices for the permutation.
        var inf = Enumerable.Range(0, p.SizeBits).Select(i => (float)i).ToArray();
        var fetched = new float[p.SizeBits];
        il.Interleave((ReadOnlySpan<float>)inf, fetched);
        var back = new float[p.SizeBits];
        il.Deinterleave(fetched, back);
        bool rt = back.SequenceEqual(inf);
        var fb = new byte[p.SizeBits];
        il.Interleave((ReadOnlySpan<byte>)inb, fb);
        var perm = fetched.Select(f => (int)f).ToArray();
        // fetched[loc] = n ; wire position of bit n is loc. We emit the "load locations" (loc of n) via inverse.
        var load = new int[p.SizeBits];
        for (int loc = 0; loc < p.SizeBits; loc++) load[perm[loc]] = loc;
        sb.AppendLine($"il wn={wn} kind={(int)kind} frames={p.Frames} size={p.SizeBits} input={p.InputBits} inc={p.Increment} rt={rt} load16 {{{Head(load, 16)}}} fnvLoad 0x{Fnv(load.Select(x => (uint)x)):X16} fnvBytes 0x{Fnv(fb.Select(x => (uint)x)):X16}");
    }
Console.Write(sb.ToString());
