# ms110d-cpp

A C++ implementation of the MIL-STD-188-110D Appendix D 3 kHz serial-tone HF modem, built to work on air with the C# reference, [M0LTE.Ms110d](https://github.com/M0LTE/M0LTE.Ms110d). Bits in, 9600 Hz audio out, and back again: no sound card, no KISS, no host application. It is C++20 with no exceptions, no RTTI and no allocation after start-up, so the same code builds for a PC and for a Cortex-M7, and there is a small C interface for other languages.

**Status: transmit and receive work, bit for bit with the C# reference in the default build; not released yet.** The plan is in [docs/plan.md](docs/plan.md) and the work is tracked in the [issues](https://github.com/M0LTE/ms110d-cpp/issues).

## What it does

- Waveforms WN0 to WN8 and WN13 (75 to 6400 bps), all four interleavers, K7 and K9 codes. See [docs/waveforms.md](docs/waveforms.md).
- Transmit: payload bits to audio, in any chunk size.
- Receive: audio in any chunk size to decoded bursts, by polling for events. The receiver finds the burst and follows whatever the preamble announces.
- Single or double precision. The default (double) build decodes the same bits as the C# receiver.

## Install

You need CMake 3.22 and a C++20 compiler. Pick one.

**FetchContent.** There is no release tag yet, so pin a commit:

```cmake
include(FetchContent)
FetchContent_Declare(m0lte-ms110d
  GIT_REPOSITORY https://github.com/M0LTE/ms110d-cpp.git
  GIT_TAG main)   # use a full commit SHA, or a release tag once there is one
FetchContent_MakeAvailable(m0lte-ms110d)
target_link_libraries(my_app PRIVATE m0lte::ms110d)
```

**Git submodule.**

```sh
git submodule add https://github.com/M0LTE/ms110d-cpp.git third_party/ms110d-cpp
```

```cmake
add_subdirectory(third_party/ms110d-cpp)
target_link_libraries(my_app PRIVATE m0lte::ms110d)
```

**Installed copy.**

```sh
cmake -S . -B build && cmake --build build -j4
cmake --install build --prefix /opt/ms110d
```

```cmake
find_package(m0lte-ms110d CONFIG REQUIRED)   # add -DCMAKE_PREFIX_PATH=/opt/ms110d
target_link_libraries(my_app PRIVATE m0lte::ms110d)
```

Used as a subproject, only the library is built (no tests). Options: `-DMS110D_PRECISION=single` for single precision (double is the default), `-DMS110D_BUILD_TESTS=OFF` for the library alone. To build and test this repository: `cmake -S . -B build`, `cmake --build build -j4`, `ctest --test-dir build`.

The C interface is the shared library `m0lte::ms110d_c` (header `m0lte/ms110d/ms110d.h`). Use it from C, .NET, Python and so on. The C++ API is the static library `m0lte::ms110d`.

## Quick start: transmit and receive in C++

Payload is one bit per byte (0 or 1). Both snippets are compiled and run by the tests (`tests/docs_examples/quickstart.cpp`), so they work.

```cpp
// Payload bits (one per byte, 0 or 1) in, 9600 Hz audio out.
std::vector<float> transmit(const std::vector<std::uint8_t>& payload_bits) {
    TxSettings settings;                 // WN 6, short interleaver, K 7, and so on
    settings.waveform_number = 4;        // 1200 bps BPSK

    const std::size_t max_bits = 4000;   // the largest payload this transmitter will carry
    std::size_t bytes = 0;
    if (Transmitter::storage_bytes(settings, max_bits, bytes) != Status::Ok) return {};
    std::vector<std::uint8_t> memory(bytes);  // or a static array: the library never allocates

    Transmitter tx;
    if (tx.configure(settings, max_bits, memory) != Status::Ok) return {};
    if (tx.push_bits(payload_bits) != Status::Ok) return {};  // more than max_bits in all
    if (tx.end_payload() != Status::Ok) return {};

    std::size_t total = 0;
    tx.audio_samples(payload_bits.size(), total);  // the whole burst's length, known up front
    std::vector<float> audio(total);
    std::size_t have = 0;
    while (!tx.done()) {
        std::size_t got = 0;
        if (tx.read(std::span(audio).subspan(have), got) != Status::Ok) return {};  // any chunk size
        have += got;
    }
    return audio;
}
```

```cpp
// 9600 Hz audio in, the first decoded burst's payload bits out (one per byte).
std::vector<std::uint8_t> receive(std::span<const float> audio) {
    RxOptions options;                   // the same defaults as the C# demodulator
    options.max_interleaver = InterleaverKind::Short;  // 4.1 MiB; Long accepts everything in 58.2 MiB
    std::size_t bytes = 0;
    if (Receiver::storage_bytes(options, bytes) != Status::Ok) return {};
    std::vector<std::uint8_t> memory(bytes);  // or a static array: the library never allocates

    Receiver rx;
    if (rx.configure(options, memory) != Status::Ok) return {};

    std::vector<std::uint8_t> payload;
    std::size_t done = 0;
    while (done < audio.size()) {
        std::size_t used = 0;
        if (rx.process(audio.subspan(done), used) != Status::Ok) break;
        done += used;                    // it stops right after a sample that raised events
        for (RxEvent e = rx.next_event(); e.kind != RxEventKind::None; e = rx.next_event()) {
            if (e.kind == RxEventKind::BurstStarted) payload.clear();  // e.waveform, e.interleaver, ...
            if (e.kind == RxEventKind::BlockDecoded) payload.insert(payload.end(), e.bits.begin(), e.bits.end());
            if (e.kind == RxEventKind::BurstCompleted) {
                payload.resize(static_cast<std::size_t>(e.payload_bits));  // the bits before the EOM
                return payload;
            }
        }
    }
    return {};
}
```

Drain the events after every `process`, even one that used all its audio. More in [docs/transmitting.md](docs/transmitting.md) and [docs/receiving.md](docs/receiving.md).

## Quick start: transmit and receive in C

The same thing through `ms110d.h` (`tests/docs_examples/quickstart_c.c`). Here the library allocates the handle once, in `create`; for no heap at all see `ms110d_tx_init` and `ms110d_rx_init` in the header.

```c
/* Payload bits (one per byte, 0 or 1) in, 9600 Hz audio out. Returns the sample count, 0 on
   failure; the caller frees *audio. */
static uint32_t transmit(const uint8_t* bits, uint32_t count, float** audio) {
    ms110d_tx_settings settings;
    ms110d_tx_settings_default(&settings);     /* WN 6, short interleaver, K 7, and so on */
    settings.waveform_number = 4;              /* 1200 bps BPSK */

    ms110d_tx* tx = NULL;
    if (ms110d_tx_create(&settings, count, &tx) != MS110D_OK) return 0;  /* count: the largest payload */
    uint32_t total = 0;
    float* out = NULL;
    if (ms110d_tx_push_bits(tx, bits, count) != MS110D_OK) goto fail;
    if (ms110d_tx_end_payload(tx) != MS110D_OK) goto fail;
    if (ms110d_tx_audio_length(tx, count, &total) != MS110D_OK) goto fail;  /* known up front */

    out = (float*)malloc(total * sizeof *out);
    if (out == NULL) goto fail;
    uint32_t have = 0;
    while (have < total) {
        uint32_t got = 0;
        if (ms110d_tx_read(tx, out + have, total - have, &got) != MS110D_OK || got == 0) goto fail;
        have += got;                           /* any chunk size, down to one sample */
    }
    ms110d_tx_destroy(tx);
    *audio = out;
    return total;
fail:
    free(out);
    ms110d_tx_destroy(tx);
    return 0;
}
```

```c
/* 9600 Hz audio in, the first decoded burst's payload bits out (one per byte, up to cap).
   Returns the bit count, 0 if no burst was received. */
static uint32_t receive(const float* samples, uint32_t count, uint8_t* payload, uint32_t cap) {
    ms110d_rx_options options;
    ms110d_rx_options_default(&options);       /* the same defaults as the C# demodulator */
    options.max_interleaver = MS110D_INTERLEAVER_SHORT;  /* 4.1 MiB; long accepts everything in 58.2 MiB */
    ms110d_rx* rx = NULL;
    if (ms110d_rx_create(&options, &rx) != MS110D_OK) return 0;

    static uint8_t bits[MS110D_RX_MAX_BLOCK_BITS];  /* room for the longest block */
    uint32_t filled = 0, result = 0, done = 0;
    while (done < count && result == 0) {
        uint32_t used = 0;
        if (ms110d_rx_process(rx, samples + done, count - done, &used) != MS110D_OK) break;
        done += used;                          /* it stops right after a sample that raised events */
        for (;;) {
            ms110d_rx_event ev;
            ev.struct_size = sizeof ev;
            if (ms110d_rx_next_event(rx, &ev, bits, sizeof bits, NULL, 0) != MS110D_OK) break;
            if (ev.kind == MS110D_RX_EVENT_NONE) break;
            if (ev.kind == MS110D_RX_EVENT_BURST_STARTED) filled = 0;
            if (ev.kind == MS110D_RX_EVENT_BLOCK_DECODED) {
                for (uint32_t i = 0; i < ev.length && filled < cap; ++i) payload[filled++] = bits[i];
            }
            if (ev.kind == MS110D_RX_EVENT_BURST_COMPLETED) {
                result = (uint32_t)ev.payload_bits < filled ? (uint32_t)ev.payload_bits : filled;
            }
        }
    }
    ms110d_rx_destroy(rx);
    return result;
}
```

## More

- [Waveforms](docs/waveforms.md): the modes, data rates, interleaver delays and target SNRs.
- [Transmitting](docs/transmitting.md) and [receiving](docs/receiving.md): settings, memory, events, statistics.
- [Microcontroller](docs/microcontroller.md): building for a Cortex-M7, and where that has got to.
- [Accepted deviations](docs/deviations.md) from the C# reference.
- [Versioning](docs/versioning.md) and the [changelog](CHANGELOG.md).
- For contributors: [docs/dev](docs/dev) (C interface conventions, the API lock, the allocation guard, interop tests, the C# reference files).

## Compatibility, proven by tests

The two implementations must agree exactly on the bits on the wire, closely on the transmitted audio, and within 0.5 dB on receive performance, on simulated HF channels and on real off-air recordings. Any difference we keep on purpose is written down as a named test. See [docs/plan.md](docs/plan.md#how-we-prove-compatibility).

## Licence

Copyright (C) 2026 Thomas Fanning.

The code is public under AGPL-3.0-or-later; see [LICENSE](LICENSE). If the AGPL does not suit your use, commercial licences are available from the copyright holder, Thomas Fanning. To ask, open a GitHub issue titled "Commercial licence enquiry". Because of that, outside contributions can only be accepted with a contributor licence agreement; please open an issue before writing code. See [PROVENANCE.md](PROVENANCE.md) for where the code comes from.
