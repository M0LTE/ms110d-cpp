// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later

// The C# 0.2.1 transmitter shifts the mini-probe before the SELECTED interleaver's boundary. The
// C++ transmitter sends the MIL-STD-188-110D D.5.2.2 pattern instead (shifted only at the Long
// interleaver's boundary; issue 68). The C# has an internal, test-only switch for the standard
// pattern; this reaches it through reflection, so the C# side of a comparison with the C++
// transmitter can be driven into the same mode.

using System.Reflection;
using M0LTE.Ms110d;

static class StandardTx
{
    /// <summary>The audio file of a reference case with the standard probe pattern (see StandardReference.cs): the
    /// receive dumps run the C# receiver on this, because the C++ receiver expects only the standard pattern.</summary>
    public static string AudioPath(string refDir, string caseName) =>
        Path.Combine(Path.GetDirectoryName(Path.GetFullPath(refDir).TrimEnd(Path.DirectorySeparatorChar))!, "standard", caseName, "13_audio.f32");

    static readonly PropertyInfo Switch =
        typeof(Ms110dModulator).GetProperty("StandardProbePattern", BindingFlags.NonPublic | BindingFlags.Instance)
        ?? throw new InvalidOperationException("Ms110dModulator.StandardProbePattern not found");

    /// <summary>A modulator for these settings sending the standard probe pattern.</summary>
    public static Ms110dModulator Create(Ms110dTxSettings settings)
    {
        var tx = new Ms110dModulator(settings);
        Switch.SetValue(tx, true);
        return tx;
    }

    /// <summary>
    /// Drives the C# receiver as if it knew from the first frame that the burst uses the standard pattern. The C# 0.2.1
    /// receiver starts every burst on the current rule and switches to the standard one only once the summed evidence of
    /// the probes the rules disagree on reaches +0.5, so for the first few frames of a short-interleaver burst it
    /// processes the wrong probe. The C++ receiver has no pattern hypotheses and expects the standard pattern from the
    /// first frame (issue 68), so a frame-by-frame comparison needs the C# side resolved in advance: at the "agc@"
    /// diagnostic (after the burst's pattern is configured, before its first frame) the evidence is set to a
    /// conclusive value and the scoring locked. Used only by the dumps that compare a C# receive result with the C++.
    /// </summary>
    public static void ResolvePatternToStandard(Ms110dDemodulator demod)
    {
        const BindingFlags flags = BindingFlags.NonPublic | BindingFlags.Instance;
        object pattern = typeof(Ms110dDemodulator).GetField("_probePattern", flags)!.GetValue(demod)!;
        FieldInfo evidence = pattern.GetType().GetField("_evidence", flags)!;
        FieldInfo locked = pattern.GetType().GetField("_locked", flags)!;
        demod.FrameDiagnostics += line =>
        {
            if (line.StartsWith("agc@", StringComparison.Ordinal))
            {
                evidence.SetValue(pattern, 10.0);
                locked.SetValue(pattern, true);
            }
        };
    }
}
