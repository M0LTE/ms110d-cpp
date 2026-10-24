// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later

using System.Security.Cryptography;
using System.Text.Json;
using Xunit;

namespace Ms110dInterop.Tests;

// Layer 4 (docs/dev/corpus.md): the corpus is self-consistent. Every curated burst file has the
// recorded hash and the pinned C# receiver decodes it to what the manifest says. Two corpora are
// checked: the small set committed under tests/data/corpus (always present), and the full one
// fetched by scripts/fetch-corpus.sh (skipped when absent; MS110D_REQUIRE_CORPUS=1 makes absence
// a failure). The C++ receiver gets its own comparison against the same manifests later.
public class CorpusTests
{
    static string RepoRoot()
    {
        string pin = Environment.GetEnvironmentVariable("MS110D_PIN_FILE")
            ?? throw new InvalidOperationException("MS110D_PIN_FILE is not set; run these tests through ctest.");
        return Path.GetFullPath(Path.Combine(Path.GetDirectoryName(Path.GetFullPath(pin))!, ".."));
    }

    [Fact]
    public void CommittedSetMatchesItsManifest() => Check(Path.Combine(RepoRoot(), "tests", "data", "corpus"));

    [Fact]
    public void FetchedCorpusMatchesItsManifest()
    {
        string dir = Environment.GetEnvironmentVariable("MS110D_CORPUS_DIR")
            ?? Path.Combine(RepoRoot(), "build", "_corpus", "current");
        if (!File.Exists(Path.Combine(dir, "manifest.json")))
        {
            Assert.False(Environment.GetEnvironmentVariable("MS110D_REQUIRE_CORPUS") == "1",
                $"no corpus in {dir}; run scripts/fetch-corpus.sh");
            Assert.Skip($"no corpus in {dir}; run scripts/fetch-corpus.sh to fetch it");
        }

        Check(dir);
    }

    static void Check(string dir)
    {
        using JsonDocument doc = JsonDocument.Parse(File.ReadAllText(Path.Combine(dir, "manifest.json")));
        JsonElement bursts = doc.RootElement.GetProperty("bursts");
        Assert.True(bursts.GetArrayLength() > 0, "manifest has no bursts");
        var pairs = new List<(JsonElement Want, CorpusDecode.Found Got)>();
        foreach (JsonElement b in bursts.EnumerateArray())
        {
            string id = b.GetProperty("id").GetString()!;
            string path = Path.Combine(dir, b.GetProperty("file").GetString()!);
            Assert.Equal(b.GetProperty("sha256").GetString(),
                Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(path))).ToLowerInvariant());

            var got = CorpusDecode.Run(CorpusDecode.ReadWav(path));
            JsonElement[] want = b.GetProperty("expected").EnumerateArray().ToArray();
            Assert.True(got.Count == want.Length, $"{id}: {got.Count} bursts decoded, manifest says {want.Length}");
            for (int i = 0; i < want.Length; i++)
            {
                JsonElement w = want[i];
                Assert.True(got[i].Blocks == w.GetProperty("blocks").GetInt32(), $"{id}: block count");
                Assert.True(got[i].Reason == w.GetProperty("reason").GetString(), $"{id}: end reason");
                Assert.True(got[i].Wn == w.GetProperty("wn").GetInt32(), $"{id}: waveform");
                Assert.True(got[i].Il == w.GetProperty("il").GetString(), $"{id}: interleaver");
                Assert.True(got[i].K == w.GetProperty("k").GetInt32(), $"{id}: constraint length");
                Assert.True(got[i].GoodFrames == w.GetProperty("good_frames").GetInt32(), $"{id}: good frames");
                Assert.True(got[i].FramesSha256 == w.GetProperty("frames_sha256").GetString(), $"{id}: good frame hash");
                Assert.True(got[i].Corrected == w.GetProperty("corrected").GetInt32(), $"{id}: symbols corrected");
                Assert.Equal(b.GetProperty("error_free").GetBoolean(), got[i].ErrorFree);
                // The raw payload hash is exact only for error-free bursts (the rule in docs/dev/corpus.md):
                // elsewhere it covers bits of frames that fail their CRC, which differ between machines
                // (CI saw this on 20261024T135800Z-06).
                if (got[i].ErrorFree)
                {
                    Assert.True(got[i].Sha256 == w.GetProperty("sha256").GetString(), $"{id}: payload hash");
                }
            }

            pairs.Add((b, got[0]));
        }

        // The pinned C# must satisfy the layer 4 rule against its own manifest.
        Assert.Empty(Layer4Rule.Evaluate(pairs));
    }

    // ---- the rule itself, exercised on synthetic candidates --------------------------------

    static (List<(JsonElement, CorpusDecode.Found)> Pairs, JsonDocument Doc) Committed()
    {
        string dir = Path.Combine(RepoRoot(), "tests", "data", "corpus");
        JsonDocument doc = JsonDocument.Parse(File.ReadAllText(Path.Combine(dir, "manifest.json")));
        var pairs = new List<(JsonElement, CorpusDecode.Found)>();
        foreach (JsonElement b in doc.RootElement.GetProperty("bursts").EnumerateArray())
        {
            pairs.Add((b, CorpusDecode.Run(CorpusDecode.ReadWav(Path.Combine(dir, b.GetProperty("file").GetString()!))).Single()));
        }

        return (pairs, doc);
    }

    [Fact]
    public void RuleAcceptsTheReferenceItself()
    {
        var (pairs, doc) = Committed();
        using (doc) Assert.Empty(Layer4Rule.Evaluate(pairs));
    }

    [Fact]
    public void RuleRejectsWhatItShould()
    {
        var (pairs, doc) = Committed();
        using (doc)
        {
            // A wrong constraint length, a wrong end reason on a burst that has good frames, a payload
            // that differs on an error-free burst, a frame decoded that failed its CRC, and two frames
            // short on one burst.
            Func<CorpusDecode.Found, CorpusDecode.Found>[] breakers =
            [
                f => f with { K = f.K == 7 ? 9 : 7 },
                f => f with { Reason = "SignalLost" },
                f => f with { Sha256 = new string('0', 64) },
                f => f with { CrcFailures = 0, AllFramesPassCrc = false },
                f => f with { GoodFrames = f.GoodFrames - 2 },
            ];
            // The first committed burst is error-free with several good frames.
            Assert.True(pairs[0].Item2.ErrorFree && pairs[0].Item2.GoodFrames >= 3);
            foreach (var breaker in breakers)
            {
                var bad = pairs.Select((p, i) => (p.Item1, i == 0 ? breaker(p.Item2) : p.Item2)).ToList();
                Assert.NotEmpty(Layer4Rule.Evaluate(bad));
            }

            // One good frame short on one burst is allowed per burst, but the corpus budget is 2 percent.
            var oneShort = pairs.Select((p, i) => (p.Item1, i == 0 ? p.Item2 with { GoodFrames = p.Item2.GoodFrames - 1 } : p.Item2)).ToList();
            int total = pairs.Sum(p => p.Item2.GoodFrames);
            Assert.Equal(total * 2 / 100 >= 1, !Layer4Rule.Evaluate(oneShort).Any());
        }
    }
}

// Layer 4 rule (docs/dev/corpus.md), the one the C++ receiver is held to. Per burst: waveform,
// interleaver and K match exactly; end reason and block count match exactly unless the manifest burst
// has no good frames; the candidate has at most one good IL2P frame fewer than the manifest, and
// at most 2 percent fewer over the whole set; every frame it decodes passes its CRC; the payload
// hash matches exactly on error-free bursts only. The C++ side supplies its own CorpusDecode.Found
// values here when the receiver exists.
static class Layer4Rule
{
    public static List<string> Evaluate(IEnumerable<(JsonElement Want, CorpusDecode.Found Got)> pairs)
    {
        var problems = new List<string>();
        int wantTotal = 0, shortTotal = 0;
        foreach (var (burst, got) in pairs)
        {
            string id = burst.GetProperty("id").GetString()!;
            JsonElement w = burst.GetProperty("expected")[0];
            int wantFrames = w.GetProperty("good_frames").GetInt32();
            void Differs(string what) => problems.Add($"{id}: {what}");
            if (got.Wn != w.GetProperty("wn").GetInt32()) Differs("waveform");
            if (got.Il != w.GetProperty("il").GetString()) Differs("interleaver");
            if (got.K != w.GetProperty("k").GetInt32()) Differs("constraint length");
            if (wantFrames > 0)
            {
                if (got.Reason != w.GetProperty("reason").GetString()) Differs("end reason");
                if (got.Blocks != w.GetProperty("blocks").GetInt32()) Differs("block count");
            }

            int missing = Math.Max(0, wantFrames - got.GoodFrames);
            if (missing > 1) Differs($"{missing} good frames short (at most 1 allowed)");
            wantTotal += wantFrames;
            shortTotal += missing;
            if (!got.AllFramesPassCrc) Differs("a decoded frame failed its CRC");
            if (burst.GetProperty("error_free").GetBoolean() && got.Sha256 != w.GetProperty("sha256").GetString())
            {
                Differs("payload differs on an error-free burst");
            }
        }

        if (shortTotal > wantTotal * 2 / 100)
        {
            problems.Add($"{shortTotal} good frames short over the set of {wantTotal} (at most 2 percent)");
        }

        return problems;
    }
}
