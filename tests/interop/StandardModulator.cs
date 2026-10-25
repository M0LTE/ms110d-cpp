// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later

using System.Reflection;
using M0LTE.Ms110d;

namespace Ms110dInterop.Tests;

/// <summary>
/// The C# 0.2.1 transmitter shifts the mini-probe before the SELECTED interleaver's boundary; the C++ transmitter
/// sends the MIL-STD-188-110D D.5.2.2 pattern (shifted only at the Long interleaver's boundary; issue 68). The C#
/// has an internal, test-only switch for the standard pattern, reached here by reflection, so that a C#
/// transmitter compared with the C++ one sends the same pattern. For the Long interleaver the two coincide.
/// </summary>
internal static class StandardModulator
{
    private static readonly PropertyInfo Switch =
        typeof(Ms110dModulator).GetProperty("StandardProbePattern", BindingFlags.NonPublic | BindingFlags.Instance)
        ?? throw new InvalidOperationException("Ms110dModulator.StandardProbePattern not found; the reference package changed");

    /// <summary>A C# modulator for these settings sending the standard probe pattern.</summary>
    public static Ms110dModulator Create(Ms110dTxSettings settings)
    {
        var tx = new Ms110dModulator(settings);
        Switch.SetValue(tx, true);
        return tx;
    }
}
