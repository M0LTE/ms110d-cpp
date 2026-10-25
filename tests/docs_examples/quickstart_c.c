// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later

/* The C quick start from docs/c-quickstart.md, compiled and run by ctest so it cannot rot. The
   regions between the snippet markers are checked to appear word for word in that page. */

#include <m0lte/ms110d/ms110d.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

// snippet-begin transmit_c
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
// snippet-end transmit_c

// snippet-begin receive_c
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
// snippet-end receive_c

int main(void) {
    enum { N = 1000, PAD = 9600 };
    uint8_t bits[N];
    for (uint32_t i = 0; i < N; ++i) bits[i] = (uint8_t)((i * 7 + i / 5) & 1);

    float* burst = NULL;
    const uint32_t total = transmit(bits, N, &burst);
    if (total == 0) return 1;
    float* audio = (float*)calloc(total + 2 * PAD, sizeof *audio);  /* a second of silence either side */
    if (audio == NULL) return 1;
    for (uint32_t i = 0; i < total; ++i) audio[PAD + i] = burst[i];
    free(burst);

    static uint8_t got[4000];
    const uint32_t n = receive(audio, total + 2 * PAD, got, sizeof got);
    free(audio);
    if (n != N) {
        fprintf(stderr, "quickstart_c: sent %d bits, got %u back\n", N, (unsigned)n);
        return 2;
    }
    for (uint32_t i = 0; i < N; ++i) {
        if (got[i] != bits[i]) return 3;
    }
    return 0;
}
