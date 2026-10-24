// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later

using M0LTE.HfChannel;

namespace Ms110dInterop.Tests.Layer3;

/// <summary>
/// A channel the harness sweeps: AWGN, or ITU-R F.1487 Good, Moderate and Poor (the same two equal-power
/// Rayleigh paths, differing in delay and Doppler spread), made by M0LTE.HfChannel.
/// </summary>
public sealed record Layer3Channel(string Name, WattersonPath[] Paths)
{
    public static readonly Layer3Channel Awgn = new("awgn", []);

    /// <summary>ITU-R F.1487 Good: 2 paths, 0.5 ms apart, 0.1 Hz Doppler spread.</summary>
    public static readonly Layer3Channel Good = new("good", [new(0, true, 0.1), new(0.5, true, 0.1)]);

    /// <summary>ITU-R F.1487 Moderate: 2 paths, 1 ms apart, 0.5 Hz Doppler spread.</summary>
    public static readonly Layer3Channel Moderate = new("moderate", [new(0, true, 0.5), new(1.0, true, 0.5)]);

    /// <summary>ITU-R F.1487 Poor (the D.6.1 channel): 2 paths, 2 ms apart, 1 Hz Doppler spread.</summary>
    public static readonly Layer3Channel Poor = new("poor", WattersonChannel.Poor);

    public static IReadOnlyList<Layer3Channel> All => [Awgn, Good, Moderate, Poor];

    public static Layer3Channel ByName(string name) =>
        All.FirstOrDefault(c => c.Name == name) ?? throw new ArgumentException($"unknown channel '{name}'");

    /// <summary>Position in <see cref="All"/>, used to keep each channel's seeds apart.</summary>
    public int Index => Name switch { "awgn" => 0, "good" => 1, "moderate" => 2, _ => 3 };

    public bool Fading => Paths.Any(p => p.Fading);

    /// <summary>The channel for one frame. The seed fixes the fading and the noise, so every transmitter
    /// and receiver sees the identical realisation.</summary>
    public WattersonChannel Create(int seed) => new(9600, seed, Paths);
}
