# FEC module and puncturing

`m0lte::fec` (headers `include/m0lte/fec/`, sources `src/fec/`, CMake target `m0lte::fec`) holds the rate 1/2 convolutional code definition and the full tail-biting encoder. It is integer only, allocation free and exception free, and has no dependency on anything MS110D, so it can become its own library when a second modem needs it. `m0lte::ms110d` links it publicly.

The MS110D part, Table D-L puncture and repetition masks keyed by constraint length and code rate, lives in `include/m0lte/ms110d/puncture.hpp` and `src/puncture.cpp`. Masks are stored as bit sets with the first column in bit 0. Rates are looked up by numerator and denominator, not by string as in the C#.

Differences in shape from the C# (none in output): a bad block size returns false or `nullopt` instead of throwing; a constraint length other than 7 or 9 finds no puncture entry (the C# treats anything but 9 as 7); `apply_puncture` checks the output capacity. The decoder side (depuncture, soft apply) is not ported yet; it belongs to the receive stories.

## Tests

`tests/fec_test.cpp` checks the generators against Figures D-9 and D-10, the encoder impulse response, tail-biting rotation, the code rate implied by every Table D-L mask, and exact vectors from the C# (M0LTE.Ms110d 0.2.0): FNV-1a checksums of encoder output for both codes and six block lengths, of punctured output for all 17 rates at 360 and 720 bits, and full bit dumps for ten small cases. The comment at the top of that file says how the vectors were made. Comparison of whole layer 1 files against the reference set is a later story.
