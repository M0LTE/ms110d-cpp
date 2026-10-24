# Provenance

This library is a port of [M0LTE.Ms110d](https://github.com/M0LTE/M0LTE.Ms110d), checked stage by stage against MIL-STD-188-110D (29 Dec 2017, Distribution A). Nothing else goes into it: no other implementation of the standard, open or otherwise, is consulted.

M0LTE.Ms110d's own provenance is recorded in [its PROVENANCE.md](https://github.com/M0LTE/M0LTE.Ms110d/blob/main/PROVENANCE.md): every wire constant transcribed from the standard, the equaliser and decoders designed from textbooks.

## Dependencies of the C# reference

M0LTE.Ms110d depends on M0LTE.Dsp and M0LTE.Fec, both written by the copyright holder with no third-party source. Up to version 0.1.0 it also used one type, a complex number struct, from M0LTE.Ofdm, which is a port of codec2 (LGPL-2.1). From version 0.2.0 it has its own original complex type and no longer depends on M0LTE.Ofdm ([#55](https://github.com/M0LTE/ms110d-cpp/issues/55)). The test-only Watterson channel in M0LTE.HfChannel still uses M0LTE.Ofdm, but it is not part of either library.

This C++ library uses `std::complex` and contains no codec2-derived code.

## Contributions

All code here is owned by the copyright holder, Thomas Fanning, so that it can be offered under commercial licences as well as the AGPL. Outside contributions can only be accepted with a contributor licence agreement. Please open an issue before writing code.
