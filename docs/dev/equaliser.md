# Equaliser

`src/equaliser.hpp` and `src/equaliser.cpp` (internal, not installed, not in the locked API) are the C++ counterpart of the C# `Dfe`: a fractionally spaced (T/2) feed-forward filter plus a symbol-spaced feedback filter over past decisions. Taps come from a regularised least-squares solve over known symbols, are re-solved at every mini-probe (anchored to the current taps), and between probes follow the data by recursive least squares (or NLMS). The C# also has a channel-shortening solve (`SolveTrainingTir`, used by the turbo pass) and a soft-label training row; both are ported. The demodulator that drives it is a later story. The C# comments cite the textbooks the design comes from (Proakis for the DFE, Haykin for NLMS); this port is from the C# source and nothing else.

## Interface

`m0lte::ms110d::Dfe`, methods in snake_case (`equalise`, `nlms`, `begin_training`, `add_training_row`, `solve_training`, `solve_training_tir`, `begin_rls`, `seed_rls_from_training`, `rls_update`, `symmetrize_p`, `snapshot_taps`, `load_taps`, `load_interpolated_taps`, `translate_taps`, `rotate_taps`, `snapshot_training`, `restore_training`). Samples and taps cross the interface as `std::complex<float>` (`EqSample`), as the C# `Complex32`. Where the C# throws (training not begun, restore without snapshot) the C++ returns `false`.

Probe interface: the equaliser never decides what a probe contains. The caller supplies the known symbols, either row by row (`add_training_row(window, past, desired, weight)`) or as a sweep (`train_on_known(known, first_row, fill, context, weight)`, which is the C# probe-site loop). `known` is the probe plus however many symbols before it the feedback filter needs, `fill` builds the T/2 window for a symbol index, and `first_row` is at least the feedback tap count. Which probe applies at a site, including a probe whose pattern was detected rather than assumed, is the caller's business.

`score_known(known, first_row, fill, context)` is the same sweep as `train_on_known` without adding rows or touching the training state, and callable on a const object: it equalises each known symbol with the current taps and returns the `ProbeStats` (correlation sum of `y * conj(known)`, error energy, rows). It is for scoring a trial probe pattern against the taps without training on it.

`eq_params_for_probe_length(k)` holds the C# demodulator's tap counts and ridges per probe length (K = 48: 32 FF + 22 FB; K = 24: 16 + 6; otherwise 24 + 12), so the later demodulator story can size the block from the mode.

## Memory

No allocation, ever, and no exceptions. Like the transmitter, all working memory is one block the caller hands to `configure(ff, fb, storage)`; `Dfe::storage_bytes(ff, fb)` says how big (0 if the taps are out of range: at least 1 feed-forward, any feedback, at most 64 in total, the same limit the C# scratch has). The object itself is a few pointers and counters.

The block is dominated by five n x n complex (8 byte) matrices, n being the total taps: the accumulated Gram matrix, the Cholesky factor, a scratch Gram for subset and seed solves (the seed Gram shares it), the saved Gram for nested re-training, and the RLS inverse-correlation matrix P. The rest is vectors. They are kept square, as in the C#, so the arithmetic is the same operation for operation.

| Probe length K | Used by | Taps (FF + FB) | n | Block |
| --- | --- | --- | --- | --- |
| 48 | WN 1, WN 2 | 32 + 22 | 54 | 121555 bytes (118.7 KiB) |
| 32 | WN 3 to 8, WN 13 | 24 + 12 | 36 | 55203 bytes (53.9 KiB) |
| 24 | none at 3 kHz | 16 + 6 | 22 | 21459 bytes (21.0 KiB) |

`tests/equaliser_test.cpp` checks these numbers, so the table cannot drift. Ways to shrink it if the microcontroller needs it, none done: store the Gram, Cholesky factor and saved Gram as packed triangles (about 40% less, same arithmetic, more index work), drop the saved Gram when the turbo re-training pass is not used (20% of the block), or run the K = 48 modes with fewer taps (an accepted deviation with its own test).

Cost, since earlier profiling found RLS dominated receive: each `rls_update` is about 2n^2 complex multiply-adds (the product P u and the update of P), 5832 for n = 54 and 2592 for n = 36, per symbol, per pass (the static path runs three passes). The solves are O(n^3) once per probe.

## Floating point

The C# `Dfe` does its complex arithmetic in single precision (`Complex32`) and only its running sums in double: the trace of a Gram matrix (which sets the ridge) and the residual sums `lin`, `quad`, `nrm`, `dt`, `sse` of the solves. The port keeps that split exactly.

| What | C# | C++ |
| --- | --- | --- |
| Taps, Gram, Cholesky factor, P, windows, decisions | float (`Complex32`) | float, in both builds |
| Weights, ridges, lambda, mu, energies, scalar thresholds | float | float, in both builds |
| Trace, `lin`, `quad`, `nrm`, `dt`, `sse` accumulators | double | `real` (double in the default build, float in the single build) |

Complex products are written out as the C# writes them, in the same order, and `-ffp-contract=off` keeps them unfused, so the default build reproduces the C# bit for bit (except `log`, used once in the channel-shortening threshold, which comes from libm on both sides). The single build therefore differs from the C# only in the sums that the C# does in double. The C# keeps its matrices in float even though the default C++ build could afford double; that is deliberate here, because the C# is the reference and the demodulator downstream was tuned on it.

## How it is tested

`tools/csharp-vectors` has a `dfe` mode that drives the real C# `Dfe` (public in the 0.2.0 package) through scripted receive scenarios mirroring the demodulator's frame loop: initial solve over known symbols, a probe-anchored solve per frame, a phase re-anchor, then the data span by the three-pass static path, the tap-trajectory path with `translate_taps`, or NLMS, with decision-directed rows, nested re-training with snapshot and restore, soft-label rows, and the channel-shortening solve. The synthetic scenarios are a seeded burst of known symbols through a T/2 multipath channel with noise and a slow rotation, at three tap sets, clean, moderate and noisy, with an echo channel and a no-distortion channel. The real scenarios take the symbol streams of three C# reference cases (WN 1, 3 and 5) and put them through the same channel. A symbol error curve over seven noise levels (19200 symbols each) comes from the same script.

The inputs are not in the dump. The generator (`tests/support/eq_scenario.cpp` and `DfeDump.cs`, kept in step) uses only integer arithmetic and IEEE add, multiply and square root in double, so both languages make identical samples; each scenario records a hash of its symbols and its samples and the test refuses to compare unless the C++ hashes match. The dump holds the outputs, as hex of the float bits: equalised symbols, decisions, the mean square error and correlation of every probe, the feed-forward energy after each solve, and the final taps.

Regenerate the dump when the C# pin moves:

    dotnet run -c Release --project tools/csharp-vectors -- dfe build/_reference/ref > tests/data/csharp-dfe-0.2.0.txt

`tests/equaliser_test.cpp`:

- Synthetic scenarios (no reference files needed) and real-stream scenarios (named `reference ...`, which the CI reference job requires not to skip).
- Equalised symbols: bit for bit in the default build; within 1e-5 in the single build (the worst measured is 1.2e-6, float rounding of the ridge). Tap vectors agree to 2e-6 relative in single, exactly in double.
- Decisions: exact on every scenario in both builds, including the noisy ones. The noisy scenarios allow 1 in 200 in the test, in case a symbol ever sits within rounding of the slicer on another compiler.
- Channel shortening: lags and flags exact; coefficients, residuals and the equalised frame bit for bit in double, within 1e-4 in single (the null case differs by 5e-7).
- Symbol error rate against noise: with the C# seed the C++ makes the same samples, and the error counts are equal at all seven noise levels in both builds (0, 0, 0, 22, 240, 687, 1811 of 19200). With another seed, where there are 20 or more errors, the count stays within five Poisson deviations of the C# count. Single precision is not materially worse: the same counts.
- Probe statistics (`ProbeStats` from `train_on_known`: correlation, error energy, rows) against the C# probe loop, and `score_known` against `train_on_known`. The tap counts, lead and ridges of `eq_params_for_probe_length` against the parameters every dump scenario ran with.
- The paths the main scenarios skip, in a `misc` section of the dump: the genie noise power in the seed and the solve, `load_interpolated_taps`, the cap on P (`symmetrize_p` changes the taps afterwards, so it is not vacuous) and the training target energy after a snapshot and restore (through the shortening solve's residuals). Shortening solves also cover acceptance (echo found), rejection when the echo is buried in noise, rejection of a wrong constrained lag, and a channel with no cursor. The monic sign fallback only runs when the floating-gain refit fails, which none of these reach; the accepted coefficient (floating form) is compared with the C# exactly.
- `scripts/check-dumps.sh` regenerates all five committed dumps and the CI reference job fails on any difference.
- Memory table, storage checks (too small, odd alignment, out of range), the throwing cases, `train_on_known` against the row loop, and zero allocation across every operation for each tap set (the allocation guard, docs/dev/allocation-guard.md).

## Differences from the C#

- Not a number in, `false` out. A NaN row in the training accumulation makes `solve_training` and `solve_training_tir` return without installing taps (the C# would install NaN taps: its Cholesky guard `sum.Re <= 0` is false for NaN, and `Math.Max(0, NaN)` passes the NaN into the residual). The guards are `!(x > 0)`, `solve_training` refuses a non-finite solution, and `rls_update` skips a NaN gain denominator. This is a deliberate robustness difference, not an accepted deviation: on finite inputs the behaviour is identical, which the dump proves (the double build still matches it bit for bit on every scenario).
- The object is not movable (it points into the caller's block; a second object must never share that block).
- Block storage supplied by the caller instead of arrays allocated in the constructor and on first use (`SnapshotTraining`, `BeginRls`); everything exists from `configure`.
- The seed Gram of `seed_rls_from_training` shares the subset scratch (the two are never live together), saving one matrix.
- No exceptions: `false` for the C# `InvalidOperationException` cases, and `configure` returns `false` for tap counts the C# constructor rejects (and for more than 64 taps, where the C# fails later in `RlsUpdate`).
- `train_on_known` is new: the C# probe-site loop with the caller's symbols.
