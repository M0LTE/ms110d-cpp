# Reference files

The C++ is tested stand-alone against files written by the C# reference, so a stage can be checked without .NET. The files come from `tools/M0LTE.Ms110d.RefGen` in [M0LTE/M0LTE.Ms110d](https://github.com/M0LTE/M0LTE.Ms110d); the format, the stages and the tolerances are documented in that repo's [docs/reference-files.md](https://github.com/M0LTE/M0LTE.Ms110d/blob/main/docs/reference-files.md). The standard set is 122 cases and about 40 MiB. It is generated, never committed.

## The pin

`reference/pin.txt` is the one place the C# reference version is named:

    nuget_version=0.2.0
    git_ref=v0.2.0
    git_sha=c7eeb7456a9bf441f25614542235d1221c7b3948

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

## Reading them from the tests

`tests/support/reference_files.hpp` is a small reader for format_version 2: the manifest (`key=value`), `index.txt`, and each stage's bits, symbols, `cf32` and `f32` files, with the file size checked against the manifest count. The directory comes from the environment variable `MS110D_REFERENCE_DIR`, which CMake sets for ctest from the cache variable of the same name (default `build/_reference/ref`, where the script puts them). Tests that need the files skip with a message when they are absent. Setting `MS110D_REQUIRE_REFERENCE=1` turns a missing directory into a failure instead; CI sets it in the `reference files` job, which generates the files and runs the tests whose names start with `reference `.

`tests/reference_tables_test.cpp` ties the C++ tables to the files: every case's manifest must agree with the mode and interleaver tables, and the table-driven stages (transcoding, mini-probes with the boundary rule, the EOM bits, the TLC and Fixed preamble sections) must reproduce the files exactly. The tables are also compared value by value with a dump of the C# package's own tables (`tests/data/csharp-tables-0.2.0.txt`, `tests/csharp_tables_test.cpp`); regenerate the dump when the pin moves, from the repo root:

    dotnet run -c Release --project tools/csharp-vectors -- tables > tests/data/csharp-tables-0.2.0.txt

`tests/framing_test.cpp` checks framing, the preamble and the whole burst symbol stream (with the probe marker 255 and the probe values) for all 122 cases, taking the interleaved bits from the files so it does not depend on the encoder. The waveform ID and downcount di-bits are also compared for every possible input with a second dump, regenerated the same way when the pin moves:

    dotnet run -c Release --project tools/csharp-vectors -- framing > tests/data/csharp-framing-0.2.0.txt

`tests/equaliser_test.cpp` compares the equaliser with the C# `Dfe` using a third dump of scripted scenarios, including real symbol streams from three reference cases, so it needs the reference directory to regenerate (docs/dev/equaliser.md):

    dotnet run -c Release --project tools/csharp-vectors -- dfe build/_reference/ref > tests/data/csharp-dfe-0.2.0.txt

`scripts/check-dumps.sh [reference-dir]` regenerates all six committed dumps (tables, framing, dfe, soft, `tests/csharp_vectors.hpp`, `tests/fec_vectors.inc`) and fails if any differs from the committed file; the CI reference job runs it.

The tool (`tools/csharp-vectors`) is pinned to the NuGet package `M0LTE.Ms110d` 0.2.0, so bump the version in its project file together with `reference/pin.txt`. The BPSK, QPSK and 8PSK transcode lines come from the C# modulator's private `Transcode`, evaluated for every input through reflection. Without arguments the same tool prints the scrambler and interleaver vectors for `tests/csharp_vectors.hpp` (see `tools/csharp-vectors/make_header.py`).
