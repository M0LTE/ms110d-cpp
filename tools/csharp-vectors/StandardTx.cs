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
}
