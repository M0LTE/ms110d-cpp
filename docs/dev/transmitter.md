# Transmitter

`m0lte::ms110d::Transmitter` (`include/m0lte/ms110d/transmitter.hpp`, with `TxSettings` in `tx_settings.hpp`) is the public transmit path: payload bits in, 9600 Hz audio out. It is what the C# `Ms110dModulator.Modulate` does, as a stream. The C interface wraps it as `ms110d_tx_*` (docs/dev/c-interface.md). A short usage example is in the README.

## What it composes

For each burst, in order: the preamble (`src/preamble.*`), the initial mini-probe, then per interleaver block: framing with EOM and zero fill (`build_tx_block` in `src/framing.*`), the tail-biting encoder (`src/fec/`), Table D-L puncturing (`src/puncture.*`), the interleaver (`src/interleaver.hpp`), the burst symbols (`write_frame`, `write_walsh_symbol` in `src/burst.hpp`) and the modulator (`src/modulator.*`, docs/dev/modulator.md). Settings validation happens once, in `configure`, and returns a `Status`; nothing in the path throws.

## Memory

Everything lives in one block the caller hands to `configure(settings, max_payload_bits, storage)`; `Transmitter::storage_bytes` says how big. The `Transmitter` object itself is one pointer (movable, not copyable). The block holds, in order, the working state (`sizeof` 5320 bytes in the double build and 4792 in the single build on the Cortex-M7: the modulator is 2368 or 1840 of that, 320 symbols of buffer and the settings make the rest), two work buffers sized from the settings, and the payload.

- The payload is kept packed, 8 bits to a byte, because it is needed until its last block has been coded. This is the one part that grows with `max_payload_bits` (one byte per 8 bits).
- The two work buffers are reused through the pipeline: the first holds the framed bits and then the punctured bits, the second the coded bits and then the interleaved block. One block is coded at a time, whatever the payload length. The sizes follow the mode: `max(input, interleaver)` and `max(2 * input, interleaver)` bytes, from 192 and 192 bytes for WN 1 ultra-short up to 65536 and 98304 for WN 8 with the long interleaver.
- Nothing is allocated after `configure`, or inside it (`tests/transmitter_test.cpp` checks `begin`, `push_bits`, `end_payload`, `audio_samples` and `read` with the allocation guard, for the heaviest modes).

The C interface adds a 16 byte header to the block and offers create (the library allocates the block once) and init (caller memory).

`configure` checks everything before it touches the storage, so a failed call leaves a transmitter already running in that block undisturbed.

## One-shot

`modulate(bits, audio, written)` does begin, push, end and every read in one call, like the C# `Modulate`. With a short `audio` it returns `BufferTooSmall` and `written` is the length needed (call it with an empty span to ask). `read` with an empty span is `InvalidArgument`, so a read that gives 0 samples only ever means the burst has finished.

## Streaming

The audio is produced a unit at a time, on demand: a 32 chip run of the preamble, then a data frame with its mini-probe (a 32 chip Walsh symbol in WN 0), each into a 320 symbol buffer. `read` pushes buffered symbols into the modulator one at a time (each makes 4 samples ready) and copies the samples out as `float`, so a read can be any size down to one sample, and the burst is never held whole. When a block's first unit is needed the whole block is coded, so the cost of calls is uneven (the worst is WN 8 with the long interleaver, a 49152 bit block); a caller feeding a real-time device should read ahead into its own buffer. The chunk size never changes the audio (a test requires identical output for chunks of 1 to a million samples).

The length is fixed by the payload length alone, as in the C#: `audio_samples(n)` is 4 times the symbol count plus the pulse length, known before any bit is pushed.

## Floating point

The core computes in `real` as docs/dev/modulator.md describes; the audio crosses the boundary as `float`, rounded once at the end. Against the C# reference audio the worst error over the 122 cases is the modulator's (docs/dev/modulator.md), plus at most one float rounding in the double build, so the 1e-6 tolerance holds in both builds.

## Differences from the C#

- The payload comes in pieces and the end is signalled with `end_payload`; the C# takes it whole.
- Settings are validated with a status, not exceptions, and a non-finite amplitude is rejected (the C# does not check it). Waveform and interleaver pairs the C# modulator would fail on (WN 0 with the ultra-short interleaver) return `Status::Unsupported`.
- The payload is capped by `max_payload_bits` given at configure time (at most `max_payload_bits_limit`, 2^24 bits, so sample counts fit 32 bits).

## Tests

`tests/transmitter_test.cpp`: all 122 reference cases from payload bits to audio, within 1e-6 (chunk sizes rotate from 1 to 100000 and the payload arrives in two pieces on alternate cases); chunk independence; lengths against `audio_samples` for payloads around every block edge in every mode; the 32 bit limit; reuse; argument and state errors; zero allocations. `tests/c_transmitter_test.cpp` repeats the 122 cases through the C interface, alternating create and caller memory. `tests/c_api_test.c` covers the C functions and their errors in plain C.
