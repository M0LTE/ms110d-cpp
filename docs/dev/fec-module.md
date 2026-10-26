# FEC module and puncturing

This is an internal module: its headers are in `src/`, not `include/`, are not installed, and are not part of the locked public API (same as the scrambler and interleaver). `src/fec/` is laid out as a module boundary so it can be split into its own library later.

`m0lte::fec` (`src/fec/convolutional.hpp`, `src/fec/tail_biting_encoder.cpp`, compiled into the `ms110d` library) holds the rate 1/2 convolutional code definition and the full tail-biting encoder. It is integer only, allocation free and exception free, and has no dependency on anything MS110D, so it can become its own library when a second modem needs it.

The MS110D part, Table D-L puncture and repetition masks keyed by constraint length and code rate, lives in `src/puncture.hpp` and `src/puncture.cpp`. Masks are stored as bit sets with the first column in bit 0. Rates are looked up by numerator and denominator, not by string as in the C#.

Differences in shape from the C# (none in output): a bad block size returns false or `nullopt` instead of throwing; a constraint length other than 7 or 9 finds no puncture entry (the C# treats anything but 9 as 7); `apply_puncture` checks the output capacity. The soft decoders (tail-biting Viterbi and SISO) are ported too; see [soft-decoders.md](soft-decoders.md). Depuncture and the soft apply are not ported yet; they live in the C# puncture class, not the decoders, and belong with the receive chain.

## Tests

`tests/fec_test.cpp` checks the generators against Figures D-9 and D-10, the encoder impulse response, tail-biting rotation, the code rate implied by every Table D-L mask, and exact vectors from the C# (M0LTE.Ms110d 0.3.0): FNV-1a checksums of encoder output for both codes and six block lengths, of punctured output for all 17 rates at 360 and 720 bits, and full bit dumps for ten small cases. The vectors come from `tools/csharp-vectors` (`fec` mode); the comment at the top of that file gives the exact commands. Comparison of whole layer 1 files against the reference set is a later story.
