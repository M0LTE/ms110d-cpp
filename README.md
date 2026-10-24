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

Options: `-DMS110D_PRECISION=single` for single precision (double is the default), `-DMS110D_BUILD_TESTS=OFF` for the library alone. For a Cortex-M7 compile check use `-DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/cortex-m7.cmake`. Use it from another project with `find_package(m0lte-ms110d CONFIG)` or `FetchContent`, then link `m0lte::ms110d`. The C interface (`include/m0lte/ms110d/ms110d.h`, shared library target `ms110d_c`, option `MS110D_BUILD_C_SHARED`) is for other languages. The .NET interop tests run under ctest when a .NET 10 SDK is installed. See [docs/dev](docs/dev) for the C interface conventions, the API lock, the allocation guard, the interop tests and the C# reference files.

## Transmitting

Payload bits in (one bit per byte, 0 or 1), 9600 Hz audio on the 1800 Hz sub-carrier out, for one burst at a time. The settings are the C# `Ms110dTxSettings`, with the same defaults.

    #include <m0lte/ms110d/transmitter.hpp>
    using namespace m0lte::ms110d;

    TxSettings settings;                    // WN 6, short interleaver, K 7, and so on
    settings.waveform_number = 4;

    const std::size_t max_bits = 4000;      // the largest payload this transmitter will carry
    std::size_t bytes = 0;
    if (Transmitter::storage_bytes(settings, max_bits, bytes) != Status::Ok) { /* bad settings */ }
    std::vector<std::uint8_t> memory(bytes);  // or a static array: the library never allocates

    Transmitter tx;
    if (tx.configure(settings, max_bits, memory) != Status::Ok) { /* bad settings or memory */ }

    if (tx.push_bits(payload_bits) != Status::Ok) { /* more than max_bits in all */ }
    if (tx.end_payload() != Status::Ok) { /* wrong step */ }
    std::size_t samples = 0;
    tx.audio_samples(payload_bits.size(), samples);  // the whole burst's length, known up front

    float chunk[480];
    while (!tx.done()) {
        std::size_t got = 0;
        if (tx.read(chunk, got) != Status::Ok) break;  // any chunk size, down to one sample
        play(chunk, got);
    }
    tx.begin();                             // ready for the next burst

For a whole burst in one call, like the C# `Modulate`, use `tx.modulate(payload_bits, audio, written)`; if `audio` is too short it returns `BufferTooSmall` and sets `written` to the length needed.

The audio is the same as the C# `Ms110dModulator.Modulate` makes, to within 1e-6 per sample. Nothing throws, and nothing allocates after `configure`. The same thing is available from C as `ms110d_tx_*` in `ms110d.h`, with the library allocating (`ms110d_tx_create`) or the caller supplying the memory (`ms110d_tx_memory_size` and `ms110d_tx_init`); `ms110d_tx_modulate` is the one-shot call. See [docs/dev/transmitter.md](docs/dev/transmitter.md).

## Compatibility, proven by tests

The two implementations must agree exactly on the bits on the wire, closely on the transmitted audio, and within 0.5 dB on receive performance, on simulated HF channels and on real off-air recordings. Any difference we keep on purpose is written down as a named test. See [docs/plan.md](docs/plan.md#how-we-prove-compatibility).

## Licence

Copyright (C) 2026 Thomas Fanning.

AGPL-3.0-or-later; see [LICENSE](LICENSE). Commercial licences are available from the copyright holder for use where the AGPL does not suit. See [PROVENANCE.md](PROVENANCE.md) for where the code comes from.
