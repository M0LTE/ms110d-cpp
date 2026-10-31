# WN 0 (Walsh) receiver

`m0lte::ms110d::Wn0Receiver` (`src/wn0_receiver.{hpp,cpp}`) is the receive side of WN 0, ported from the C# `Wid0WalshModem.DemodulateRake` and the WN 0 part of `Ms110dDemodulator` (`TrackWalsh` and the sample access it uses). It is internal, like the modulator: not installed, not in the locked API.

## What is in it

- The samples: the shared front end and ring (`src/rx_stream.hpp`: mix the 9600 Hz audio down by 1800 Hz, root-raised-cosine matched filter, keep every second output, 4800 Hz "half-chip" samples), as the C# reads its one ring. `StandaloneWn0Receiver` carries its own stream for use on its own.
- Chip reads: 8-tap windowed-sinc interpolation at the burst's timing, rotated by the carrier model (`theta_base + omega * (pos - chip0)`).
- The RAKE: 13 fingers at chip delays -6 to +6, Walsh correlation of the descrambled chips (Trinomial scramble from `trinomial_scrambler`), a one-pole decision-directed gain per finger, an MRC statistic per candidate, non-coherent selection for the first 8 symbols, max-log soft values (positive means 0, first value is the di-bit MSB).
- The decision-directed carrier loop (phase of the combined statistic averaged over 8 symbols retunes `omega` and `theta_base`) and the signal-lost discriminator (90 weak symbols in a row ends the burst).
- Block assembly: soft values go into a caller buffer in fetched (wire) order; `deinterleaved_block` writes them in code order with the existing `Interleaver`.
- Decoding: `decode_block` deinterleaves, converts to float into a buffer inside the object (the decoder takes float; the copy is a no-op rounding in the single build) and runs the caller's `fec::TailBitingViterbiDecoder` (rate 1/2 for WN 0, so no depuncturing), as the C# `Ms110dFraming.DecodeBlock` does. The decoder's own buffers are allocated when the caller constructs it.

## What is not

- Acquisition and the WN 0 identification: the caller gives `Wn0Start` (chip 0 position in half-chip samples, timing, residual carrier phase and frequency, the first data chip, the interleaver). The C# also re-fits the carrier over the last preamble super-frames before tracking (`RefineCarrier`); that is shared with the other modes and belongs with the preamble search story, so the C# values after that step are the input here.
- Searching for the EOM in the decoded bits, which ends a burst in the C#. The caller does that (the receiver runs on until the signal is lost or the caller stops stepping).
- The C# `DemodulateRakeOracle` (a diagnostic instrument, not the shipped path).

## Memory and allocation

One object of 5160 bytes, reading the shared ring (at least 4096 half-chip samples; the stand-alone one carries a 32 KB ring). Nothing is allocated, nothing throws. The caller owns one block of soft values (`interleaver.size_bits()` values, at most 1152 for WN 0). The caller must `step()` between pushes so the unread audio never exceeds the ring (a chunk of up to 3800 audio samples is always safe).

## Precision

The C# uses `float` for samples, chips, correlations and gains, and `double` for the carrier phase model, the interpolation weights, the finger power sums and the signal-lost statistics. The samples are the shared front end's single precision ones, exactly the C#'s (before the front end was shared, this receiver filtered in `real`, so the double build's samples were slightly more precise than the C#'s; the tests passed unchanged either way). From the chips on the port keeps the C#'s split: where the C# has `float` the C++ has `real` (float in the single build, so the same arithmetic; double in the default build, so slightly more accurate than the reference), and where the C# has `double` the C++ has `double`. Float constants of the C# (the gain filter 1/6 and 1/32) are widened, not recomputed in double.

## Agreement with the C#

Tests are in `tests/wn0_receiver_test.cpp`, against numbers dumped from the real 0.4.1 demodulator by `tools/csharp-vectors` (modes `wn0`, `wn0-awgn`, `wn0-poor`; the burst start is read from its private fields by reflection at the first Walsh symbol, the soft values from `FirstPassBlockLlrs`, the decoded bits from `BlockDecoded`, the end of the burst from `BurstCompleted`, and the decisions and gains from `FrameDiagnostics`). The clean and AWGN dumps are committed (not part of scripts/check-dumps.sh: they hold the C#'s float results, which differ in the last digits between machines, so they are not byte-reproducible; the tests compare decisions exactly and values with a tolerance); regenerate them when the pin moves, from the repo root, with the reference files in `<ref>`:

    dotnet run -c Release --project tools/csharp-vectors -- wn0 <ref> > tests/data/csharp-wn0-0.4.1.txt
    dotnet run -c Release --project tools/csharp-vectors -- wn0-awgn <ref> > tests/data/csharp-wn0-awgn-0.4.1.txt

The Poor channel corpus (about 1.5 MiB of audio) is never committed: `scripts/generate-wn0-poor.sh` makes it into `build/_reference/wn0-poor.{txt,i16le}`, the CI reference job runs it (cached on the tool sources and the pin) and the test skips without it (fails when `MS110D_REQUIRE_REFERENCE` is set).

- Clean audio, all ten WN0 reference cases (short, medium and long interleavers, K 7 and 9, TLC): every decision equal, soft values within 1e-4 of the case maximum (measured 4e-7 or better in the double build), finger gains within the dump's 3 decimals, and the Viterbi-decoded bits equal to the C#'s and to the transmitted bits.
- Seeded AWGN (the noise generator is repeated in the test, so no audio is stored) at -8 and -10 dB, 100 bursts each: the decisions, the sign of every soft value and the decoded bits are identical to the C#'s, tolerance zero, and the same for the C++ transmitter's audio of the same payload.
- Signal loss: 30 bursts per point are cut after one or two blocks and followed by a clean tone 150 Hz off the carrier, which is what the C# discriminator can see as "not Walsh" (noise alone, even a loud tone with noise, never fires it). The C# ends 29 of these 48 locked bursts with SignalLost at several different symbol counts and the C++ ends at exactly the same symbol, with the same decisions up to it; the rest run to the end of the audio in both. Changing the 0.35 threshold or the run of 90 fails the test.
- The Poor channel (M0LTE.HfChannel, no EOM, noise after): decisions, decoded bits and the symbol count identical, soft values within 1e-4 (measured 4e-7). None of these ends in SignalLost in the C# (a faded burst followed by noise does not).
- The C++ transmitter into the C++ receiver, clean (every decision and the decoded bits right) and in noise.
- Nothing allocates after construction, including `decode_block`.

Because the decisions are identical on every symbol, the error rates are identical, so the receive difference is far inside the 0.5 dB allowed.
