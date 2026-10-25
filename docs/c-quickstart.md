# Quick start in C

The same transmit and receive as the [README](../README.md) quick start, through `m0lte/ms110d/ms110d.h` (link the shared library `m0lte::ms110d_c`; as a subproject, add `-DMS110D_BUILD_C_SHARED=ON` to get it). The code needs `<stdint.h>` and `<stdlib.h>` as well. Payload is one bit per byte (0 or 1) and the audio is 9600 Hz `float`. Here the library allocates the handle once, in `create`; for no heap at all see `ms110d_tx_init` and `ms110d_rx_init` in the header. The code is `tests/docs_examples/quickstart_c.c`, built and run by the tests.

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

Drain the events after every `ms110d_rx_process`, as in C++. More in [transmitting](transmitting.md#from-c) and [receiving](receiving.md#from-c).
