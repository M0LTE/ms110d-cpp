# Layer 3: cross-decoding harness

Issue 28, under epic 23. Code: `tests/interop/Layer3/`. It sends seeded bursts from a transmitter through the C# HF channel (`M0LTE.HfChannel`) at a ladder of SNRs to a receiver, and records frame and bit errors. The question it answers: do the pairings (C# to C#, C++ to C#, and later C# to C++ and C++ to C++) need the same SNR to reach the same frame loss, to within 0.5 dB?

## What is measured

- A frame is one burst carrying exactly one interleaver block (payload = block size - 32 bits, so the EOM ends the block), 3 preamble super-frames. It is lost if no burst is decoded or the payload differs in any bit. Bit errors are counted too (a missing decode counts every bit).
- Channels: `awgn`, and the ITU-R F.1487 Good (2 paths, 0.5 ms, 0.1 Hz), Moderate (1 ms, 0.5 Hz) and Poor (2 ms, 1 Hz, the D.6.1 channel). SNR is in 3 kHz.
- Target: 10% frame loss on AWGN, 20% on the fading channels. Fading curves are shallow (a few % per dB) and on a short block have a floor of a few %, so 10% is ill-conditioned there.
- Threshold: the SNR at which a logistic fit of logit(loss) against SNR reaches the target. The fit uses every frame of every point (half a pseudo-frame each way keeps all-zero and all-lost points finite), which makes it much less noisy than reading a crossing off the two nearest points. The interval printed next to a threshold is approximate (the fit's standard error, treating points as independent); it is a guide, not a test.
- Ladder: a 12 frame pilot at 2 dB steps finds roughly where the reference pairing reaches the target; the cell then runs 7 points centred on that (0.5 dB apart on AWGN, 1 dB on fading). If the target is not bracketed the ladder moves by half its span (up to three times). A mode that never reaches the target on a channel (a floor; high-rate modes on Poor) is reported as "target not reached" with the loss it floors at, and is not a failure.

## Same realisation for every pairing

The payload seed depends on (mode, frame) and the channel seed on (channel, mode, frame). Neither depends on the transmitter, receiver or SNR, so every pairing sees the same fading and noise and differences come from the modems. The channel only draws the same realisation for the same audio length, so the harness stops with an error if two transmitters produce audio of different lengths.

## The pass rule

Per cell (one mode, interleaver, code and channel), each pairing other than C# to C# is compared with C# to C# by the paired threshold difference (positive means the pairing needs more SNR, so is worse) and its 95% interval from a paired bootstrap (frames are resampled together in both pairings, so the realisation luck cancels and only the modems' difference is left). Then:

- FAIL: the interval lies wholly above +0.5 dB. The pairing is provably worse.
- PASS: the interval lies wholly within +/-0.5 dB.
- INCONCLUSIVE: anything else. The data cannot say. This is not a failure and is listed in the report.
- NOT REACHED: the target was never reached.

A single cell does not resolve 0.5 dB unless the pairings decode nearly the same frames. A modem that is genuinely 0.5 dB off on a few dozen frames per point gives an interval a dB wide. So the verdict that matters, "within 0.5 dB", is per channel model on the aggregate over cells: the mean of the per-cell differences with a bootstrap 95% interval that resamples cells and, within each, frames. The same FAIL / PASS / INCONCLUSIVE rule is applied to that interval. The default run applies it to its few cells; the long run applies it to the whole matrix in the combine job (78 combinations per channel).

## Does the rule work? Validation with receivers that are known to be worse

A fake receiver is the C# receiver with extra white noise added to its input, a fixed fraction of the channel noise measured from the noise-only lead-in (so it tracks each point's SNR). Adding the fraction f costs 10 log10(1 + f) dB. The noise is seeded from a run seed and the audio, so every repetition draws different noise. The table is the aggregate verdict over 20 repetitions: the rule must not fail 0.2 dB or the near-zero case, and must fail 1.0 dB.

Counts: AWGN, 3 cells (WN3, WN4, WN6, ultra-short interleaver, K7) x 40 frames per point; Moderate, 4 cells (those three and WN2 short interleaver) x 96 frames per point; 7 ladder points; 20 repetitions with different noise seeds (`MS110D_LAYER3_VALIDATE=1`, test `Layer3ValidationTests`, about 2 hours on 4 threads). "Decoder jitter" is extra noise at 0.002 of the channel noise (0.009 dB): a true zero that is not a bit-identical decode.

| channel | extra noise | FAIL | PASS (interval within +/-0.5) | INCONCLUSIVE | mean difference (dB) | mean interval width (dB) |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| AWGN | decoder jitter | 0/20 | 20/20 | 0/20 | -0.04 | 0.20 |
| AWGN | +0.2 dB | 0/20 | 20/20 | 0/20 | 0.11 | 0.33 |
| AWGN | +1.0 dB | 20/20 | 0/20 | 0/20 | 0.85 | 0.46 |
| Moderate | decoder jitter | 0/20 | 20/20 | 0/20 | -0.01 | 0.31 |
| Moderate | +0.2 dB | 0/20 | 18/20 | 2/20 | 0.20 | 0.40 |
| Moderate | +1.0 dB | 20/20 | 0/20 | 0/20 | 0.98 | 0.63 |

So at these counts the rule never failed a receiver 0.2 dB worse or equal, and failed one 1.0 dB worse every time (the AWGN mean difference reads 0.85 for a true 1.0 dB: the fit is a little compressed on the steep AWGN curve; it is a measured bias, not hidden). The long run has far more cells per channel than these tables, so its aggregate is tighter. The same experiment runs once in the default run (`Layer3SensitivityTests`, AWGN, +0.2 must not fail and +1.0 must fail, fixed seed).

Early versions of the rule read the threshold off the two points either side of the target and could not tell 0.3 dB from 0.7 dB at 40 frames; the logistic fit is what made the table above possible.

## Pairings and the C++ receiver slot

Today: C# to C# and C++ to C#. `Layer3Endpoints.CppReceiver` (`Endpoints.cs`) is a `MissingReceiver`, reported as "not yet available", and the pairings needing it are listed as skipped. The story that ports the C++ receiver sets it to an `ILayer3Receiver` that decodes through the C interface; the other two pairings then run with no other change, and each is judged against C# to C#. A real C++ receiver will not decode frame for frame like the C# one, so its cells will have intervals like the fakes above, and the aggregate is what carries the verdict.

## Running

Default (ctest test `dotnet_layer3`, about 80 s on 4 threads; the CI interop job uploads the results): `dotnet run --project tests/interop -c Release -- -trait "Category=Layer3"`. It runs the real C++ transmitter on AWGN (2 cells x 24 frames) and Moderate (1 cell x 48 frames), where the pairing is frame for frame identical so few frames are needed, plus the sensitivity test (3 cells x 40 frames, AWGN). Results land in `layer3-results/default/cells/` (override with `MS110D_LAYER3_OUT`):

- `<cell>.curves.csv`: frame and bit errors, loss and its Wilson interval per pairing and SNR.
- `<cell>.thresholds.csv`: threshold per pairing, and for each non-reference pairing the paired difference, its interval, the number of differing decodes and the verdict.
- `<cell>.outcomes.csv`: per frame lost/kept flags (the combine step recomputes aggregates from these).
- `<cell>.md`: the same, readable.

Long mode: `MS110D_LAYER3_LONG=1` and `-class Ms110dInterop.Tests.Layer3.Layer3LongTests`, with `MS110D_LAYER3_SHARD=i/n`, `MS110D_LAYER3_CHANNELS`, `MS110D_LAYER3_WN`, `MS110D_LAYER3_IL`, `MS110D_LAYER3_K`, `MS110D_LAYER3_FRAMES`, `MS110D_LAYER3_THREADS`. Frames per point: 60 on AWGN, 160 on the fading channels. A cell fails only if provably worse. `Layer3CombineTests` (`MS110D_LAYER3_COMBINE_DIR`) then reads all cells and gives the per-channel aggregate verdict.

## Long run: cadence and cost

`layer3.yml` runs by hand (workflow_dispatch, optionally narrowed by channel and waveform) and on pushes to main that touch `src`, `include`, `tests/interop`, `cmake`, the top CMakeLists or the reference pin. There is no schedule. It uses 24 shards, at most 8 at a time, each with `permissions: contents: read`; a final job combines them into the per-channel verdicts, writes a short job summary (the aggregate table and the first 100 cells that were not a conclusive pass) and uploads the cells and the combined results as artifacts.

Cost estimate from measured decode times: about 0.7 s of CPU per decode, 78 combinations x 4 channels x (60 or 160 frames) x 7 points x 2 transmitters is about 135 CPU hours, so about 1.5 to 2 hours per shard on a 4 vcpu runner, and about 5 to 6 hours for the whole run with 8 shards at a time. Each shard is far below the 6 hour job limit.
