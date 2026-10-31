# Keeping parity with the C#

The C# package M0LTE.Ms110d is the reference (docs/plan.md). Two light checks say when it has moved on, and where the C++ now differs.

## The weekly check

`.github/workflows/parity.yml` runs every Monday (and by hand, with an optional C# version) on a GitHub-hosted runner, in about an hour. It runs `scripts/parity-check.sh`:

- **Pin current** (`reference/pin.txt` names the newest C# release on NuGet): only the random streams below, with fresh seeds (the date). Nothing to report unless they differ.
- **Newer C# release**: in the job's own checkout it moves the pin to that release, regenerates every C# file the tests read (the reference set, the committed dumps under their current names, the receive dumps, the WN 0 Poor channel corpus), builds the C++, and runs the C++ tests (masks aside), the acquisition scenarios, the real-audio corpus (layer 4) and the random streams against it. Nothing is committed.

The report goes to the job summary and, when there is something to do, to an issue labelled `parity` (one per C# version, commented on by later runs):

- **No gap**: the C++ gives the new C#'s results on every test run. Bump the pin as in docs/dev/reference-files.md.
- **Gap**: the C# changes since the pin (its commits and the source files they touch), a table of the C++ tests by stage with how many now fail, the failing tests, and the interop results. That says where to port; how is still a person's or an agent's job.
- **The check failed** (a new C# release can break the tooling itself): an issue saying so, with the run to look at.

Not run: the performance masks (`masks.yml`, on every PR touching the receiver) and layer 3. Locally, run it in a throwaway worktree, since it rewrites the checkout: `scripts/parity-check.sh --modify-checkout build/parity.md`.

## Random streams

`tests/interop/DifferentialTests.cs` builds random streams of one to three bursts (any waveform, ultra-short or short interleaver, either code, random length, a fifth cut short, a sixth without an EOM, random gaps), puts them through a random HF channel (AWGN, Good, Moderate or Poor) with noise from 3 to 20 dB, a carrier offset up to 40 Hz and a level from 0.05 to 2, and requires the same bursts from the pinned C# receiver and the C++ receiver: waveform, interleaver, K, end reason, block count and payload. It catches a behaviour difference no hand-written test happens to reach, in either direction: a C# change, or a C++ change that broke parity.

It runs 16 streams in every interop run (PR CI), and 300 in the weekly check. `MS110D_DIFF_STREAMS` and `MS110D_DIFF_SEED` set the count and the first seed; a failure names the seed, so `MS110D_DIFF_STREAMS=1 MS110D_DIFF_SEED=<seed>` replays it. Measured: 300 streams (484 bursts) equal on both receivers, about 20 CPU minutes; a level target changed from 0.5 to 0.45 in the C++ made 6 of 32 streams differ.
