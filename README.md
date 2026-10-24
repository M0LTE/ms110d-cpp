# ms110d-cpp

A C++ implementation of the **MIL-STD-188-110D Appendix D** 3 kHz serial-tone HF modem, built to be compatible on air with the C# reference, [M0LTE.Ms110d](https://github.com/M0LTE/M0LTE.Ms110d).

**Status: not started.** The plan is in [docs/plan.md](docs/plan.md) and the work is tracked as epics and stories in the [issues](https://github.com/M0LTE/ms110d-cpp/issues).

## What it will be

- A waveform library on its own: bits in, 9600 Hz audio on an 1800 Hz sub-carrier out, and back. No KISS, no sound card, no host application.
- The same modes as the C# package: WN0 to WN8 and WN13, short and long interleavers, K7 and K9 codes.
- C++20, no exceptions, no RTTI and no allocation after start-up, so the same code builds for a PC and for a Cortex-M7.
- A small C interface for use from other languages.
- An installable CMake project: `find_package`, `FetchContent` or a git submodule.

## Building

    cmake -S . -B build
    cmake --build build -j4
    ctest --test-dir build

Options: `-DMS110D_PRECISION=single` for single precision (double is the default), `-DMS110D_BUILD_TESTS=OFF` for the library alone. For a Cortex-M7 compile check use `-DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/cortex-m7.cmake`. Use it from another project with `find_package(m0lte-ms110d CONFIG)` or `FetchContent`, then link `m0lte::ms110d`. See [docs/dev](docs/dev) for the API lock and the allocation guard.

## Compatibility, proven by tests

The two implementations must agree exactly on the bits on the wire, closely on the transmitted audio, and within 0.5 dB on receive performance, on simulated HF channels and on real off-air recordings. Any difference we keep on purpose is written down as a named test. See [docs/plan.md](docs/plan.md#how-we-prove-compatibility).

## Licence

AGPL-3.0-or-later; see [LICENSE](LICENSE). Commercial licences are available from the copyright holder for use where the AGPL does not suit. See [PROVENANCE.md](PROVENANCE.md) for where the code comes from.
