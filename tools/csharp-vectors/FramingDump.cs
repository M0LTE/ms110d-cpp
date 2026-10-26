// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later

// Dumps the C# waveform ID and downcount di-bits for every input, for
// tests/data/csharp-framing-0.3.0.txt. Run from the repo root:
//   dotnet run -c Release --project tools/csharp-vectors -- framing > tests/data/csharp-framing-0.3.0.txt
// PreambleGenerator.EncodeWid and EncodeCount are internal, so they are called through
// reflection on the real methods; the dump holds what the C# computes.

using System.Reflection;
using System.Text;
using M0LTE.Ms110d;

static class FramingDump
{
    public static string Run()
    {
        var sb = new StringBuilder();
        sb.Append("# Dumped from M0LTE.Ms110d 0.3.0 by tools/csharp-vectors (framing mode): PreambleGenerator.EncodeWid and EncodeCount.\n");
        BindingFlags flags = BindingFlags.NonPublic | BindingFlags.Static;
        MethodInfo wid = typeof(PreambleGenerator).GetMethod("EncodeWid", flags)!;
        MethodInfo count = typeof(PreambleGenerator).GetMethod("EncodeCount", flags)!;
        for (int wn = 0; wn <= 13; wn++)
            foreach (var kind in Enum.GetValues<Ms110dInterleaverKind>())
                foreach (int k in new[] { 7, 9 })
                {
                    var d = (byte[])wid.Invoke(null, [wn, kind, k])!;
                    sb.Append($"wid {wn} {(int)kind} {k} {string.Join(' ', d)}\n");
                }
        for (int c = 0; c < 32; c++)
        {
            var d = (byte[])count.Invoke(null, [c])!;
            sb.Append($"count {c} {string.Join(' ', d)}\n");
        }
        return sb.ToString();
    }
}
