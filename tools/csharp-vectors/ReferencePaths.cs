// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later

// Path helpers for the reference files. The C# 0.4.1 transmitter and receiver use the MIL-STD-188-110D D.5.2.2
// standard mini-probe pattern only, so the reference files hold the standard probes and audio and the
// receive dumps run the C# receiver on them directly, with nothing forced.

static class ReferencePaths
{
    /// <summary>The audio file of a reference case (the receive dumps run the C# receiver on this).</summary>
    public static string AudioPath(string refDir, string caseName)
    {
        string dir = Path.Combine(refDir, caseName);
        var m = File.ReadAllLines(Path.Combine(dir, "manifest.txt"))
            .Select(l => l.Split('=', 2)).Where(p => p.Length == 2).ToDictionary(p => p[0], p => p[1]);
        return Path.Combine(dir, m["file.audio_f32le"]);
    }
}
