# Layer 4 corpus: real off-air audio

Real recordings of MS110D bursts, with what the pinned C# receiver makes of each, so the C++ receiver can be held to "decodes every frame the C# does, give or take documented deviations" (docs/plan.md, layer 4; story 29). The recordings are public on-air amateur transmissions, and may be published.

## Where the recordings come from

A shadow receiver listens to a web SDR on the 40 m mailcast frequency and records every hourly daylight HF bulletin slot as 12 kHz mono 16-bit audio (the raw capture of the modem software, not the receiver's own demodulated feed), a window from 2 minutes before to 13 minutes after the hour. Each slot is a directory `<UTC start>/` holding `audio.wav`, `decode.log` and `meta.json` (slot time, audio span, gaps). A slot is about 20 MB and holds 10 to 25 bursts. Everything about the recorder (host, access) stays out of this repo; `curate.py` reaches it only through two environment variables.

The raw audio is equivalent to, not bit-identical with, what the live receiver heard.

## What is in the corpus

For each burst the C# receiver can decode to the end-of-message marker (end reason Eom):

- `bursts/<slot>-<nn>.wav`: 9600 Hz mono 16-bit PCM, the burst with up to 6 s before it (never into the previous burst) and 1.5 s after. 9600 Hz is the C# receiver's input rate.
- `manifest.json`: per burst the id, UTC start of the file, waveform, interleaver, constraint length, an SNR estimate, duration, the SHA-256 of the file, and `expected`, the C# result on that exact file: block count, end reason, payload length in bytes and the SHA-256 of the payload (bits packed MSB first, zero padded).

Bursts that do not reach an EOM (false locks on noise, signal lost, signal absent) are dropped; they are lock attempts on noise, not frames. The first corpus (three slots, 14:00 to 16:00 UTC on 2026-10-24) is 31 bursts and 670 s of audio: WN 4 (1200 bps, Short interleaver) and WN 3 (600 bps, Short), SNR estimates from +1 to +20 dB. More modes and weaker signals come as more slots are recorded; refresh as below.

The SNR estimate is crude: the burst's power over the quiet floor of the same slot (median of the quietest fifth of 0.5 s windows), in the audio passband, not a 3 kHz figure. Use it to rank bursts, not to quote.

### The `stable` flag

The C# receiver is sensitive on marginal bursts: adding noise at -100 dBFS to the input changed the payload of some bursts by hundreds of bits. Decoding a burst inside its slot, cut out, and cut out as float can all give different payloads for the same burst. So each manifest entry records `matches_slot_decode` (the cut decodes as it did in the slot), `float_input_agrees` (the unquantised audio decodes the same) and `stable` (both). In the first corpus 7 of 31 bursts are stable. Only stable bursts carry a payload hash a port can be held to bit for bit; for the rest hold the C++ to block count, end reason and waveform, and judge payload differences by bit errors, not equality. This is a finding about the C# for the C++ comparison to keep in mind, not a defect of the corpus. Quantising to 16 bits is not itself a problem: it is far below the sensitivity above (the float check is how this was measured), and the source audio is 16-bit already.

## The resampler

12 kHz to 9600 Hz is exactly 4/5. `curate.py` zero-stuffs by 4, low-passes with a 801-tap Kaiser (beta 8) windowed-sinc FIR with a 4400 Hz cutoff at 48 kHz (flat across the 3 kHz signal band, 4800 Hz output Nyquist well into the stop band), and keeps every 5th sample, evaluated as a polyphase sum in float64 and aligned to the group delay. The filter is computed from a formula in numpy, with no resampling library, so the same input gives the same output on any machine; the output is rounded to int16. The method is also written into the manifest.

## Curating

    scripts/corpus/curate.py --pull --committed tests/data/corpus

Needs python3 with numpy and a .NET 10 SDK. `--pull` copies slots from the recorder first, using `CORPUS_REMOTE_CMD` (a command prefix that runs a command there, such as an ssh invocation) and `CORPUS_REMOTE_SLOTS` (the slots directory there); without it, `--slots-dir` (default `~/.cache/ms110d-corpus/slots`, or `$MS110D_CORPUS_CACHE/slots`) is used as it is. `--only` limits it to named slots. Work and output go under the cache directory (`curated/` holds the corpus), outside the tree. The steps per slot:

1. Resample the slot to 9600 Hz.
2. Run the C# receiver over it (`tools/csharp-vectors`, mode `corpus find`) to locate each burst. The receiver reports a burst some seconds after it began, so the cut starts up to 6 s earlier, never before the previous burst's end (the receiver cannot lock again earlier); shorter margins are tried if the cut does not decode to a single complete burst.
3. Write the cut as 16-bit WAV and decode it (`corpus decode`) for the expected results; also decode the float version and the slot's own result to set `stable`.
4. Write `manifest.json`. With `--committed DIR` also write the repo set (below).

A burst is found by the C# receiver's own lock, so a burst the C# misses is not in the corpus; that is a limit of a corpus made from the reference.

## The committed set

`tests/data/corpus/` holds 4 bursts (about 1.6 MB): per waveform the weakest and the strongest SNR estimate (stable bursts when the waveform has any), for quick tests that need no download. It is the same format as the full corpus and is chosen by `--committed`, up to a byte budget. It earns its place by letting CI check the format, the hashes and the C# results on every PR. Keep it small; bulk goes to the release.

## Publishing

    scripts/corpus/package.sh corpus-v1

writes `ms110d-corpus-v1.tar.gz` (and a `.sha256`) under the cache's `release/`, byte-for-byte reproducible from the same curated files, and prints its size, burst count and hash. The first one is 31 bursts, about 9.9 MB. Then, by hand:

1. Put the tag and hash in `reference/corpus.txt` (`corpus_tag`, `tarball_sha256`).
2. Create the GitHub release with that tag and upload the tarball as an asset.
3. Check `scripts/fetch-corpus.sh --force` downloads it and verifies.

Nothing in the scripts creates a release or uploads. Corpus tags are `corpus-vN`, independent of the code versions; a new corpus is a new tag, and the old assets stay.

## Fetching and using

    scripts/fetch-corpus.sh [--force] [--from FILE] [cache-dir]

reads `reference/corpus.txt`, downloads the asset, checks the tarball hash against the pin, unpacks it to `<cache-dir>/current` (default `build/_corpus`, git-ignored) and checks every burst file against the manifest. A rerun with an unchanged pin only re-verifies. `--from FILE` uses a local tarball instead of downloading.

`tests/interop/CorpusTests.cs` is the self-consistency test: for the committed set, and for the fetched corpus when it is present (`MS110D_CORPUS_DIR`, default `build/_corpus/current`; it skips cleanly when absent, and `MS110D_REQUIRE_CORPUS=1` makes absence a failure), it checks each file against its hash and the pinned C# decode against the manifest: block count, end reason, waveform and payload length for every burst, and the payload hash for stable ones. It shares `tools/csharp-vectors/CorpusDecode.cs` with the tool, so the expected results and the check cannot drift apart. Run it with `dotnet run --project tests/interop -c Release -- -class Ms110dInterop.Tests.CorpusTests` (see dotnet-interop.md for the environment).

The C++ comparison against the same manifests is added with the C++ receiver.

## Refreshing

More slots accumulate on the recorder. To refresh: run `curate.py --pull`, review the stats (modes, SNRs, stable count), run `package.sh` with the next tag, update `reference/corpus.txt`, publish the release, and put the new stats in the PR. When the C# pin moves (docs/dev/reference-files.md) rerun `curate.py` on the same slots: the audio is unchanged, the expected results are what changes, and the diff of the manifest is the record of what the new C# does differently.
