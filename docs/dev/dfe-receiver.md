# Equaliser receive path

`m0lte::ms110d::DfeReceiver` (`src/dfe_receiver.{hpp,cpp}`) is the receive side of the equaliser waveforms (WN 1 to 8 and 13): everything the C# `Ms110dDemodulator` does after the preamble detector (docs/dev/preamble-detector.md) has handed over a burst, up to the decoded blocks and the end of the burst. It is ported from M0LTE.Ms110d 0.2.1, step for step. It is internal, like the detector and the WN 0 receiver: not installed and not in the locked API. The public receive API (C++ and the poll-and-drain C interface) is built on it in a later PR; story 27.

## What is in it

- `InitializeDfe`: tap counts, lead and ridges per probe length (`eq_params_for_probe_length`), the least-squares solve over the last half preamble super-frame and the first mini-probe, RLS seeded from it with the frame-tied forgetting factor `1 - ln 10 / U`, and the probe statistics the collapse and signal-lost tests measure against. The level gain and the carrier re-fit the C# does first here are already done by the detector, whose event carries the result.
- `ProcessFrame`, per frame: the probe pattern score where the two shifted-probe conventions disagree (`src/probe_pattern.hpp`, the C# `ProbePattern`, issue 68), the probe-anchored solve, collapse detection and the fresh re-solve, the phase re-anchor, the carrier trim from the tap rotation (clamped to 3 Hz of the acquired carrier), the fading statistic and its per-burst latch, then the data span: three passes averaged on a static channel, the probe-anchored tap trajectory with RLS on top when fading, or the QAM path. Then soft values, probe timing, the probe presence statistic (with the carrier cancellation) and the signal-lost and signal-absent exits.
- `FinishBlock`: deinterleave, depuncture onto the mother lattice (`depuncture_soft`, the C# `Ms110dPuncture.Depuncture`), tail-biting Viterbi, the EOM scan over the burst's bits and the block limit.
- A small front end (`src/rx_front_end.hpp`): mixer, matched filter and decimation to the T/2 stream, the same arithmetic as the detector's own, and a ring of T/2 samples.

## Where the boundary is

Not here yet:

- The turbo re-equalisation loop, the frozen-probe salvage paths and the MFB block decoder (the next PR, with issue 26). They run at the end of a block, and the C# saves and restores every piece of tracking state around them (taps, training rows; the carrier, timing and RLS state are never touched), so everything above, including every first-pass soft value, is the same with or without them. The C# tool checks this on every burst it dumps: it runs each one with turbo on and off and fails if any first-pass soft value differs. What the turbo pass changes is only the decoded bits of a block whose first pass had errors.
- The terminate command and the public API, events and options (a later PR). The receiver has `max_input_data_blocks`; the C#'s other options (forgetting factor, track ridge, turbo off, presence release off) are not here.
- The C#'s diagnostic instruments (channel-truth genie, oracle and truth passes, the W5a seam), which never run in normal use and leave the shipped path bit-identical when unset.
- WN 0, which is `Wn0Receiver` (docs/dev/wn0-receiver.md).

## Using it

`configure(config, storage, k7, k9)` with caller storage of `storage_bytes(config)` bytes and the caller's two tail-biting Viterbi decoders (built for `max_input_bits_for(config.max_interleaver)` info bits). Feed it the same audio as the detector, from the same first sample: `push_audio()` returns how many samples it used, and stops after a sample that raised an event, like the detector. When the detector reports a burst, call `start(dfe_start_from_detection(detection))` before pushing the samples after the detection: the C# initialises the equaliser when a fixed number of samples past the data start have arrived, and the reads there depend on exactly how many have (the interpolator reads zero for a sample not yet received), so the moment matters to the bit. `event()` is then `BlockDecoded` (the block's index, decoded bits, first-pass soft values and the T/2 sample it happened at, valid until the next push) or `BurstEnded` (reason, blocks, payload length: bits before the EOM, or all decoded bits without one, and the T/2 sample); a block that ends the burst raises both, the block first. `take_event()` moves on. Every event carries its sample position, so tests match them with the C#'s one for one. Bits are delivered a block at a time and the end event only closes the burst; the receiver keeps 31 bits of the burst for the EOM scan and nothing more.

`set_frame_trace()` installs a per-frame hook with the tracking state (the C# `FrameDiagnostics` point), for tests and diagnostics.

`tests/support/rx_driver.{hpp,cpp}` runs the detector and the receiver together the way the C# does, for the tests; the real demodulator that does it is the later PR.

## Precision

Exactly the C#'s choices, the same in both builds, as in the detector: samples, the ring, the interpolated reads, the equaliser, decisions and soft values are single precision (the C# `Complex32` and `float`, through `src/dsp/complex_f.hpp`, so products round as the C#'s do); the carrier and timing model, the interpolator weights, the probe statistics (gain, MSE, energy and their references), the fading statistic and all of the presence statistic are double. Mixed expressions keep the C#'s types (for example the probe gain is a float divided by an int, then widened). The only `real` is inside the equaliser, which keeps its running sums in `real` (docs/dev/equaliser.md), so the single build differs from the C# only there. `-ffp-contract=off` keeps compiler fusing out.

## Memory

Nothing allocates after `configure` and nothing throws. The object is 26856 bytes (fixed scratch for 64 taps and 256 data symbols, the presence window and spectrum). The caller's block holds the T/2 ring, the equaliser's block (sized for the largest, K = 48), the block's soft values and decode buffers, and the presence statistic's DFT tables:

| Longest interleaver accepted | Ring (T/2 samples) | Storage | Decoders for (info bits) |
| --- | --- | --- | --- |
| Ultra-short | 4096 | 209440 bytes | 768 |
| Short | 8192 | 287520 bytes | 3072 |
| Medium | 16384 | 534304 bytes | 12288 |
| Long | 65536 (the C#'s) | 1652512 bytes | 49152 |

The ring is sized to hold a whole interleaver block plus the probe after it and the last preamble super-frame, because the turbo pass (next PR) re-reads the block at its end; without it the receiver only looks back about one frame. The C# always uses 65536. The ring size never changes a result: every read is of a sample still in the ring (asserted in debug builds). Ways to shrink for a microcontroller (issue 44): size the equaliser block for the waveforms actually accepted (K = 32 needs 55 KB, not 119 KB), compute the DFT table for the burst's probe length only, and window the block buffers.

## Tests and agreement with the C#

`tests/dfe_receiver_test.cpp`, against two dumps from the real 0.2.1 demodulator made by `tools/csharp-vectors` (`RxDump.cs`; private fields are read by reflection at the C#'s `FrameDiagnostics` point, the presence statistic comes from `PresenceTraced`, the soft values from `FirstPassBlockLlrs` and the decoded bits from `BlockDecoded`):

    dotnet run -c Release --project tools/csharp-vectors -- rx <ref> > tests/data/csharp-rx-0.2.1.txt
    dotnet run -c Release --project tools/csharp-vectors -- rx-awgn <ref> > tests/data/csharp-rx-awgn-0.2.1.txt

Like the WN 0 dumps they hold the C#'s floating point results, whose last bits can differ between machines, so they are not in `scripts/check-dumps.sh`; regenerate them when the pin moves.

- **Clean, every WN 1 to 5 ultra-short and short reference case (41).** Frame by frame: the same frames at the same input sample, the same fading, latch, fresh-solve and bad-probe decisions, and the carrier, timing, probe references, MSE, fade floor, presence statistics, probe pattern evidence and tap energy; the decoded bits exactly (they equal the transmitted payload, and the C#'s with turbo on); the first-pass soft values; the end of the burst (reason, blocks, payload length, sample). Measured: the default build reproduces the C# bit for bit, every value difference 0 and the hash of the taps after every one of the 870 frames equal; the single build is within 8.3e-7 (relative) everywhere, with every decision and decoded bit the same.
- **Seeded AWGN, 212 bursts.** WN 1 to 5 short, K 7, twenty trials at each of two SNRs per waveform, from where the first pass fails every burst (WN 1 at -9 dB) to where it decodes all of them; plus twelve cut bursts (WN 2 and 4, the signal stops half way and 20 s of noise follow, a steady tone on a probe line in half of them), which end in the signal-lost exit (11) and the signal-absent exit (1) and exercise the presence statistic's carrier cancellation. The noise generator is repeated in the test, so no audio is stored. Per block: the decoded bits exactly, the sign of every soft value (346624 of them) exactly, the hash of the soft values' bits, the sum of their magnitudes, the T/2 sample; per burst the frame count, the end (reason, blocks, payload, sample) and the final tracking state. Measured: identical in the default build (all 438 soft value hashes equal); in the single build, signs, decoded bits and ends identical, magnitudes within 2.2e-7, tracking state within 1.7e-5.
- The bounds in the test (1e-4 relative on values, 1e-8 on the carrier increment, 1e-3 rad on the phase) are far above what is measured, to leave room for another machine's libm, whose last bits of sine and cosine move the detector's carrier by about 1e-6 Hz (docs/dev/preamble-detector.md). Decisions, decoded bits, signs, frame and sample positions are exact.
- Without reference files: the C++ transmitter into the C++ receiver for every WN 1 to 5 ultra-short and short combination, K 7 and 9, at block-edge payload lengths (80 bursts, all decoded), and in noise 3 dB above the C#'s upper test points (40 bursts, all decoded); results independent of the chunk size (1 to 100000 samples); zero allocation over a whole WN 1 burst; storage, configuration and refusals; `depuncture_soft` against the puncture map for every rate; the two probe pattern rules and the evidence thresholds.

Because the decisions are identical on every burst, the receive difference against the C# on these signals is zero, far inside the plan's 0.5 dB.
