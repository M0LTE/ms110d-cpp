# WN 0 (Walsh) receiver

`m0lte::ms110d::Wn0Receiver` (`src/wn0_receiver.{hpp,cpp}`) is the receive side of WN 0, ported from the C# `Wid0WalshModem.DemodulateRake` and the WN 0 part of `Ms110dDemodulator` (`TrackWalsh` and the sample access it uses). It is internal, like the modulator: not installed, not in the locked API.

## What is in it

- The front end: mix the 9600 Hz audio down by 1800 Hz, root-raised-cosine matched filter, keep every second output (4800 Hz, "half-chip" samples) in a ring.
- Chip reads: 8-tap windowed-sinc interpolation at the burst's timing, rotated by the carrier model (`theta_base + omega * (pos - chip0)`).
- The RAKE: 13 fingers at chip delays -6 to +6, Walsh correlation of the descrambled chips (Trinomial scramble from `trinomial_scrambler`), a one-pole decision-directed gain per finger, an MRC statistic per candidate, non-coherent selection for the first 8 symbols, max-log soft values (positive means 0, first value is the di-bit MSB).
- The decision-directed carrier loop (phase of the combined statistic averaged over 8 symbols retunes `omega` and `theta_base`) and the signal-lost discriminator (90 weak symbols in a row ends the burst).
- Block assembly: soft values go into a caller buffer in fetched (wire) order; `deinterleaved_block` writes them in code order with the existing `Interleaver`.

## What is not

- Acquisition and the WN 0 identification: the caller gives `Wn0Start` (chip 0 position in half-chip samples, timing, residual carrier phase and frequency, the first data chip, the interleaver). The C# also re-fits the carrier over the last preamble super-frames before tracking (`RefineCarrier`); that is shared with the other modes and belongs with the preamble search story, so the C# values after that step are the input here.
- The decoder. The tail-biting soft decoder is story #26 and is not on main. The hook is `deinterleaved_block`: rate 1/2 for WN 0, no puncturing, so its output is the decoder's input (mother code soft values, positive means 0). That is why the story is "Part of", not "Closes".
- The C# `DemodulateRakeOracle` (a diagnostic instrument, not the shipped path).

## Memory and allocation

One object: 71504 bytes in the double build, 36424 in the single build (the ring of 4096 half-chip samples is nearly all of it). Nothing is allocated, nothing throws. The caller owns one block of soft values (`interleaver.size_bits()` values, at most 1152 for WN 0). The caller must `step()` between pushes so the unread audio never exceeds the ring (a chunk of up to 3800 audio samples is always safe).

## Precision

The C# uses `float` for samples, chips, correlations and gains, and `double` for the carrier phase model, the interpolation weights, the finger power sums and the signal-lost statistics. The port keeps exactly that split: where the C# has `float` the C++ has `real` (float in the single build, so the same arithmetic; double in the default build, so slightly more accurate than the reference), and where the C# has `double` the C++ has `double`. Float constants of the C# (the gain filter 1/6 and 1/32) are widened, not recomputed in double. The mixer table uses the cosine and sine of the float-rounded phase, like `MathF`. The FIR filter accumulates newest sample first in the same order. The skipped (odd) filter outputs are not computed; they are discarded in the C#, so the result is unchanged.

## Agreement with the C#

Tests are in `tests/wn0_receiver_test.cpp`, against numbers dumped from the real 0.2.0 demodulator by `tools/csharp-vectors` (modes `wn0`, `wn0-awgn`, `wn0-poor`; the burst start is read from its private fields by reflection at the first Walsh symbol, the soft values from `FirstPassBlockLlrs`, the decisions and gains from `FrameDiagnostics`). Regenerate when the pin moves, from the repo root, with the reference files in `<ref>`:

    dotnet run -c Release --project tools/csharp-vectors -- wn0 <ref> > tests/data/csharp-wn0-0.2.0.txt
    dotnet run -c Release --project tools/csharp-vectors -- wn0-awgn <ref> > tests/data/csharp-wn0-awgn-0.2.0.txt
    dotnet run -c Release --project tools/csharp-vectors -- wn0-poor tests/data/csharp-wn0-poor-0.2.0

- Clean audio, all ten WN 0 reference cases (short, medium and long interleavers, K 7 and 9, TLC): every decision equal, soft values within 1e-4 of the case maximum (measured 4e-7 or better in the double build), finger gains within the dump's 3 decimals. The decisions are the transmitted di-bits, the signs past the warm-up are the transmitted bits, and the deinterleaved block has the coded bits' signs.
- Seeded AWGN (the noise generator is repeated in the test, so no audio is stored) at -8 and -10 dB, 100 bursts each: decisions identical to the C# on every symbol, and the same for the C++ transmitter's audio of the same payload.
- The Poor channel (M0LTE.HfChannel, stored as 16 bit audio trimmed to the data): decisions identical, soft values within 1e-4.
- The C++ transmitter into the C++ receiver, clean (every decision right) and in noise.
- Nothing allocates after construction.

Because the decisions are identical on every symbol, the error rates are identical, so the receive difference is far inside the 0.5 dB allowed.
