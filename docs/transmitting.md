# Transmitting

Payload bits in, 9600 Hz audio out, one burst at a time. The quick start is in the [README](../README.md); this page has the details.

## Settings

`TxSettings` is the C# `Ms110dTxSettings` with the same defaults.

| Field | Default | Notes |
|---|---|---|
| `waveform_number` | 6 | 0 to 8 or 13: see [waveforms](waveforms.md) |
| `interleaver` | `Short` | `UltraShort`, `Short`, `Medium`, `Long`. Longer is more robust to fading and slower to deliver |
| `constraint_length` | 7 | 7 or 9 |
| `preamble_superframes` | 3 | 1 to 32; each is 240 ms |
| `tlc_blocks` | 0 | 0 to 255 blocks of the optional TLC section (transmit level control); 0 leaves it out |
| `append_eom` | true | the end of message word; the receiver uses it to cut the payload |
| `append_eot` | true | extends the last mini-probe as an end marker |
| `amplitude` | 0.5 | linear scale of the audio, so peaks stay under 1.0 |
| `roll_off` | 0.35 | 0.05 to 1 |

Bad values give `Status::InvalidArgument`; a waveform and interleaver the standard has no row for (WN0 ultra-short) gives `Unsupported`.

## Steps

1. `Transmitter::storage_bytes(settings, max_bits, bytes)`: how much memory this transmitter needs for payloads up to `max_bits` (at most 16,777,216).
2. `configure(settings, max_bits, memory)`: the memory is yours (a vector, a static array, the stack) and must stay put while the transmitter is in use.
3. `push_bits(bits)` as often as you like, then `end_payload()`. Bits are one per byte, 0 or 1.
4. `audio_samples(count, n)` gives the burst length, which depends only on the number of bits. `read(buffer, got)` fills your buffer with the audio until `done()`.
5. `begin()` to start the next burst. Calling things in the wrong order gives `Status::State`.

For the whole burst in one call use `modulate(bits, audio, written)`. If `audio` is too short it returns `BufferTooSmall` and sets `written` to the length needed, so calling it first with an empty span tells you.

## Chunked audio

`read` takes any chunk size, down to one sample, and the burst is never held whole, so you can feed a sound card or DMA buffer as it is made. The chunk size never changes the audio. Each new interleaver block is coded when its first sample is wanted, so some reads cost more than others; real-time players should read ahead into their own buffer. A read of 0 samples only ever means the burst has finished (an empty buffer is an error).

This one uses static memory and 480 sample chunks:

```cpp
// A transmitter in static memory, played out in 480 sample chunks (50 ms at 9600 Hz).
constexpr std::size_t max_bits = 2000;
alignas(8) static std::array<std::uint8_t, 16 * 1024> tx_memory;  // WN 4, short interleaver needs about 7.3 KiB

bool send(const std::vector<std::uint8_t>& payload_bits) {
    TxSettings settings;
    settings.waveform_number = 4;
    static Transmitter tx;
    if (tx.configure(settings, max_bits, tx_memory) != Status::Ok) return false;  // too small is BufferTooSmall
    if (tx.push_bits(payload_bits) != Status::Ok || tx.end_payload() != Status::Ok) return false;
    std::array<float, 480> chunk;
    while (!tx.done()) {
        std::size_t got = 0;
        if (tx.read(chunk, got) != Status::Ok) return false;
        play(chunk.data(), got);        // hand it to the sound card or the DMA buffer
    }
    return true;
}
```

## Memory

Everything is in the one block you give `configure`, and nothing is allocated, ever. It holds a small fixed part, two work buffers that grow with the interleaver and the waveform, and your payload, packed at one byte per 8 bits. In the default build, in KiB for an empty payload (add 1 KiB per 8000 bits):

| | Ultra-short | Short | Medium | Long |
|---|---|---|---|---|
| WN1 | 5.7 | 6.8 | 11.3 | 29.3 |
| WN4 | 5.8 | 7.1 | 12.3 | 33.3 |
| WN6 | 6.6 | 10.3 | 25.3 | 85.3 |
| WN8 | 7.8 | 15.3 | 45.3 | 165.3 |

WN0 needs 5.5 KiB (short) to 7.6 KiB (long). The single precision build needs about 0.5 KiB less. Ask `storage_bytes` rather than relying on these.

## Precision

The default build is double precision and makes the same audio as the C# modulator to within 1e-6 per sample. A single precision build (`-DMS110D_PRECISION=single`) has the same API and is meant for processors without a fast double FPU; its audio was measured within 2e-8 of the C#'s, which is one float step. The audio you get is `float` either way. Do not mix the two: a program built for one precision fails to link against a library built for the other.

## From C

`ms110d_tx_*` in `ms110d.h`, with the same steps: `ms110d_tx_settings_default`, then `ms110d_tx_create` (the library allocates) or `ms110d_tx_memory_size` plus `ms110d_tx_init` (your memory), `ms110d_tx_push_bits`, `ms110d_tx_end_payload`, `ms110d_tx_read`, or `ms110d_tx_modulate` for a whole burst. Status codes are the same numbers; `ms110d_status_string` turns one into text. The conventions are in [dev/c-interface.md](dev/c-interface.md).
