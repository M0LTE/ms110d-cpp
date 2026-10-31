// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later

using System.Security.Cryptography;
using System.Text.Json;
using Xunit;

namespace Ms110dInterop.Tests;

// Layer 4 (docs/dev/corpus.md): the corpus is self-consistent. Every curated burst file has the
// recorded hash and the pinned C# receiver identifies each burst as the manifest says. Two corpora
// are checked: the small set committed under tests/data/corpus (always present), and the full one
// fetched by scripts/fetch-corpus.sh (skipped when absent; MS110D_REQUIRE_CORPUS=1 makes absence
// a failure). The C++ receiver (through the C interface) is then held to the layer 4 rule against
// the pinned C# receiver's own decode of the same audio, on every burst. The audio is of
// standard-pattern transmissions (corpus-v2) and the manifests' "expected" results were recorded
// with the C# 0.3.0, the pin when the corpus was made; the comparison is made against the live C# so
// that a pin move (since then to 0.4.1) shows up as a reported difference (issue 68).
public class CorpusTests(ITestOutputHelper output)
{
    static string RepoRoot()
    {
        string pin = Environment.GetEnvironmentVariable("MS110D_PIN_FILE")
            ?? throw new InvalidOperationException("MS110D_PIN_FILE is not set; run these tests through ctest.");
        return Path.GetFullPath(Path.Combine(Path.GetDirectoryName(Path.GetFullPath(pin))!, ".."));
    }

    static string FetchedDir()
    {
        string dir = Environment.GetEnvironmentVariable("MS110D_CORPUS_DIR")
            ?? Path.Combine(RepoRoot(), "build", "_corpus", "current");
        if (!File.Exists(Path.Combine(dir, "manifest.json")))
        {
            Assert.False(Environment.GetEnvironmentVariable("MS110D_REQUIRE_CORPUS") == "1",
                $"no corpus in {dir}; run scripts/fetch-corpus.sh");
            Assert.Skip($"no corpus in {dir}; run scripts/fetch-corpus.sh to fetch it");
        }

        return dir;
    }

    static string CommittedDir() => Path.Combine(RepoRoot(), "tests", "data", "corpus");

    [Fact]
    public void CommittedSetMatchesItsManifest() => Check(CommittedDir(), minGoodFrames: 7);

    [Fact]
    public void FetchedCorpusMatchesItsManifest() => Check(FetchedDir(), minGoodFrames: 248);

    // The pinned C# receiver's decode of every recording, with the manifest's recorded result beside it: every
    // burst it finds, in order (the second and later get "#n" on the id). A recording can hold more bursts than
    // the manifest records when a newer pin finds them: since 0.4.0 the C# lets go of a burst when a new preamble
    // passes every gate while it is locked (its issue 10), so a dropout can split a recording in two.
    static List<(JsonElement Burst, List<Expect> Live)> LiveReference(string dir, JsonDocument doc)
    {
        var live = new List<(JsonElement, List<Expect>)>();
        foreach (JsonElement b in doc.RootElement.GetProperty("bursts").EnumerateArray())
        {
            string id = b.GetProperty("id").GetString()!;
            var got = CorpusDecode.Run(CorpusDecode.ReadWav(Path.Combine(dir, b.GetProperty("file").GetString()!)));
            Assert.True(got.Count > 0, $"{id}: the pinned C# receiver found no burst");
            live.Add((b, got.Select((f, i) => Expect.From(i == 0 ? id : $"{id}#{i}", f)).ToList()));
        }

        return live;
    }

    void Check(string dir, int minGoodFrames)
    {
        using JsonDocument doc = JsonDocument.Parse(File.ReadAllText(Path.Combine(dir, "manifest.json")));
        JsonElement bursts = doc.RootElement.GetProperty("bursts");
        Assert.True(bursts.GetArrayLength() > 0, "manifest has no bursts");
        // Every burst file has the recorded hash.
        foreach (JsonElement b in bursts.EnumerateArray())
        {
            string path = Path.Combine(dir, b.GetProperty("file").GetString()!);
            Assert.Equal(b.GetProperty("sha256").GetString(),
                Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(path))).ToLowerInvariant());
        }

        // The pinned C# receiver finds the same burst the manifest records: waveform, interleaver, constraint length.
        // The rest of the recorded result was made with the C# pin current when the corpus was made; how far
        // the pinned C# is from it (nothing, on an unchanged pin) is reported, not asserted.
        int same = 0, goodRecorded = 0, goodNow = 0;
        var countChanged = new List<string>();
        foreach (var (b, all) in LiveReference(dir, doc))
        {
            Expect now = all[0];
            if (all.Count != b.GetProperty("expected").GetArrayLength())
            {
                countChanged.Add($"{now.Id} ({all.Count} bursts, {b.GetProperty("expected").GetArrayLength()} recorded)");
            }
            JsonElement w = b.GetProperty("expected")[0];
            Assert.True(now.Wn == w.GetProperty("wn").GetInt32(), $"{now.Id}: waveform");
            Assert.True(now.Il == w.GetProperty("il").GetString(), $"{now.Id}: interleaver");
            Assert.True(now.K == w.GetProperty("k").GetInt32(), $"{now.Id}: constraint length");
            goodRecorded += w.GetProperty("good_frames").GetInt32();
            goodNow += now.GoodFrames;
            if (now.Reason == w.GetProperty("reason").GetString() && now.Blocks == w.GetProperty("blocks").GetInt32()
                && now.GoodFrames == w.GetProperty("good_frames").GetInt32()) same++;
        }

        string summary = $"layer 4, pinned C# receiver against the manifest: {bursts.GetArrayLength()} bursts, " +
            $"good frames {goodNow} against {goodRecorded} recorded, same end reason, block count and good frames on {same}, " +
            $"burst count not as recorded on {countChanged.Count}{(countChanged.Count > 0 ? ": " + string.Join(", ", countChanged) : "")}";
        output.WriteLine(summary);
        Console.WriteLine(summary);

        // A floor on the reference itself: the C++ is held to the live C# decode, so a pin that decoded worse would
        // silently lower the bar for the C++. Raise the floor deliberately if a pin decodes better.
        Assert.True(goodNow >= minGoodFrames,
            $"the pinned C# receiver decodes {goodNow} good frames on this corpus, below the floor of {minGoodFrames} (C# 0.4.1 on corpus-v2: 248, 0.3.0: 234; committed set: 7). " +
            "A pin that decodes worse lowers the bar the C++ is held to; investigate it. If a pin decodes better, raise the floor deliberately.");
    }

    // ---- the C++ receiver against the pinned C# receiver (layer 4) --------------------------

    [Fact]
    public void CppMeetsTheLayer4RuleOnTheCommittedSet() => CheckCpp(CommittedDir());

    [Fact]
    public void CppMeetsTheLayer4RuleOnTheFetchedCorpus() => CheckCpp(FetchedDir());

    void CheckCpp(string dir)
    {
        using JsonDocument doc = JsonDocument.Parse(File.ReadAllText(Path.Combine(dir, "manifest.json")));
        var pairs = new List<(Expect Want, CorpusDecode.Found Got)>();
        var extra = new List<string>();
        int wantFrames = 0, gotFrames = 0, sameEnd = 0, errorFree = 0, errorFreeSame = 0;
        foreach (var (b, wants) in LiveReference(dir, doc))
        {
            var got = CppCorpusDecode.Run(CorpusDecode.ReadWav(Path.Combine(dir, b.GetProperty("file").GetString()!)));
            // Every burst the C# found against the C++'s burst at the same place in order; a missing one counts as
            // no decode, and a C++ burst the C# did not find is a rule problem.
            for (int i = wants.Count; i < got.Count; i++)
            {
                extra.Add($"{wants[0].Id}: C++ burst {i} ({got[i].Reason}, {got[i].Blocks} blocks) has no C# burst");
            }
            for (int i = 0; i < wants.Count; i++)
            {
                Expect want = wants[i];
                CorpusDecode.Found first = i < got.Count
                    ? got[i]
                    : new CorpusDecode.Found(-1, -1, -1, "none", 0, 0, "None", 0, "", 0, "", 0, 0, 0);
                pairs.Add((want, first));
                wantFrames += want.GoodFrames;
                gotFrames += Math.Min(first.GoodFrames, want.GoodFrames);
                if (first.Reason == want.Reason && first.Blocks == want.Blocks) sameEnd++;
                if (want.ErrorFree)
                {
                    errorFree++;
                    if (first.Sha256 == want.Sha256) errorFreeSame++;
                }

                output.WriteLine($"{want.Id}: C# wn={want.Wn} {want.Reason} blocks={want.Blocks} good={want.GoodFrames}; " +
                    $"C++ bursts={got.Count} wn={first.Wn} {first.Reason} blocks={first.Blocks} good={first.GoodFrames} crc_failures={first.CrcFailures}");
            }
        }

        List<string> problems = [.. Layer4Rule.Evaluate(pairs), .. extra];
        string summary = $"layer 4, C++ receiver against the pinned C# receiver: {pairs.Count} bursts, good frames {gotFrames} of the C#'s {wantFrames}, " +
            $"same end and block count on {sameEnd}, error-free payloads equal {errorFreeSame} of {errorFree}, rule problems {problems.Count}";
        output.WriteLine(summary);
        Console.WriteLine(summary);
        Assert.True(problems.Count == 0, string.Join("; ", problems));
    }

    // ---- the rule itself, exercised on synthetic candidates --------------------------------

    static List<(Expect, CorpusDecode.Found)> Committed()
    {
        string dir = CommittedDir();
        var pairs = new List<(Expect, CorpusDecode.Found)>();
        using JsonDocument doc = JsonDocument.Parse(File.ReadAllText(Path.Combine(dir, "manifest.json")));
        foreach (JsonElement b in doc.RootElement.GetProperty("bursts").EnumerateArray())
        {
            var found = CorpusDecode.Run(CorpusDecode.ReadWav(Path.Combine(dir, b.GetProperty("file").GetString()!))).Single();
            pairs.Add((Expect.From(b.GetProperty("id").GetString()!, found), found));
        }

        return pairs;
    }

    [Fact]
    public void RuleAcceptsTheReferenceItself() => Assert.Empty(Layer4Rule.Evaluate(Committed()));

    [Fact]
    public void RuleRejectsWhatItShould()
    {
        var pairs = Committed();
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
// interleaver and K match exactly; end reason and block count match exactly unless the reference burst
// has no good frames; the candidate has at most one good IL2P frame fewer than the reference, and
// at most 2 percent fewer over the whole set; every frame it decodes passes its CRC; the payload
// hash matches exactly on error-free bursts only. The reference is the pinned C# receiver's decode of
// the same audio (CorpusDecode.Run); CppCorpusDecode supplies the C++ receiver's Found values.
sealed record Expect(string Id, int Wn, string Il, int K, string Reason, int Blocks, int GoodFrames, bool ErrorFree, string Sha256)
{
    public static Expect From(string id, CorpusDecode.Found f) =>
        new(id, f.Wn, f.Il, f.K, f.Reason, f.Blocks, f.GoodFrames, f.ErrorFree, f.Sha256);
}

static class Layer4Rule
{
    public static List<string> Evaluate(IEnumerable<(Expect Want, CorpusDecode.Found Got)> pairs)
    {
        var problems = new List<string>();
        int wantTotal = 0, shortTotal = 0;
        foreach (var (w, got) in pairs)
        {
            void Differs(string what) => problems.Add($"{w.Id}: {what}");
            if (got.Wn != w.Wn) Differs("waveform");
            if (got.Il != w.Il) Differs("interleaver");
            if (got.K != w.K) Differs("constraint length");
            if (w.GoodFrames > 0)
            {
                if (got.Reason != w.Reason) Differs("end reason");
                if (got.Blocks != w.Blocks) Differs("block count");
            }

            int missing = Math.Max(0, w.GoodFrames - got.GoodFrames);
            if (missing > 1) Differs($"{missing} good frames short (at most 1 allowed)");
            wantTotal += w.GoodFrames;
            shortTotal += missing;
            if (!got.AllFramesPassCrc) Differs("a decoded frame failed its CRC");
            if (w.ErrorFree && got.Sha256 != w.Sha256)
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
