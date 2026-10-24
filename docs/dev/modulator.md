# Modulator and the dsp module

This is an internal module, like the FEC module: its headers are in `src/`, are not installed, and are not part of the locked public API. The public transmitter (docs/dev/transmitter.md) wraps it.

## What is here

- `src/dsp/filter_design.{hpp,cpp}` (`m0lte::dsp`): the root-raised-cosine pulse function. It knows nothing of MS110D and is laid out as a module boundary so it can become its own library, as `src/fec/` can. Only the part of M0LTE.Dsp the modulator uses is ported (`FilterDesign.RootRaisedCosine`). The C# modulator uses no FIR filter, so `FirFilter`, the low-pass and band-pass designs and the rest wait for the receiver.
- `include/m0lte/ms110d/tx_settings.hpp`: `TxSettings` (public), the C# `Ms110dTxSettings` with the same fields and defaults (waveform 6, short interleaver, K 7, 3 super-frames, no TLC, EOM and EOT on, amplitude 0.5, roll-off 0.35).
- `src/modulator.{hpp,cpp}`: `design_pulse`, `pulse_span_symbols` and the streaming `Modulator`, from burst symbols to 9600 Hz audio: pulse shaping at 4 samples per symbol and mixing to the 1800 Hz sub-carrier. The symbol stream itself (preamble, scrambling, probes, FEC) is the framing module's job; the modulator takes constellation points.

## Streaming, no allocation

`Modulator` is a plain object (sizeof 2392 bytes in the double build, 1864 in the single build): the pulse (at most 257 taps, the 64 symbol cap), the last 65 symbols and a 16 entry double carrier table. Nothing depends on the burst length, so nothing is allocated after construction and nothing throws. A sample depends only on symbols up to its own, so there is no delay: `push_symbol` makes 4 samples available, `read` takes them in chunks of any size (one sample at a time if need be, for example to fill a DMA half buffer), and after `end_of_symbols` `read` drains the pulse tail (one pulse length). The cost is about 17 multiply-adds per sample per component. A test reads the same burst in chunks of 1, 3, 7, 64 and 1000 samples and requires bit-identical output.

## Floating point

The C# computes in float, with `Math` functions in double for the pulse design and the carrier. The C++ follows it where it can be followed cheaply and exactly, and uses `real` elsewhere:

- The pulse is designed in double and stored as float, whatever the build: the RRC in double, each tap rounded to float, the energy summed in double from float products, the normaliser rounded to float and applied in float. This is the C# step for step, so the taps equal the C# taps (the pulse test finds an error of exactly 0 against file 11, in both builds, against a tolerance of 1e-6).
- Symbol scaling (constellation float times amplitude) and the shaping sums run in `real`, and the mix always runs in double and is rounded once to `real` (the C# does `(float)(re * cos - im * sin)` in double): float in the single build, double in the default build. The shaping sums add the contributions oldest symbol first, the same order as the C#, so in the single build the baseband equals the C# baseband exactly. Mixing in double instead of float in the single build cut its worst audio error from 3.0e-8 to 1.5e-8 (one float step at the sample size, i.e. the rounding of the reference itself) and its relative level error from 1.9e-8 to 3.5e-12; the cost is one double multiply-add pair per sample, which the Cortex-M7's double-precision FPU does in hardware.
- The carrier is a 16 entry table (1800 Hz at 9600 Hz repeats every 16 samples), computed in double at configure time from the same phase expression as the C# and kept in double. The C# evaluates the phase afresh per sample in double; its error after a million samples is about 1e-10, far below float resolution.
- `-ffp-contract=off` applies as everywhere, so no fused multiply-add changes the rounding. The maths is in `src/`, never in a public header.

## Tolerance

The reference audio and baseband are float32 files, compared at 1e-6 absolute per sample. The C# repo suggests 1e-5 for a port, but the measured errors are 1.5e-7 or less in both builds, so we hold ourselves to 1e-6 (still six times the worst case, tight enough that a one-part-in-a-million change in amplitude or a few hundredths of a hertz of carrier error would be caught). In the single build the baseband must be exactly equal to the reference. Measured worst case over the 122 cases (re-measured and printed as a warning by the test on every run): double build, audio 1.5e-7 and baseband 1.5e-7 (both on the 0.10 roll-off case, and the same on gcc and clang); single build, audio 1.5e-8 and baseband exactly 0. The double build is further from the reference than the single one because the reference files are float32 and the double build does not round its sums to float. Roll-offs 0.05 and 1.0 have no reference files; the test `modulator matches a simple reference shaper` compares against a direct double convolution (worst 9e-14 double, 2.3e-7 single). The reference file for the pulse is compared at 1e-6.

Besides the sample comparison the tests check length, the RMS level (relative error at most 1e-5) and the magnitude spectrum at 17 frequencies from 0 to 4800 Hz (at most 1e-5 of the strongest). A mutation test changes the roll-off (0.36), the amplitude (0.501), the carrier (1801 Hz) or the carrier phase (one sample) and requires the comparison to fail.
