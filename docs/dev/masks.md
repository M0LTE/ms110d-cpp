# Performance masks

Issue 30, under epic 23. The standard's D.6 performance masks, run by the C++ tests alone (no .NET at test time) on channel audio made by the C# reference.

Code: `tools/csharp-vectors/MaskDump.cs` (the audio and the C# counts), `scripts/generate-masks.sh`, `tests/mask_test.cpp`, `tests/support/mask_driver.*` (the public `Receiver`), `scripts/mask-summary.sh` (the results table). The CI workflow is `masks.yml` (pushes to main, pull requests touching the receiver, the mask code or its tooling, and by hand; not in `ci.yml`, so `release.yml` does not run it); generation is one job and the double and single decodes are two parallel jobs; the full counts are `masks-long.yml`, by hand.

## What is tested

Table D-LXIV, 3 kHz, the AWGN and the Poor channel SNR for every waveform the standard gives a mask for and the modem has: WN 0 to 8 and 13 (WN 9 to 12 are not in the modem). Poor is the D.6.1 channel (2 equal Rayleigh paths, 2 ms, 1 Hz fading), `WattersonChannel.Poor` of M0LTE.HfChannel. Table D-LXV, 3 kHz: the three path static channel for WID 2 (0, 3 and 9 ms, equal power) is WN 2; the rows for WID 10 and 12 are waveforms the modem does not have yet. The standard's D-LXV SNR column is "not yet standardized", so the SNR is the C# repo's own house bar for this rig, 9 dB (`Static_Wid2_Gate`, its comments say why); it is a comparison point, not a standard mask. Conditions as the C# tests (D.6.1): the Long interleaver, constraint length 7, a 20 super-frame preamble, SNR in a 3 kHz noise bandwidth, bursts of about 90 s with the EOM, random payload.

Four more points, `below-*`, sit 3 to 4 dB under the mask SNR (WN 2 and 6 on AWGN, WN 3 and 5 on Poor). They are not masks. They exist because at the mask SNRs both decoders make no errors and "the same as the C#" then says little; below the mask the C# makes hundreds of errors and the comparison is exercised (the test insists the C# makes some).

## How

1. `scripts/generate-masks.sh` runs `tools/csharp-vectors -- masks write <profile>`: per burst it makes a payload, modulates it with the C# transmitter, passes it through the C# channel, quantises to 16 bit (1.0 = 4096, full scale +-8; the generator fails if a sample would clip), and runs the C# 0.4.1 receiver on the quantised audio for its error count, counted exactly as the C# mask test counts (a burst that does not decode counts every payload bit; bits missing or beyond the transmitted stream count too). Output per point: `<id>.txt` (what was made and the C# result per burst), `<id>.i16le` (audio), `<id>.bits` (the payloads). Seeds depend on the burst's index only, so the files do not depend on the thread count. Never committed; `masks.yml` generates them and caches them on the tool sources, the pin and the .NET SDK version.
2. `ms110d_mask_tests` decodes the same audio with the C++ detector and receiver and counts errors the same way. One ctest entry per point (`masks...`), so they run in parallel. They skip without the files and fail when `MS110D_REQUIRE_REFERENCE` is set. `MS110D_MASK_DIR` overrides where the files are; `MS110D_MASK_LOG` appends a CSV row per point.

## What is checked

- (a) The C# repo's accept rule, as the C# applies it: every burst acquired, and BER at most 1e-5, directly with 30 or more errors, otherwise by the 97.5 % Poisson upper bound on the count. Where the rule holds for the C# on this audio it must hold for the C++. At the pull request counts it can only hold where there are enough bits for zero errors to decide it (about 370,000), so it is decided for WN 3 and up and left undecided for WN 0 to 2, which the long profile decides.
- (b) The C++ error count against the C#'s on the same audio, with no more acquisition failures. In the double build the C++ is the C#'s arithmetic step for step, so the counts must be equal, burst by burst (a loose tolerance let a deliberately weakened turbo stage through). In the single build they differ by at most three standard deviations of a Poisson count at the larger of the two (at least 2).

## Bit counts

| Profile | Bits per point | Simulated audio per point | Used by |
| --- | --- | --- | --- |
| `ci` | 370,000 (the fewest at which zero errors decide the 1e-5 mask), at least 640 s, at most 1300 s | 8 to 14 bursts of 89 s | `masks.yml` |
| `long` | 3,000,000 (the C# repo's budget; 6,000,000 for Poor WN2, 5 and 6 as there), at least 600 s on Poor | up to 40000 s for WN 0 | `masks-long.yml`, by hand |

The cap exists because WN 0 is 75 bit/s: 370,000 bits is 4900 s of audio. At the cap WN 0 has about 88,000 bits and WN 1 about 172,000, and WN 2 (344,000) is just under; those three compare with the C# at the pull request counts and the long run decides them. The Poor fading points get at least 640 s, above the 600 s the C# asks for, so they see enough fades.

The audio is about 350 MiB and the decode about 5 to 10 minutes on four cores per precision (WN 7 and 8 dominate: the turbo stage and the MFB decoder).

## Results

See the pull request for the table; the CI job puts it in the job summary on every run. Columns: the C# result, the C++ result in double and in single precision, and whether the mask rule holds in each.

## Long runs

Actions, "Performance masks (long)", Run workflow. Optionally name points (`awgn-wn3,poor-wn5`). Each point is a job: it makes its audio (hours for WN 0, 1 and 2), decodes it in both precisions and uploads the CSV; a last job puts the table in the summary. Locally: `scripts/generate-masks.sh --profile long build/_reference awgn-wn3` then `MS110D_MASK_DIR=build/_reference/masks-long ctest -R "mask awgn-wn3$"`.
