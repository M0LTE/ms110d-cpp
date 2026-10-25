# ms110d-cpp

A C++ implementation of the MIL-STD-188-110D Appendix D 3 kHz serial-tone HF modem, built to work on air with the C# reference, [M0LTE.Ms110d](https://github.com/M0LTE/M0LTE.Ms110d). Bits in, 9600 Hz audio out, and back again: no sound card, no KISS, no host application. It is C++20 with no exceptions, no RTTI and no allocation after start-up, so the same code builds for a PC and for a Cortex-M7, and there is a small C interface for other languages.

**Status: transmit and receive work and, in the default build, match the C# reference, apart from [one fault in the C# transmitter](#the-mini-probe-pattern). Not released yet.** The plan is in [docs/plan.md](docs/plan.md) and the work is tracked in the [issues](https://github.com/M0LTE/ms110d-cpp/issues).

## What it does

- Waveforms WN0 to WN8 and WN13 (75 to 6400 bps), all four interleavers, K7 and K9 codes. See [docs/waveforms.md](docs/waveforms.md).
- Transmit: payload bits to audio, in any chunk size.
- Receive: audio in any chunk size to decoded bursts, by polling for events. The receiver finds the burst and follows whatever the preamble announces.
- Single or double precision. The default (double) build decodes the same bits as the C# receiver.

## Install

You need CMake 3.22 and a C++20 compiler. Pick one.

**FetchContent.** Pin a commit (there is no release tag yet):

```cmake
include(FetchContent)
FetchContent_Declare(m0lte-ms110d
  GIT_REPOSITORY https://github.com/M0LTE/ms110d-cpp.git
  GIT_TAG <full-commit-sha>)
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
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DMS110D_BUILD_TESTS=OFF
cmake --build build -j4
cmake --install build --prefix /opt/ms110d
```

```cmake
find_package(m0lte-ms110d CONFIG REQUIRED)   # add -DCMAKE_PREFIX_PATH=/opt/ms110d
target_link_libraries(my_app PRIVATE m0lte::ms110d)
```

Build in Release: the receiver is slow without optimisation, and there is no default build type. Options: `-DMS110D_PRECISION=single` for single precision (double is the default), `-DMS110D_BUILD_TESTS=OFF` for the library alone. Used as a subproject (FetchContent or a submodule), only the C++ library is built: no tests, and no C library unless you add `-DMS110D_BUILD_C_SHARED=ON`. To build and test this repository: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release`, `cmake --build build -j4`, `ctest --test-dir build`.

The C++ API is the static library `m0lte::ms110d`. The C interface is the shared library `m0lte::ms110d_c` (header `m0lte/ms110d/ms110d.h`), for C, .NET, Python and so on; it is built and installed by default when this is the top-level project.

## Quick start: transmit and receive in C++

Payload is one bit per byte (0 or 1), and the audio is 9600 Hz `float`. Both snippets are compiled and run by the tests (`tests/docs_examples/quickstart.cpp`), so they work.

```cpp
#include <m0lte/ms110d/transmitter.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

using namespace m0lte::ms110d;

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
    if (tx.push_bits(payload_bits) != Status::Ok) return {};  // fails past max_bits
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
#include <m0lte/ms110d/receiver.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

using namespace m0lte::ms110d;

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

There is a C version of the same quick start in [docs/c-quickstart.md](docs/c-quickstart.md).

## More

- [Waveforms](docs/waveforms.md): the modes, data rates, interleaver delays and target SNRs.
- [C quick start](docs/c-quickstart.md).
- [Transmitting](docs/transmitting.md) and [receiving](docs/receiving.md): settings, memory, events, statistics.
- [Microcontroller](docs/microcontroller.md): building for a Cortex-M7, and where that has got to.
- [Accepted deviations](docs/deviations.md) from the C# reference.
- [Versioning](docs/versioning.md) and the [changelog](CHANGELOG.md).
- For contributors: [docs/dev](docs/dev) (C interface conventions, the API lock, the allocation guard, interop tests, the C# reference files).

## Compatibility, proven by tests

The two implementations must agree exactly on the bits on the wire, closely on the transmitted audio, and within 0.5 dB on receive performance, on simulated HF channels and on real off-air recordings. Any difference we keep on purpose is written down as a named test, and [there are none yet](docs/deviations.md). The one difference today is the C#'s fault, below. See [docs/plan.md](docs/plan.md#how-we-prove-compatibility).

## The mini-probe pattern

This library sends and expects the mini-probe pattern that MIL-STD-188-110D defines. The C# reference does not send it yet: up to 0.2.1 its transmitter shifts some mini-probes at the selected interleaver's boundary instead of the long interleaver's. The two patterns are the same on the long interleaver and on WN0, and differ on ultra-short, short and medium. What that means today:

- A C# 0.2.1 receiver decodes this library's bursts on every interleaver, because it accepts both patterns. Older C# receivers decode its ultra-short, short and medium bursts less well.
- This library decodes C# transmissions on the long interleaver as well as the C# receiver does, but its ultra-short, short and medium bursts less well. GB7RDG's bulletins are short-interleaver bursts from the C#: on 31 recorded ones this library decoded 95 of the 99 good frames that the C# receiver did.

This is a fault in the C# transmitter, not a difference we keep, so it is not listed as a deviation. The C# and GB7RDG are due to switch to the standard pattern; [issue 68](https://github.com/M0LTE/ms110d-cpp/issues/68) tracks it. The details are in [docs/dev/receiver.md](docs/dev/receiver.md#probe-pattern-issue-68).

## Licence

Copyright (C) 2026 Thomas Fanning.

The code is public under AGPL-3.0-or-later; see [LICENSE](LICENSE). If the AGPL does not suit your use, commercial licences are available from the copyright holder, Thomas Fanning. To ask, open a GitHub issue titled "Commercial licence enquiry". Because of that, outside contributions can only be accepted with a contributor licence agreement; please open an issue before writing code. See [PROVENANCE.md](PROVENANCE.md) for where the code comes from.
