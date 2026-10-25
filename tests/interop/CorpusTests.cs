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
// a failure). The C++ receiver (through the C interface) is then held to the layer 4 rule against
// the same manifests, on the Long interleaver bursts only: the C++ receiver expects the standard
// mini-probe pattern alone, and corpus-v1 is current-pattern audio (the C# 0.2.1 transmitter shifts
// at the selected interleaver's boundary), so for the other interleavers the comparison is measured
// and reported, not asserted, until a corpus recorded after GB7RDG switches replaces it (issue 68).
public class CorpusTests(ITestOutputHelper output)
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

    // ---- the C++ receiver against the same manifests (layer 4) ------------------------------

    [Fact]
    public void CppMeetsTheLayer4RuleOnTheCommittedSet() => CheckCpp(Path.Combine(RepoRoot(), "tests", "data", "corpus"), floor: false);

    // Accepted difference, issue 68 (remove with corpus-v2, recorded after the stations send the standard pattern):
    // corpus-v1 is current-pattern audio, so on its non-Long bursts the C++ receiver is held to a floor instead of the
    // layer 4 rule: it identifies waveform, interleaver and constraint length on every burst, and decodes at least
    // 95 of every 99 of the C#'s good frames (measured on the fetched corpus: 95 of 99).
    [Fact]
    public void Issue68_CppFloorOnCurrentPatternAudio_CommittedSet() => CheckCpp(Path.Combine(RepoRoot(), "tests", "data", "corpus"), floor: true);

    [Fact]
    public void Issue68_CppFloorOnCurrentPatternAudio_FetchedCorpus()
    {
        string dir = Environment.GetEnvironmentVariable("MS110D_CORPUS_DIR")
            ?? Path.Combine(RepoRoot(), "build", "_corpus", "current");
        if (!File.Exists(Path.Combine(dir, "manifest.json")))
        {
            Assert.False(Environment.GetEnvironmentVariable("MS110D_REQUIRE_CORPUS") == "1",
                $"no corpus in {dir}; run scripts/fetch-corpus.sh");
            Assert.Skip($"no corpus in {dir}; run scripts/fetch-corpus.sh to fetch it");
        }

        CheckCpp(dir, floor: true);
    }

    [Fact]
    public void CppMeetsTheLayer4RuleOnTheFetchedCorpus()
    {
        string dir = Environment.GetEnvironmentVariable("MS110D_CORPUS_DIR")
            ?? Path.Combine(RepoRoot(), "build", "_corpus", "current");
        if (!File.Exists(Path.Combine(dir, "manifest.json")))
        {
            Assert.False(Environment.GetEnvironmentVariable("MS110D_REQUIRE_CORPUS") == "1",
                $"no corpus in {dir}; run scripts/fetch-corpus.sh");
            Assert.Skip($"no corpus in {dir}; run scripts/fetch-corpus.sh to fetch it");
        }

        CheckCpp(dir, floor: false);
    }

    void CheckCpp(string dir, bool floor)
    {
        using JsonDocument doc = JsonDocument.Parse(File.ReadAllText(Path.Combine(dir, "manifest.json")));
        var pairs = new List<(JsonElement Want, CorpusDecode.Found Got)>();
        int wantFrames = 0, gotFrames = 0, sameEnd = 0, errorFree = 0, errorFreeSame = 0;
        foreach (JsonElement b in doc.RootElement.GetProperty("bursts").EnumerateArray())
        {
            string id = b.GetProperty("id").GetString()!;
            var got = CppCorpusDecode.Run(CorpusDecode.ReadWav(Path.Combine(dir, b.GetProperty("file").GetString()!)));
            JsonElement w = b.GetProperty("expected")[0];
            CorpusDecode.Found first = got.Count > 0
                ? got[0]
                : new CorpusDecode.Found(-1, -1, -1, "none", 0, 0, "None", 0, "", 0, "", 0, 0, 0);
            pairs.Add((b, first));
            wantFrames += w.GetProperty("good_frames").GetInt32();
            gotFrames += Math.Min(first.GoodFrames, w.GetProperty("good_frames").GetInt32());
            bool same = first.Reason == w.GetProperty("reason").GetString() && first.Blocks == w.GetProperty("blocks").GetInt32();
            if (same) sameEnd++;
            if (b.GetProperty("error_free").GetBoolean())
            {
                errorFree++;
                if (first.Sha256 == w.GetProperty("sha256").GetString()) errorFreeSame++;
            }
            output.WriteLine($"{id}: C# wn={w.GetProperty("wn").GetInt32()} {w.GetProperty("reason").GetString()} blocks={w.GetProperty("blocks").GetInt32()} good={w.GetProperty("good_frames").GetInt32()}; " +
                $"C++ bursts={got.Count} wn={first.Wn} {first.Reason} blocks={first.Blocks} good={first.GoodFrames} crc_failures={first.CrcFailures}");
        }

        // corpus-v1 is current-pattern audio: only the Long interleaver, where the two patterns coincide, is held
        // to the layer 4 rule. The other bursts are measured with the same rule and reported (issue 68).
        var longPairs = pairs.Where(p => p.Want.GetProperty("expected")[0].GetProperty("il").GetString() == "Long").ToList();
        var otherPairs = pairs.Where(p => p.Want.GetProperty("expected")[0].GetProperty("il").GetString() != "Long").ToList();
        List<string> problems = Layer4Rule.Evaluate(longPairs);
        List<string> otherProblems = Layer4Rule.Evaluate(otherPairs);
        int otherWant = otherPairs.Sum(p => p.Want.GetProperty("expected")[0].GetProperty("good_frames").GetInt32());
        int otherGot = otherPairs.Sum(p => Math.Min(p.Got.GoodFrames, p.Want.GetProperty("expected")[0].GetProperty("good_frames").GetInt32()));
        string summary = $"layer 4, C++ receiver: {pairs.Count} bursts, good frames {gotFrames} of the C#'s {wantFrames}, " +
            $"same end and block count on {sameEnd}, error-free payloads equal {errorFreeSame} of {errorFree}. " +
            $"Asserted on the {longPairs.Count} Long interleaver bursts: rule problems {problems.Count}. " +
            $"NOT asserted on the {otherPairs.Count} other bursts (current mini-probe pattern audio, the C++ expects the standard one, issue 68): " +
            $"good frames {otherGot} of {otherWant}, rule problems {otherProblems.Count}";
        output.WriteLine(summary);
        Console.WriteLine(summary);
        foreach (string p in otherProblems) output.WriteLine("  not asserted (issue 68): " + p);
        if (floor)
        {
            // The accepted difference of issue 68: the non-Long bursts are held to a floor, not to the layer 4 rule.
            Assert.True(otherPairs.Count > 0, "no non-Long bursts: the floor test has nothing to hold");
            foreach (var (want, got) in otherPairs)
            {
                JsonElement w = want.GetProperty("expected")[0];
                string id = want.GetProperty("id").GetString()!;
                Assert.True(got.Wn == w.GetProperty("wn").GetInt32(), $"{id}: waveform");
                Assert.True(got.Il == w.GetProperty("il").GetString(), $"{id}: interleaver");
                Assert.True(got.K == w.GetProperty("k").GetInt32(), $"{id}: constraint length");
            }

            Assert.True(otherGot * 99 >= otherWant * 95,
                $"good frames {otherGot} of the C#'s {otherWant} are below the issue 68 floor of 95 in 99");
            return;
        }

        // The layer 4 rule applies to the Long interleaver bursts; with none in the corpus nothing is asserted, and
        // the test says so by skipping instead of passing green (see the floor tests for the other bursts).
        Assert.True(problems.Count == 0, string.Join("; ", problems));
        if (longPairs.Count == 0)
        {
            Assert.Skip("no Long interleaver bursts in this corpus; the rest is current-pattern audio held to the floor test (issue 68)");
        }
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

// The C++ receiver over a corpus file, fed exactly as CorpusDecode.Run feeds the C# (chunks of 96,
// three seconds of silence after, then a terminate if a burst is still open), with the same IL2P
// deframer on its decoded blocks, giving the same Found record.
static class CppCorpusDecode
{
    public static List<CorpusDecode.Found> Run(float[] audio)
    {
        using var rx = new CppReceiver();
        var outList = new List<CorpusDecode.Found>();
        long fed = 0, start = -1;
        int goodFrames = 0, corrected = 0;
        var framesHash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        M0LTE.Il2p.Il2pDeframer NewDeframer() => new((frame, info) =>
        {
            goodFrames++;
            corrected += info.CorrectedSymbols;
            framesHash.AppendData(BitConverter.GetBytes(frame.Length));
            framesHash.AppendData(frame);
        }, crcMode: true);
        var deframer = NewDeframer();
        rx.BlockDecoded = bits => { foreach (byte bit in bits) deframer.PushBit(bit); };
        void Completed(CppBurst bu)
        {
            byte[] packed = Pack(bu.PayloadBits);
            string sha = Convert.ToHexString(SHA256.HashData(packed)).ToLowerInvariant();
            outList.Add(new CorpusDecode.Found(start, fed, bu.Wn, bu.Il.ToString(), bu.K, bu.Blocks, bu.Reason.ToString(),
                packed.Length, sha, goodFrames, Convert.ToHexString(framesHash.GetHashAndReset()).ToLowerInvariant(), corrected,
                deframer.CrcFailures, deframer.RsFailures));
            start = -1;
            goodFrames = corrected = 0;
            deframer = NewDeframer();
        }
        const int chunk = 96;
        void Feed(ReadOnlySpan<float> s)
        {
            for (int i = 0; i < s.Length; i += chunk)
            {
                int n = Math.Min(chunk, s.Length - i);
                foreach (CppBurst b in rx.Process(s.Slice(i, n))) Completed(b);
                fed += n;
                if (start < 0 && rx.Busy) start = fed;
            }
        }
        Feed(audio);
        Feed(new float[9600 * 3]);
        if (rx.Busy)
        {
            rx.Terminate();
            Feed(new float[chunk]);
        }
        return outList;
    }

    static byte[] Pack(byte[] bits)
    {
        var r = new byte[(bits.Length + 7) / 8];
        for (int i = 0; i < bits.Length; i++) if (bits[i] != 0) r[i >> 3] |= (byte)(0x80 >> (i & 7));
        return r;
    }
}

// Layer 4 rule (docs/dev/corpus.md), the one the C++ receiver is held to. Per burst: waveform,
// interleaver and K match exactly; end reason and block count match exactly unless the manifest burst
// has no good frames; the candidate has at most one good IL2P frame fewer than the manifest, and
// at most 2 percent fewer over the whole set; every frame it decodes passes its CRC; the payload
// hash matches exactly on error-free bursts only. CppCorpusDecode supplies the C++ receiver's
// CorpusDecode.Found values.
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
