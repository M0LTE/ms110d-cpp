# Receiving

9600 Hz audio in, decoded bursts out, by polling for events. The quick start is in the [README](../README.md); this page has the details.

## Options

`RxOptions` is the C# `Ms110dDemodOptions` with the same defaults, plus two of ours. The defaults are what you want unless you know better.

| Field | Default | Notes |
|---|---|---|
| `max_interleaver` | `Long` | the longest interleaver accepted. **Sizes the memory**, see below. Ours |
| `soft_decisions` | false | also raise the soft value events. Costs no memory, only copying. Ours |
| `sync_threshold` | 0.32 | how sure the preamble search must be |
| `disable_turbo` | false | decode from the first pass alone: faster, less sensitive |
| `max_input_data_blocks` | 0 | stop after this many blocks; 0 is no limit |
| `roll_off` | 0.35 | receive filter, 0.05 to 1 |
| `presence_release_off` | false | never end a burst as `SignalAbsent` |
| `rls_forgetting_factor`, `track_ridge`, `turbo_noise_pricing` | unset, unset, `SpikeUp` | tuning knobs carried over from the C#; leave them alone |

A value out of range gives `Status::InvalidArgument` from `storage_bytes`.

## Chunked audio

`process(audio, used)` takes any number of samples, from one to a whole recording. It stops right after a sample that raised events and tells you how many it used, so you loop until your chunk is gone. The audio is `float` at 9600 Hz, whatever the precision.

## Events and the drain rule

Results come out of `next_event()` one at a time; it returns an event of kind `None` when the queue is empty. Nothing is called back.

| Event | When | What is in it |
|---|---|---|
| `BurstStarted` | a preamble was identified | `waveform`, `interleaver`, `constraint_length`, `cfo_hz` (carrier offset) |
| `BlockDecoded` | one interleaver block decoded | `block_index`, `bits` (one per byte) |
| `BurstCompleted` | the burst ended | `reason`, `blocks`, `payload_bits` |
| `BurstRefused` | a burst needs a longer interleaver than `max_interleaver` | `waveform`, `interleaver` |
| `FirstPassSoft`, `TurboSoft` | only with `soft_decisions` | `soft`: one value per interleaver bit, positive for bit 0 |

Every event also has `sample`, the number of input samples taken when it was raised.

The payload is the `bits` of all the `BlockDecoded` events, in order, cut to `payload_bits` (the bits before the end of message). Nothing holds a whole burst for you, so there is no maximum burst length. A block is at most 49152 bits. The `bits` and `soft` spans point into the receiver and are good until the next `process()` or `reset()`: copy what you want to keep.

**The drain rule.** With events still queued, `process()` takes no audio and returns `Status::State`. So call `next_event()` until it says `None` after every `process()`, also one that used all its audio, because its last sample may have raised events. The events of one sample are never more than four, so none are ever dropped.

```cpp
// Take every queued event. Call it after every process(), also one that used all its audio.
void drain(Receiver& rx) {
    for (RxEvent e = rx.next_event(); e.kind != RxEventKind::None; e = rx.next_event()) {
        switch (e.kind) {
        case RxEventKind::BurstStarted:
            std::printf("burst: WN %d, interleaver %d, K %d, %.1f Hz off\n",
                        e.waveform, static_cast<int>(e.interleaver), e.constraint_length, e.cfo_hz);
            ++g_started;
            break;
        case RxEventKind::BlockDecoded:
            ++g_blocks;                 // e.bits is valid until the next process() or reset()
            break;
        case RxEventKind::BurstCompleted:
            std::printf("done: %lld bits, %d blocks, reason %d\n",
                        static_cast<long long>(e.payload_bits), e.blocks, static_cast<int>(e.reason));
            ++g_completed;
            break;
        case RxEventKind::BurstRefused:
            std::printf("refused: WN %d needs a longer interleaver than this receiver holds\n", e.waveform);
            ++g_refused;
            break;
        default:
            break;                      // soft values, and kinds added later
        }
    }
}
```

```cpp
// Feed one chunk of audio of any size. False if the receiver reported an error.
bool feed(Receiver& rx, std::span<const float> chunk) {
    std::size_t done = 0;
    while (done < chunk.size()) {
        std::size_t used = 0;
        if (rx.process(chunk.subspan(done), used) != Status::Ok) return false;
        done += used;
        drain(rx);
    }
    return true;
}
```

`BurstCompleted::reason` is why it ended: `Eom` (the normal way), `MaxInputDataBlocks`, `Terminated` (you called `terminate()`), `SignalLost`, or `SignalAbsent` (the mini-probes looked like noise for 16 s). `reset()` goes back to the start of a stream; `state()` is `Searching`, `ReadingPreamble` or `Tracking`; `carrier_detect()` is true from a found preamble until the burst ends.

## Memory

All working memory is the one block you give `configure`, and nothing is allocated, ever. It is set by the longest interleaver you accept. A decoder has to hold a whole block, and the longest are very long:

| `max_interleaver` | Memory (double build) | Single build |
|---|---|---|
| `UltraShort` | 1.4 MiB | 1.4 MiB |
| `Short` | 4.1 MiB | 4.1 MiB |
| `Medium` | 14.8 MiB | 14.8 MiB |
| `Long` | 58.2 MiB | 58.2 MiB |

The default accepts everything, as the C# does, and needs the 58.2 MiB. If you only expect short bursts, say so. Ask `Receiver::storage_bytes(options, bytes)` for the exact figure. Most of it is the decoder's store for a block, which is why single precision saves almost nothing here. [Microcontroller](microcontroller.md) says what that means for small boards.

## Refused bursts

A burst whose interleaver is longer than `max_interleaver` is not received, for any waveform. The receiver raises `BurstRefused` at the moment it would have locked, counts it in `RxStats::bursts_refused`, and carries on searching. Nothing else of that burst is delivered.

## Statistics

`stats()` returns the counters since `configure()` or `reset()`: the C#'s public counters (the turbo outcomes `turbo_converged`, `turbo_reverted`, `turbo_salvaged`, `turbo_aborted`, `turbo_skipped`; `collapse_resolves`, `signal_absent_releases`, `agc_resolves`, `mfb_offered`, `mfb_selected`, `peak_search_metric`) and two of ours, `bursts_refused` and `samples_taken`.

```cpp
    const RxStats stats = rx.stats();
    std::printf("%lld samples, %lld bursts refused, %lld turbo passes converged\n",
                static_cast<long long>(stats.samples_taken), static_cast<long long>(stats.bursts_refused),
                static_cast<long long>(stats.turbo_converged));
```

## Precision

The default (double) build decodes the same bits as the C# receiver on every reference case. The single precision build (`-DMS110D_PRECISION=single`) has the same API, takes the same decisions on clean bursts, and may differ from the C# on a marginal fading burst. Use it where there is no fast double FPU, and expect it to matter for speed, not memory. A program built for one precision fails to link against a library built for the other.

On a PC the BPSK modes take about 6 to 10% of one core with turbo decoding. Not thread safe: use one receiver from one thread at a time.

## From C

`ms110d_rx_*` in `ms110d.h`: `ms110d_rx_options_default`, `ms110d_rx_create` (or `ms110d_rx_memory_size` plus `ms110d_rx_init` for your memory), `ms110d_rx_process`, `ms110d_rx_next_event`, `ms110d_rx_get_stats` and the rest. Events are copied into your buffers: one of `MS110D_RX_MAX_BLOCK_BITS` bytes holds any block, and `MS110D_ERR_BUFFER_TOO_SMALL` leaves the event queued and tells you the length. See [dev/c-interface.md](dev/c-interface.md).
