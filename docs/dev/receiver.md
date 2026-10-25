# Receiver

`m0lte::ms110d::Receiver` (`include/m0lte/ms110d/receiver.hpp`, `src/receiver.cpp`) is the public receive path: 9600 Hz audio in, decoded bursts out, for every waveform (WN 0 to 8 and 13), interleaver and code. It is the C# `Ms110dDemodulator` with its options (`RxOptions`, the C# `Ms110dDemodOptions` with the same defaults) and its events, so a caller can swap one for the other. The C interface wraps it as `ms110d_rx_*` (docs/dev/c-interface.md). A short example is in the README. Story 27.

## What it composes

One shared front end and ring (`src/rx_stream.hpp`, below) feeds the internal stages: the preamble detector (docs/dev/preamble-detector.md), the WN 0 receiver (docs/dev/wn0-receiver.md) and the equaliser path with its turbo, salvage and MFB stages (docs/dev/dfe-receiver.md). The receiver drives them a T/2 sample at a time in the order of the C# `PushT2`: the terminate command first, then the detector (its running energies always, the search while searching), then the burst's stage. Around them it does what the C# does around its stages: the lock (the C# `Lock` becomes set and the search stops), the hand-over at the detection, the WN 0 block end (decode, EOM scan over the burst's bits, block limit), the terminate command and the return to the search after every end.

## Shared front end

The C# has one mixer, one receive filter and one 65536 sample ring that every stage reads. The ports of the detector, the WN 0 receiver and the equaliser path each had their own; they now all read one `RxStream`, which owns the front end and a ring in caller storage and is driven by whoever owns it. Each internal stage gets `configure(..., RxStream&)` and an `on_t2()` hook (the WN 0 receiver is stepped instead); `StandalonePreambleDetector`, `StandaloneWn0Receiver` and `StandaloneDfeReceiver` carry their own stream for tests and tools that use one stage on its own. `tests/support/rx_driver` now runs the detector and the equaliser path on one stream too, so every existing reference, corpus and dump test exercises the shared front end.

The ring is the longest look-back of any reader: 16384 samples (the detector's) up to the medium interleaver, and the equaliser path's 65536 for the long one. Against three separate front ends this saves the equaliser path's ring (64 KB with the short interleaver, 512 KB with the long) and the WN 0 receiver's (64 KB in the double build, 32 KB in the single), and two of the three filter computations per sample.

The detector and the equaliser path read the same single precision samples as before, so the default build is still the C# to the bit on every reference, corpus and dump test. The WN 0 receiver used to filter in the build's precision; it now reads the shared single precision samples, which is what the C# does (its ring is `Complex32`). In the double build its numbers therefore moved, towards the C#'s; its tests compare decisions exactly and soft values with a tolerance, and all pass unchanged.

## Events

`process(audio, consumed)` takes any number of samples and stops right after a sample that raised events; `next_event()` returns them in order, by value, until one of kind `None`. With events queued `process` takes nothing and returns `Status::State`. Kinds, in the C#'s order within a sample:

| Kind | C# | Carries |
| --- | --- | --- |
| `BurstStarted` | `Lock` set | waveform, interleaver, K, carrier offset after the first fit |
| `FirstPassSoft` | `FirstPassBlockLlrs` | block index, soft values (wire order, positive for 0); only with `soft_decisions` |
| `TurboSoft` | `TurboBlockLlrs` | block index, the last turbo iterate's soft values; only with `soft_decisions`, only when the turbo loop ran and was not aborted |
| `BlockDecoded` | `BlockDecoded` | block index, info bits |
| `BurstCompleted` | `BurstCompleted` | reason (the C# `Ms110dBurstEndReason`, same values), blocks, payload length |

Every event has `sample`, the input samples taken when it was raised, and the burst's waveform, interleaver, K and carrier offset. The payload is the burst's block bits in order cut to `payload_bits` (the C# `PayloadBits`: bits before the EOM, or all of them without one); it is not repeated in the completed event. The spans point into the receiver and stay valid until the next `process()` or `reset()`.

How the four open points of the C interface were settled is in docs/dev/c-interface.md: samples consumed, a sample position on every event, bits per block with the burst event closing the burst, soft events switched on at configure.

Also as the C#: `terminate()` (the burst in progress ends on the next sample with `Terminated`, or a preamble being read is dropped), `reset()`, `state()` (`Searching`, `ReadingPreamble`, `Tracking`, the C# `Ms110dRxState`; the C++ is `Tracking` from the lock, as the C#), `carrier_detect()` and `stats()` (the C#'s public counters). Not ported: the C#'s diagnostic events (`FrameDiagnostics`, `DataSymbolEqualized`, `PresenceTraced`) and instruments (genie, oracle and the rest).

## Options

| `RxOptions` | C# | Default |
| --- | --- | --- |
| `max_input_data_blocks` | `MaxInputDataBlocks` | 0 (no limit) |
| `sync_threshold` | `SyncThreshold` | 0.32 |
| `rls_forgetting_factor` (optional) | `RlsForgettingFactor` | unset: 1 - ln 10 / U |
| `disable_turbo` | `DisableTurbo` | false |
| `track_ridge` (optional) | `TrackRidge` | unset: per probe length |
| `turbo_noise_pricing` | `TurboNsegMode` ("spikeup", "off", "spike2s") | `SpikeUp` |
| `roll_off` | `RollOff` | 0.35 |
| `presence_release_off` | `PresenceReleaseOff` | false |
| `max_interleaver` | none | `Long` (accepts everything, as the C#) |
| `soft_decisions` | none (the C# events are always there) | false |

The four instrument options (`RlsForgettingFactor`, `TrackRidge`, `TurboNsegMode`, `PresenceReleaseOff`) are ported into the equaliser path where the C# applies them; with their defaults nothing changes. `max_interleaver` sizes the memory: a burst whose interleaver is longer is not received (it is counted in `RxStats::bursts_refused` and the search goes on at once, where the C# would have tracked it).

## Memory

Nothing is allocated, ever, and nothing throws: all memory is the block given to `configure()`, `storage_bytes()` long. The object is one pointer, movable and not copyable. The block holds the state (about 167 KB: the detector 19 KB, the equaliser path's object 123 KB, the WN 0 receiver and its block buffers 20 KB, the front end 4 KB), the ring, the equaliser path's storage, one Viterbi and one SISO decoder and the chain BCJR:

| `max_interleaver` | Ring | Equaliser path | Viterbi | SISO | Total |
| --- | --- | --- | --- | --- | --- |
| Ultra-short | 128 KB | 289 KB | 30 KB | 771 KB | 1431376 bytes (1.4 MB) |
| Short | 128 KB | 668 KB | 102 KB | 3.0 MB | 4252256 bytes (4.1 MB) |
| Medium | 128 KB | 2.1 MB | 390 KB | 12.0 MB | 15535760 bytes (14.8 MB) |
| Long | 512 KB | 8.0 MB | 1.5 MB | 48.0 MB | 61062992 bytes (58.2 MB) |

(double build; the chain BCJR is 17 KB in each.) The SISO decoder's forward metrics for a whole block dominate, as docs/dev/soft-decoders.md says; windowing it for a microcontroller is issue 44. The decoders now take caller storage (`storage_bytes()` and `configure()` on each, allocating nothing); the allocating constructors remain for the tests. Only one burst is decoded at a time, so there is one Viterbi and one SISO, set up for the burst's code at each detection in storage sized for K = 9, where the internal equaliser path had a pair for each code: that saves the K = 7 pair (about 0.8 MB with the short interleaver).

## Precision

What the stages do (docs/dev/dfe-receiver.md and the rest); the receiver adds no arithmetic of its own. Audio comes in as `float` whatever the build.

## Tests

`tests/receiver_test.cpp`:

- **Every reference case (122) through the public API.** WN 1 to 8 and 13 against the C# rx-turbo dump: the identified waveform, interleaver and K, every block's bits and sample position, the first-pass soft values (the hash of their float bits), the end (reason, blocks, payload length, sample) and the payload; and against the internal path (`RxDriver`) the blocks, soft values, positions and the lock sample exactly, in both builds. WN 0 against the C# WN 0 dump: every decoded bit and the block count. Measured: the default build gives the C#'s bits, ends and positions on every case and every one of the 214 first-pass soft value blocks bit for bit; the single build the same bits and ends (the clean cases take every decision the C# takes). Chunk sizes rotate from case to case (1 to 65536).
- **Chunk independence.** WN 0, 2 and 7 transmitter bursts: the whole event trace (kinds, positions, bits, soft values) is identical for chunks of 1, 2, 3, 7, 64, 577, 1000, 4800, 9600, a mix, and every size from 1 to 9600 in turn.
- **Zero allocation** over two whole bursts (WN 7 with the turbo, salvage and MFB stages, and WN 0), events included, plus terminate and reset.
- **Several bursts in one stream**, the receiver searching again after each; the block limit; turbo off (no `TurboSoft`); terminate mid-burst; a burst refused for its interleaver while the others decode.
- **Options and misuse**: every option out of range, the C# defaults, order (process before configure, events queued), storage too small or at an odd address, moving.

`tests/c_receiver_test.cpp` decodes every reference case through the C interface alone (create and caller memory alternately) against the C# dumps, and covers `struct_size` (too small, a larger struct with zeros or not), `reserved`, flags, buffers too small (the event stays queued and comes back with the length), events pending, terminate and the null cases. `tests/c_api_test.c` calls every receive function from plain C.

Through the C interface the receiver also runs in the .NET harnesses: the layer 3 C++ receiver slot (docs/dev/layer3.md) and the layer 4 corpus (docs/dev/corpus.md).
