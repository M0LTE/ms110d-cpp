# Layer 4 corpus: real off-air audio

Real recordings of MS110D bursts, with what the pinned C# receiver makes of each, so the C++ receiver can be held to "decodes every frame the C# does, give or take documented deviations" (docs/plan.md, layer 4; story 29). The recordings are public on-air amateur transmissions, and may be published.

## Where the recordings come from

A shadow receiver listens to a web SDR on the 40 m mailcast frequency and records every hourly daylight HF bulletin slot as 12 kHz mono 16-bit audio (the raw capture of the modem software, not the receiver's own demodulated feed), a window from 2 minutes before to 13 minutes after the hour. Each slot is a directory `<UTC start>/` holding `audio.wav`, `decode.log` and `meta.json` (slot time, audio span, gaps). A slot is about 20 MB and holds 10 to 25 bursts. Everything about the recorder (host, access) stays out of this repo; `curate.py` reaches it only through two environment variables.

The raw audio is equivalent to, not bit-identical with, what the live receiver heard.

## What is in the corpus

For each burst the C# receiver can decode to the end-of-message marker (end reason Eom):

- `bursts/<slot>-<nn>.wav`: 9600 Hz mono 16-bit PCM, the burst with up to 6 s before it (never into the previous burst) and 1.5 s after. 9600 Hz is the C# receiver's input rate.
- `manifest.json`: per burst the id, UTC start of the file, waveform, interleaver, constraint length, an SNR estimate, duration, the SHA-256 of the file, and `expected`, the C# result on that exact file: block count, end reason, payload length in bytes and the SHA-256 of the payload (bits packed MSB first, zero padded), then the IL2P results below.

Bursts that do not reach an EOM (false locks on noise, signal lost, signal absent) are dropped; they are lock attempts on noise, not frames. The current corpus, corpus-v2, is 77 bursts and 1722 s of audio from 23 of the 24 slots recorded from 08:00 to 16:00 UTC on 2026-10-27, 28 and 29 (the first slots after GB7RDG switched to the standard mini-probe pattern; the 24th happened to contribute no burst to the selection). GB7RDG sent only WN 4 (1200 bps, Short interleaver, K 7; 39 bursts) and WN 3 (600 bps, Short, K 7; 38 bursts) in that time, so those are the only modes in it. SNR estimates run from +1 to +18 dB. 30 bursts are error-free, 11 have no good frame (weak signals), 2 overlap a stream dropout, and the C# gets 234 good frames over the corpus. The whole curation of those slots gave 446 bursts and 111 MB; the corpus is a deterministic selection of 77 from them (below). More modes and weaker signals come as more slots are recorded; refresh as below.

The SNR estimate is crude: the burst's power over the quiet floor of the same slot (median of the quietest fifth of 0.5 s windows), in the audio passband, not a 3 kHz figure. Use it to rank bursts, not to quote.

### IL2P results and the `error_free` flag

Mailcast bursts carry IL2P+CRC frames back to back. The expected result for each burst therefore includes what the IL2P deframer of the pinned M0LTE.Il2p makes of the decoded blocks, fed as the modem does: `good_frames` (frames that passed Reed-Solomon and the CRC), `frames_sha256` (a hash of those frames, each as a 4-byte little-endian length then its bytes, in order), `corrected` (symbols Reed-Solomon corrected) and the counts of dropped frames, `crc_failures` and `rs_failures`. `error_free` is set when there is at least one good frame and nothing was dropped.

The C# receiver is sensitive on marginal bursts: noise at -100 dBFS on the input changed the payload of some bursts by hundreds of bits. The differing bits sit in frames that fail their CRC anyway, which is why the layer 4 rule below compares good frames, and the raw payload only where nothing failed. In corpus-v1 6 of 31 bursts are error-free, 7 have no good frame at all (weak WN 3 bursts, and a dropout), and 99 good frames are spread over the rest.

A burst flagged `dropout` overlaps a stream dropout or fall-behind reported in the recorder's log, so audio may be missing from it. One such burst is kept on purpose, as the dropout case.

The live comparison of both decoders over whole slots is docs/dev/shadow.md.

## History: corpus-v1 and the pattern switch (issue 68)

corpus-v1 (31 bursts, 14:00 to 16:00 UTC on 2026-10-24) was recorded from C# 0.2.1 transmitters, which shift the mini-probe at the selected interleaver's boundary rather than at the Long interleaver's (the standard pattern), and its expected results were made with the C# 0.2.1 receiver, which accepted both. The C# 0.3.0 and the C++ receivers expect the standard pattern only and decoded that audio a little worse. corpus-v2 replaces it: recorded after GB7RDG switched to the standard pattern, with the expected results made by the pinned C# 0.3.0. On an unchanged pin the live C# decode therefore equals the manifest on every burst (the first check below prints this), and the layer 4 rule is applied against the live C# decode, so a pin move shows up as a reported difference rather than a failure. The corpus-v1 release stays published but nothing refers to it.

## The layer 4 rule

How the C++ receiver is held to the C# on this corpus (decided with the maintainer; `Layer4Rule` in `tests/interop/CorpusTests.cs` implements it, checks that the C# meets it against its own manifest, and holds the C++ receiver to it):

- Waveform, interleaver and constraint length match exactly, on every burst.
- End reason and block count match exactly, except on bursts the manifest says have no good frames.
- At most one good IL2P frame short per burst, and at most 2 percent short over the corpus.
- Every frame the C++ decodes must pass its CRC.
- The payload matches exactly only on error-free bursts.

Anything beyond the rule is a named deviation with an issue (docs/dev/deviations.md).

## The resampler

12 kHz to 9600 Hz is exactly 4/5. `curate.py` zero-stuffs by 4, low-passes with a 801-tap Kaiser (beta 8) windowed-sinc FIR with a 4400 Hz cutoff at 48 kHz (flat across the 3 kHz signal band, 4800 Hz output Nyquist well into the stop band), and keeps every 5th sample, evaluated as a polyphase sum in float64 and aligned to the group delay. The filter is computed from a formula in numpy, with no resampling library, so the same input gives the same output on any machine; the output is rounded to int16. The method is also written into the manifest.

## Selection

`curate.py` keeps every burst the C# decodes to an EOM, which for 24 slots is 446 bursts and 111 MB, more than a corpus needs. `scripts/corpus/select.py` cuts a full curation down: it keeps every burst with no good frame or overlapping a dropout (13), then, for each waveform and for error-free and not error-free separately, bursts spaced evenly through the list sorted by SNR estimate, so the SNR range and the marginal bursts are covered. The entries and audio are unchanged, so ids stay the slot and index in the full run. corpus-v2 was made from the 24 slots `20261027T085800Z` to `20261029T155800Z` (every slot recorded from 2026-10-27, the first after the switch to the standard pattern) with:

    scripts/corpus/curate.py --pull --out FULL --only 20261027T085800Z 20261027T095800Z 20261027T105800Z 20261027T115800Z 20261027T125800Z 20261027T135800Z 20261027T145800Z 20261027T155800Z 20261028T085800Z 20261028T095800Z 20261028T105800Z 20261028T115800Z 20261028T125800Z 20261028T135800Z 20261028T145800Z 20261028T155800Z 20261029T085800Z 20261029T095800Z 20261029T105800Z 20261029T115800Z 20261029T125800Z 20261029T135800Z 20261029T145800Z 20261029T155800Z
    scripts/corpus/select.py FULL OUT 14 18
    scripts/corpus/package.sh corpus-v2 OUT

that is 14 error-free and 18 other bursts per waveform plus the 13 always kept, 77 in all. The committed set was then cut from `OUT` with the two ids above.

## Curating

    scripts/corpus/curate.py --pull --committed tests/data/corpus

Needs python3 with numpy and a .NET 10 SDK. `--pull` copies slots from the recorder first, using `CORPUS_REMOTE_CMD` (a command prefix that runs a command there, such as an ssh invocation) and `CORPUS_REMOTE_SLOTS` (the slots directory there); without it, `--slots-dir` (default `~/.cache/ms110d-corpus/slots`, or `$MS110D_CORPUS_CACHE/slots`) is used as it is. `--only` limits it to named slots. Work and output go under the cache directory (`curated/` holds the corpus), outside the tree. The steps per slot:

1. Resample the slot to 9600 Hz.
2. Run the C# receiver over it (`tools/csharp-vectors`, mode `corpus find`) to locate each burst. The receiver reports a burst some seconds after it began, so the cut starts up to 6 s earlier, never before the previous burst's end (the receiver cannot lock again earlier); shorter margins are tried if the cut does not decode to a single complete burst.
3. Write the cut as 16-bit WAV and decode it (`corpus decode`) for the expected results. Bursts the cut does not decode to a complete one (an EOM) are dropped. Bursts overlapping a stream dropout in the recorder's log are flagged.
4. Write `manifest.json`. With `--committed DIR` also write the repo set (below).

A burst is found by the C# receiver's own lock, so a burst the C# misses is not in the corpus; that is a limit of a corpus made from the reference.

## The committed set

`tests/data/corpus/` holds 2 bursts (about 0.7 MB), for quick tests that need no download: `20261028T095800Z-10`, a strong WN 4 burst that is error-free (6 frames), and `20261027T095800Z-03`, a WN 3 burst at about +5 dB with one good frame. It is the same format as the full corpus and is chosen by `--committed` (ids by `--committed-ids`, under a byte budget). It earns its place by letting CI check the format, the hashes and the C# results on every PR. Keep it small; bulk goes to the release.

## Publishing

    scripts/corpus/package.sh corpus-v2

writes `ms110d-corpus-v2.tar.gz` (and a `.sha256`) under the cache's `release/`, byte-for-byte reproducible from the same curated files, and prints its size, burst count and hash. corpus-v2 is 77 bursts, 25.6 MB (the expected results were produced with C# 0.3.0, the pin current when the tarball was made). Then, by hand:

1. Put the tag and hash in `reference/corpus.txt` (`corpus_tag`, `tarball_sha256`).
2. Create the GitHub (pre-)release with that tag and upload the tarball as an asset.
3. Check `scripts/fetch-corpus.sh --force` downloads it and verifies.

Nothing in the scripts creates a release or uploads. Corpus tags are `corpus-vN`, independent of the code versions; a new corpus is a new tag, and the old assets stay.

## Fetching and using

    scripts/fetch-corpus.sh [--force] [--from FILE] [cache-dir]

reads `reference/corpus.txt`, downloads the asset, checks the tarball hash against the pin, unpacks it to `<cache-dir>/current` (default `build/_corpus`, git-ignored) and checks every burst file against the manifest. A rerun with an unchanged pin only re-verifies. `--from FILE` uses a local tarball instead of downloading.

`tests/interop/CorpusTests.cs` is the self-consistency test: for the committed set, and for the fetched corpus when it is present (`MS110D_CORPUS_DIR`, default `build/_corpus/current`; it skips cleanly when absent, and `MS110D_REQUIRE_CORPUS=1` makes absence a failure), it checks each file against its hash and the pinned C# decode against the manifest: waveform, interleaver, K, block count, end reason, good frames, their hash and the symbols corrected for every burst, and the raw payload hash for error-free bursts only (elsewhere it covers frames that fail their CRC, and differed between machines), and then that the C# meets the layer 4 rule. It shares `tools/csharp-vectors/CorpusDecode.cs` with the tool, so the expected results and the check cannot drift apart. Run it with `dotnet run --project tests/interop -c Release -- -class Ms110dInterop.Tests.CorpusTests` (see dotnet-interop.md for the environment).

The C++ receiver is held to the layer 4 rule against the same manifests (`CppMeetsTheLayer4RuleOnTheCommittedSet`, `CppMeetsTheLayer4RuleOnTheFetchedCorpus`): `CppCorpusDecode` feeds it through the C interface exactly as `CorpusDecode.Run` feeds the C# (chunks of 96, three seconds of silence, then a terminate if a burst is open) with the same IL2P deframer on its blocks, and the test prints a per-burst line and a summary. The CI reference job runs it on the fetched corpus (must not skip). Measured on corpus-v2: 77 bursts, all 234 good frames the C# has, the same end and block count on every burst, the payload equal on all 30 error-free bursts; no rule problem and no deviation. The committed set: 2 bursts, 7 of 7 frames. The C# floor in `Check` is the C#'s own good frame count (234 on the fetched corpus, 7 on the committed set); raise it deliberately if a pin decodes better.

## Refreshing

More slots accumulate on the recorder. To refresh: run `curate.py --pull`, review the stats (modes, SNRs, error-free count), run `package.sh` with the next tag, update `reference/corpus.txt`, publish the release, and put the new stats in the PR. When the C# pin moves (docs/dev/reference-files.md) rerun `curate.py` on the same slots: the audio is unchanged, the expected results are what changes, and the diff of the manifest is the record of what the new C# does differently.
