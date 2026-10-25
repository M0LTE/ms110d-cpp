# Microcontroller

The library is written to run without an operating system: no exceptions, no RTTI, no allocation, and all memory handed in by the caller. This page says honestly how far that has got. Short version: it compiles for a Cortex-M7 in CI, transmit would fit easily, and receive does not fit in the RAM of a typical part yet.

## Status

- **Builds:** CI compiles the library for a Cortex-M7 (`arm-none-eabi-gcc`, hard-float double FPU), in both precisions, and checks the C interface's types are the same size there.
- **Not run:** it has not run on real hardware. How much processor time it needs on a Cortex-M7 is not measured.
- **Fits:** transmit, in 5.5 to 165 KiB depending on waveform and interleaver (see [transmitting](transmitting.md)).
- **Does not fit yet:** receive. The smallest receiver, for the ultra-short interleaver only, is 1.4 MiB. The short interleaver is 4.1 MiB. Common Cortex-M7 parts have well under 1.4 MiB of RAM, though some can add external memory.

## Building

You need an `arm-none-eabi-gcc` toolchain. There is a toolchain file for a Cortex-M7 with the double FPU (`-mcpu=cortex-m7 -mfpu=fpv5-d16 -mfloat-abi=hard`, no exceptions, no RTTI):

```sh
cmake -S . -B build-m7 -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/cortex-m7.cmake \
  -DMS110D_BUILD_TESTS=OFF -DMS110D_INSTALL=OFF
cmake --build build-m7 -j4
```

That gives the static library `libms110d.a`; link it into your firmware and use the C++ API. A cross build leaves out the tests and the C shared library. Add `-DMS110D_PRECISION=single` for single precision, which is meant for a core with only a single-precision FPU (a Cortex-M4, say).

## The memory model

You give each transmitter or receiver one block of memory, `storage_bytes()` long, and it stays yours. Nothing is allocated after (or inside) `configure()`, which a test checks with an allocation guard. A static array is the usual home, and `configure()` returns `BufferTooSmall` if it is short, so a wrong figure shows up at start-up and not later:

```cpp
// A receiver for the ultra-short interleaver only, in a static array (no heap, no allocation).
// 1431392 bytes is what storage_bytes() says for these options in the default build; configure()
// answers BufferTooSmall if the array is too small, so a wrong figure is caught at start-up.
alignas(8) static std::array<std::uint8_t, 1431392> rx_memory;

bool start_receiver(Receiver& rx) {
    RxOptions options;
    options.max_interleaver = InterleaverKind::UltraShort;
    return rx.configure(options, rx_memory) == Status::Ok;
}
```

The transmit example in [transmitting](transmitting.md) is the same idea with static memory and chunked audio.

## Current memory figures

Receive, default build (single is within 6 KiB of these):

| `max_interleaver` | Total | Of which the SISO decoder |
|---|---|---|
| `UltraShort` | 1.4 MiB | 771 KiB |
| `Short` | 4.1 MiB | 3.0 MiB |
| `Medium` | 14.8 MiB | 12.0 MiB |
| `Long` | 58.2 MiB | 48.0 MiB |

The rest is a sample ring (128 KiB, 512 KiB for long), the equaliser's storage, the Viterbi decoder and about 163 KiB of fixed state.

## What is still to come

The big item is the soft decoder, which keeps its forward metrics for a whole interleaver block. Decoding in windows would cut that to a fraction, and is tracked as [issue 44](https://github.com/M0LTE/ms110d-cpp/issues/44). It is not done. Other ideas, also not done: size the equaliser for the waveforms actually accepted (a BPSK-only receiver needs no 16QAM storage), and keep some buffers in single precision.

Any shortcut that costs sensitivity (windowing, skipping the turbo pass, a lighter equaliser) will be an accepted deviation from the C#, written as a test with an issue number and listed in [deviations](deviations.md), so its cost is measured and cannot quietly grow. There are none yet.

The first on-hardware job is to measure processor load on an STM32H7. If it is too slow, the plan is single precision, then a lighter equaliser, then no turbo pass. The aim is the BPSK waveforms (WN1 to WN5) and the short interleaver first.
