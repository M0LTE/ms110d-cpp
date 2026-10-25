# Reference files

The C++ is tested stand-alone against files written by the C# reference, so a stage can be checked without .NET. The files come from `tools/M0LTE.Ms110d.RefGen` in [M0LTE/M0LTE.Ms110d](https://github.com/M0LTE/M0LTE.Ms110d); the format, the stages and the tolerances are documented in that repo's [docs/reference-files.md](https://github.com/M0LTE/M0LTE.Ms110d/blob/main/docs/reference-files.md). The standard set is 122 cases and about 40 MiB. It is generated, never committed.

## The pin

`reference/pin.txt` is the one place the C# reference version is named:

    nuget_version=0.2.1
    git_ref=v0.2.1
    git_sha=86abef9fab9cf42437e700e9d70652c3001f1afb

- `nuget_version` is the M0LTE.Ms110d NuGet package the .NET interop tests load (tests/interop reads it as an exact version).
- `git_ref` is the release tag of M0LTE.Ms110d the reference files are generated from, and `git_sha` the full commit that tag points at (so a moved tag is caught).

Both name the same release: tag `vX.Y.Z` and package `X.Y.Z`. Never update one without the other. `scripts/generate-reference.sh` enforces it: it fails unless `git_ref` is `v<nuget_version>` and the fetched tag resolves to `git_sha`. (0.1.0 predates RefGen, so this repo started on a commit SHA; 0.2.0 is the first release that contains it.)

## Generating

    scripts/generate-reference.sh [--force] [cache-dir]

It reads the pin, clones M0LTE.Ms110d at the pinned tag into `<cache-dir>/csharp`, runs RefGen `standard --verify` (which generates the set twice and checks the tree hashes match), then prints the tree hash. The files are in `<cache-dir>/ref`. The default cache-dir is `build/_reference`, which is git-ignored. A rerun with an unchanged pin does not regenerate, but it does recompute the tree hash of the cached files and compare it with the recorded one (cheap), and regenerates if they differ; `--force` regenerates regardless. It needs git and a .NET 10 SDK. The recorded commit is passed explicitly, because RefGen otherwise inspects the git checkout of the current directory.

The tree hash covers the manifests, which record the C# commit, so two commits give different hashes even if the audio is identical. The hash is for comparing a regeneration with the one you expect, not a value to check in.

CI runs the script on every PR and caches `build/_reference` keyed on `reference/pin.txt` and the script, so it only regenerates when the pin changes.

## When the C# changes

1. Release the C# change.
2. Bump `reference/pin.txt`: `nuget_version`, `git_ref` and `git_sha` together (see above).
3. Run `scripts/generate-reference.sh --force` and note the new tree hash.
4. Rerun every layer: the C++ unit tests against the new reference files, the interop tests against the new package, and the cross-decoding and real audio layers from docs/plan.md. A difference is either a C++ bug, a C# bug (fix the C# per the plan's deviation rules) or a named accepted deviation.
5. Put the new tree hash and what changed in the PR.

## The standard-pattern equivalents

The reference files above hold what the C# 0.2.1 transmits by default: the mini-probe shifted before the selected interleaver's boundary. The C++ transmitter sends the D.5.2.2 standard pattern (shifted only at the Long interleaver's boundary, issue 68), so the files' probe values and audio (stages `probe_values_cf32le` and `audio_f32le`; every other stage is the same in both patterns) are not what the C++ must reproduce. `scripts/generate-standard-reference.sh [--force] [cache-dir]` runs the C# modulator for every case, with the same settings and payload, with its internal standard-pattern switch set through reflection (`tools/csharp-vectors`, mode `standard-reference`), and writes `10_probes.cf32` and `13_audio.f32` for each case into `<cache-dir>/standard/<case>/`. The tests compare the C++ transmit output, the burst symbol stream's probe values and the layer 1 burst stage with those. `standard-pattern probes: the C# in standard mode differs from its default exactly where predicted` (tests/reference_tables_test.cpp) states the difference between the two sets: it rewrites both shift rules independently, checks every probe of the default files against the default rule and every probe of the standard files against the standard rule, requires the two to differ at exactly the probes the rules treat differently, and requires the audio to differ in exactly the cases that have such a probe. CI generates the files in the `reference files` job, after the reference files.

Reference dumps and files must be generated on the machine that runs the exact tests: the results depend on the CPU's libm paths (an older CPU without FMA takes other paths and fails exact comparisons that pass elsewhere). Details are coming in a separate story.

## Reading them from the tests

`tests/support/reference_files.hpp` is a small reader for format_version 2: the manifest (`key=value`), `index.txt`, and each stage's bits, symbols, `cf32` and `f32` files, with the file size checked against the manifest count. The directory comes from the environment variable `MS110D_REFERENCE_DIR`, which CMake sets for ctest from the cache variable of the same name (default `build/_reference/ref`, where the script puts them). Tests that need the files skip with a message when they are absent. Setting `MS110D_REQUIRE_REFERENCE=1` turns a missing directory into a failure instead; CI sets it in the `reference files` job, which generates the files and runs the tests whose names start with `reference `.

`tests/reference_tables_test.cpp` ties the C++ tables to the files: every case's manifest must agree with the mode and interleaver tables, and the table-driven stages (transcoding, mini-probes with the boundary rule, the EOM bits, the TLC and Fixed preamble sections) must reproduce the files exactly. The tables are also compared value by value with a dump of the C# package's own tables (`tests/data/csharp-tables-0.2.1.txt`, `tests/csharp_tables_test.cpp`); regenerate the dump when the pin moves, from the repo root:

    dotnet run -c Release --project tools/csharp-vectors -- tables > tests/data/csharp-tables-0.2.1.txt

`tests/framing_test.cpp` checks framing, the preamble and the whole burst symbol stream (with the probe marker 255 and the probe values) for all 122 cases, taking the interleaved bits from the files so it does not depend on the encoder. The waveform ID and downcount di-bits are also compared for every possible input with a second dump, regenerated the same way when the pin moves:

    dotnet run -c Release --project tools/csharp-vectors -- framing > tests/data/csharp-framing-0.2.1.txt

`tests/equaliser_test.cpp` compares the equaliser with the C# `Dfe` using a third dump of scripted scenarios, including real symbol streams from three reference cases, so it needs the reference directory to regenerate (docs/dev/equaliser.md):

    dotnet run -c Release --project tools/csharp-vectors -- dfe build/_reference/ref > tests/data/csharp-dfe-0.2.1.txt

`scripts/check-dumps.sh [reference-dir]` regenerates all six committed dumps (tables, framing, dfe, soft, `tests/csharp_vectors.hpp`, `tests/fec_vectors.inc`) and fails if any differs from the committed file; the CI reference job runs it.

The tool (`tools/csharp-vectors`) is pinned to the NuGet package `M0LTE.Ms110d` 0.2.1, so bump the version in its project file together with `reference/pin.txt`. The BPSK, QPSK and 8PSK transcode lines come from the C# modulator's private `Transcode`, evaluated for every input through reflection. Without arguments the same tool prints the scrambler and interleaver vectors for `tests/csharp_vectors.hpp` (see `tools/csharp-vectors/make_header.py`).

`tests/wn0_receiver_test.cpp` compares the WN 0 receiver with three more dumps from the same tool (`wn0`, `wn0-awgn`, `wn0-poor`), regenerated when the pin moves; the commands, and `scripts/generate-wn0-poor.sh` for the Poor channel corpus that is not committed, are in docs/dev/wn0-receiver.md.

The receive dumps (`rx`, `rx-awgn`, `rx-turbo`, `rx-options`) and the corpus run the C# receiver over the standard-pattern audio of the reference cases (`scripts/generate-rx-dumps.sh` calls `scripts/generate-standard-reference.sh` first), with the C# receiver's pattern evidence resolved in advance (docs/dev/dfe-receiver.md); the C++ receiver expects the standard pattern only (issue 68).

`tests/dfe_receiver_test.cpp` compares the equaliser receive path with two more (`rx`, `rx-awgn`), and `tests/dfe_turbo_test.cpp` the shipped receiver with `rx-turbo` and the `rx-corpus` of channel and standard probe pattern bursts; none is committed: `scripts/generate-rx-dumps.sh` makes them in the reference cache (docs/dev/dfe-receiver.md).

The layer 4 real-audio corpus follows the same pattern (pinned in `reference/corpus.txt`, fetched by `scripts/fetch-corpus.sh`); see [corpus.md](corpus.md).
