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
        foreach (JsonElement b in bursts.EnumerateArray())
        {
            string id = b.GetProperty("id").GetString()!;
            string path = Path.Combine(dir, b.GetProperty("file").GetString()!);
            Assert.Equal(b.GetProperty("sha256").GetString(),
                Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(path))).ToLowerInvariant());

            var got = CorpusDecode.Run(CorpusDecode.ReadWav(path));
            JsonElement[] want = b.GetProperty("expected").EnumerateArray().ToArray();
            Assert.True(got.Count == want.Length, $"{id}: {got.Count} bursts decoded, manifest says {want.Length}");
            bool stable = b.GetProperty("stable").GetBoolean();
            for (int i = 0; i < want.Length; i++)
            {
                JsonElement w = want[i];
                Assert.True(got[i].Blocks == w.GetProperty("blocks").GetInt32(), $"{id}: block count");
                Assert.True(got[i].Reason == w.GetProperty("reason").GetString(), $"{id}: end reason");
                Assert.True(got[i].Wn == w.GetProperty("wn").GetInt32(), $"{id}: waveform");
                Assert.True(got[i].Bytes == w.GetProperty("bytes").GetInt32(), $"{id}: payload length");
                // An unstable burst is one whose payload changes with tiny input differences, so
                // only the stable ones are held to the payload hash (docs/dev/corpus.md).
                if (stable)
                {
                    Assert.True(got[i].Sha256 == w.GetProperty("sha256").GetString(), $"{id}: payload hash");
                }
            }
        }
    }
}
