// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later

/* Compiled as C99, not C++: proves the header is valid C and every entry point links. */
#include <m0lte/ms110d/ms110d.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                          \
        }                                                                        \
    } while (0)

/* A burst through the handle, reading `chunk` samples at a time. Returns the sample count, or
 * -1 if something failed. audio must hold `capacity` samples. */
static long run_burst(ms110d_tx* tx, const uint8_t* bits, uint32_t count, uint32_t push_piece, uint32_t chunk,
                      float* audio, uint32_t capacity) {
    uint32_t at = 0, total = 0, got = 0, expected = 0, left = 0;
    if (ms110d_tx_begin(tx) != MS110D_OK) return -1;
    while (at < count) {
        uint32_t n = count - at < push_piece ? count - at : push_piece;
        if (ms110d_tx_push_bits(tx, bits + at, n) != MS110D_OK) return -1;
        at += n;
    }
    if (ms110d_tx_end_payload(tx) != MS110D_OK) return -1;
    if (ms110d_tx_audio_length(tx, count, &expected) != MS110D_OK) return -1;
    if (ms110d_tx_remaining(tx, &left) != MS110D_OK || left != expected) return -1;
    if (expected > capacity) return -1;
    for (;;) {
        uint32_t room = capacity - total < chunk ? capacity - total : chunk;
        if (ms110d_tx_read(tx, audio + total, room, &got) != MS110D_OK) return -1;
        if (got == 0) break;
        total += got;
    }
    if (total != expected) return -1;
    if (ms110d_tx_remaining(tx, &left) != MS110D_OK || left != 0) return -1;
    return (long)total;
}

static void test_transmit(void) {
    enum { MAXBITS = 400, CAP = 40000 };
    ms110d_tx_settings s;
    ms110d_tx* tx = NULL;
    ms110d_tx* tx2 = NULL;
    uint8_t bits[MAXBITS];
    static float whole[CAP];
    static float piece[CAP];
    uint32_t i, bytes = 0, alignment = 0, samples = 0, got = 77;
    long n1, n2;
    void* raw;
    uint8_t* aligned;
    double peak = 0;

    for (i = 0; i < MAXBITS; i++) bits[i] = (uint8_t)((i * 7u + (i >> 3)) & 1u);

    /* Defaults are the C# Ms110dTxSettings defaults. */
    CHECK(ms110d_tx_settings_default(NULL) == MS110D_ERR_INVALID_ARGUMENT);
    CHECK(ms110d_tx_settings_default(&s) == MS110D_OK);
    CHECK(s.struct_size == sizeof(ms110d_tx_settings));
    CHECK(s.waveform_number == 6 && s.interleaver == MS110D_INTERLEAVER_SHORT && s.constraint_length == 7);
    CHECK(s.preamble_superframes == 3 && s.tlc_blocks == 0 && s.append_eom == 1 && s.append_eot == 1);
    CHECK(s.amplitude == 0.5f && s.roll_off == 0.35 && s.reserved == 0);

    CHECK(ms110d_tx_create(&s, MAXBITS, NULL) == MS110D_ERR_INVALID_ARGUMENT);
    CHECK(ms110d_tx_create(NULL, MAXBITS, &tx) == MS110D_ERR_INVALID_ARGUMENT);
    CHECK(tx == NULL);
    CHECK(ms110d_tx_create(&s, MS110D_TX_MAX_PAYLOAD_BITS + 1u, &tx) == MS110D_ERR_INVALID_ARGUMENT);
    CHECK(ms110d_tx_create(&s, MAXBITS, &tx) == MS110D_OK);
    CHECK(tx != NULL);

    /* The length is known before any bit is sent, and the audio is plausible. */
    CHECK(ms110d_tx_audio_length(tx, 100, &samples) == MS110D_OK);
    CHECK(samples > 9600u / 4u);
    n1 = run_burst(tx, bits, 100, 100, 100000, whole, CAP);
    CHECK(n1 == (long)samples);
    for (i = 0; i < samples; i++) {
        double a = fabs((double)whole[i]);
        if (a > peak) peak = a;
    }
    CHECK(peak > 0.1 && peak < 2.0);

    /* Chunk sizes and push pieces do not matter, and the handle is reusable. */
    n2 = run_burst(tx, bits, 100, 7, 1, piece, CAP);
    CHECK(n2 == n1 && memcmp(whole, piece, (size_t)n1 * sizeof(float)) == 0);
    n2 = run_burst(tx, bits, 100, 1, 4096, piece, CAP);
    CHECK(n2 == n1 && memcmp(whole, piece, (size_t)n1 * sizeof(float)) == 0);

    /* Caller memory gives the same audio, and needs no destroy. */
    CHECK(ms110d_tx_memory_size(&s, MAXBITS, NULL, &alignment) == MS110D_ERR_INVALID_ARGUMENT);
    CHECK(ms110d_tx_memory_size(&s, MAXBITS, &bytes, NULL) == MS110D_ERR_INVALID_ARGUMENT);
    CHECK(ms110d_tx_memory_size(NULL, MAXBITS, &bytes, &alignment) == MS110D_ERR_INVALID_ARGUMENT);
    CHECK(ms110d_tx_memory_size(&s, MAXBITS, &bytes, &alignment) == MS110D_OK);
    CHECK(bytes > MAXBITS && alignment >= 8u && (alignment & (alignment - 1u)) == 0u);
    raw = malloc((size_t)bytes + alignment);
    CHECK(raw != NULL);
    if (raw != NULL) {
        aligned = (uint8_t*)raw;
        while ((uintptr_t)aligned % alignment != 0) aligned++;
        CHECK(ms110d_tx_init(aligned, bytes, &s, MAXBITS, NULL) == MS110D_ERR_INVALID_ARGUMENT);
        CHECK(ms110d_tx_init(NULL, bytes, &s, MAXBITS, &tx2) == MS110D_ERR_BAD_MEMORY);
        CHECK(ms110d_tx_init(aligned, bytes - 1u, &s, MAXBITS, &tx2) == MS110D_ERR_BAD_MEMORY);
        CHECK(ms110d_tx_init(aligned + 1, bytes - 1u, &s, MAXBITS, &tx2) == MS110D_ERR_BAD_MEMORY);
        CHECK(tx2 == NULL);
        CHECK(ms110d_tx_init(aligned, bytes, &s, MAXBITS, &tx2) == MS110D_OK);
        n2 = tx2 != NULL ? run_burst(tx2, bits, 100, 100, 333, piece, CAP) : -1;
        CHECK(n2 == n1 && memcmp(whole, piece, (size_t)n1 * sizeof(float)) == 0);
        free(raw);
    }

    /* Order, limits and bad arguments. */
    CHECK(ms110d_tx_begin(tx) == MS110D_OK);
    CHECK(ms110d_tx_read(tx, piece, 10, &got) == MS110D_ERR_STATE && got == 0);
    CHECK(ms110d_tx_remaining(tx, &got) == MS110D_OK && got == 0);
    CHECK(ms110d_tx_push_bits(tx, bits, MAXBITS) == MS110D_OK);
    CHECK(ms110d_tx_push_bits(tx, bits, 1) == MS110D_ERR_BUFFER_TOO_SMALL);
    CHECK(ms110d_tx_push_bits(tx, NULL, 1) == MS110D_ERR_INVALID_ARGUMENT);
    CHECK(ms110d_tx_push_bits(tx, NULL, 0) == MS110D_OK);
    CHECK(ms110d_tx_push_bits(NULL, bits, 1) == MS110D_ERR_INVALID_ARGUMENT);
    CHECK(ms110d_tx_end_payload(tx) == MS110D_OK);
    CHECK(ms110d_tx_end_payload(tx) == MS110D_ERR_STATE);
    CHECK(ms110d_tx_push_bits(tx, bits, 1) == MS110D_ERR_STATE);
    CHECK(ms110d_tx_read(tx, NULL, 5, &got) == MS110D_ERR_INVALID_ARGUMENT);
    CHECK(ms110d_tx_read(tx, piece, 5, NULL) == MS110D_ERR_INVALID_ARGUMENT);
    CHECK(ms110d_tx_read(NULL, piece, 5, &got) == MS110D_ERR_INVALID_ARGUMENT && got == 0);
    CHECK(ms110d_tx_read(tx, NULL, 0, &got) == MS110D_ERR_INVALID_ARGUMENT && got == 0);
    CHECK(ms110d_tx_read(tx, piece, 0, &got) == MS110D_ERR_INVALID_ARGUMENT && got == 0);
    CHECK(ms110d_tx_read(tx, piece, 5, &got) == MS110D_OK && got == 5);
    CHECK(ms110d_tx_audio_length(NULL, 1, &samples) == MS110D_ERR_INVALID_ARGUMENT);
    CHECK(ms110d_tx_audio_length(tx, 1, NULL) == MS110D_ERR_INVALID_ARGUMENT);
    CHECK(ms110d_tx_audio_length(tx, MS110D_TX_MAX_PAYLOAD_BITS + 1u, &samples) == MS110D_ERR_INVALID_ARGUMENT);
    CHECK(ms110d_tx_remaining(NULL, &got) == MS110D_ERR_INVALID_ARGUMENT);
    CHECK(ms110d_tx_remaining(tx, NULL) == MS110D_ERR_INVALID_ARGUMENT);
    CHECK(ms110d_tx_begin(NULL) == MS110D_ERR_INVALID_ARGUMENT);
    CHECK(ms110d_tx_end_payload(NULL) == MS110D_ERR_INVALID_ARGUMENT);
    ms110d_tx_destroy(tx);
    ms110d_tx_destroy(NULL);

    /* The one-shot path equals the streamed one, and reports the length when short. */
    ms110d_tx_settings_default(&s);
    CHECK(ms110d_tx_create(&s, MAXBITS, &tx) == MS110D_OK);
    n1 = run_burst(tx, bits, 100, 100, 1000, whole, CAP);
    CHECK(n1 > 0);
    CHECK(ms110d_tx_modulate(tx, bits, 100, piece, CAP, &got) == MS110D_OK && (long)got == n1);
    CHECK(memcmp(whole, piece, (size_t)n1 * sizeof(float)) == 0);
    got = 0;
    CHECK(ms110d_tx_modulate(tx, bits, 100, NULL, 0, &got) == MS110D_ERR_BUFFER_TOO_SMALL && (long)got == n1);
    CHECK(ms110d_tx_modulate(tx, bits, 100, piece, (uint32_t)n1 - 1u, &got) == MS110D_ERR_BUFFER_TOO_SMALL && (long)got == n1);
    CHECK(ms110d_tx_modulate(tx, bits, MAXBITS + 1u, piece, CAP, &got) == MS110D_ERR_BUFFER_TOO_SMALL && got == 0);
    CHECK(ms110d_tx_modulate(tx, NULL, 1, piece, CAP, &got) == MS110D_ERR_INVALID_ARGUMENT);
    CHECK(ms110d_tx_modulate(tx, bits, 1, NULL, 1, &got) == MS110D_ERR_INVALID_ARGUMENT);
    CHECK(ms110d_tx_modulate(tx, bits, 1, piece, CAP, NULL) == MS110D_ERR_INVALID_ARGUMENT);
    CHECK(ms110d_tx_modulate(NULL, bits, 1, piece, CAP, &got) == MS110D_ERR_INVALID_ARGUMENT);
    ms110d_tx_destroy(tx);
    tx = NULL;

    /* Reserved must be zero. struct_size: bigger is fine only with zeros beyond what the library knows. */
    {
        struct {
            ms110d_tx_settings s;
            uint8_t extra[16];
        } big;
        memset(&big, 0, sizeof big);
        ms110d_tx_settings_default(&big.s);
        big.s.struct_size = (uint32_t)sizeof big;
        CHECK(ms110d_tx_create(&big.s, 10, &tx) == MS110D_OK);
        ms110d_tx_destroy(tx);
        tx = NULL;
        big.extra[15] = 1;
        CHECK(ms110d_tx_create(&big.s, 10, &tx) == MS110D_ERR_INVALID_ARGUMENT && tx == NULL);
        CHECK(ms110d_tx_memory_size(&big.s, 10, &bytes, &alignment) == MS110D_ERR_INVALID_ARGUMENT);
        big.s.struct_size = (uint32_t)sizeof big.s;  /* the extra bytes are then not part of the struct */
        CHECK(ms110d_tx_create(&big.s, 10, &tx) == MS110D_OK);
        ms110d_tx_destroy(tx);
        tx = NULL;
    }
    ms110d_tx_settings_default(&s);
    s.struct_size = (uint32_t)sizeof s - 1u;
    CHECK(ms110d_tx_create(&s, 10, &tx) == MS110D_ERR_INVALID_ARGUMENT && tx == NULL);
    ms110d_tx_settings_default(&s);
    s.reserved = 1;
    CHECK(ms110d_tx_create(&s, 10, &tx) == MS110D_ERR_INVALID_ARGUMENT && tx == NULL);
    CHECK(ms110d_tx_init(NULL, 0, &s, 10, &tx) == MS110D_ERR_INVALID_ARGUMENT);

    /* Bad settings. */
    tx = NULL;
    ms110d_tx_settings_default(&s);
    s.waveform_number = 9;
    CHECK(ms110d_tx_create(&s, 10, &tx) == MS110D_ERR_INVALID_ARGUMENT && tx == NULL);
    ms110d_tx_settings_default(&s);
    s.waveform_number = 0;
    s.interleaver = MS110D_INTERLEAVER_ULTRA_SHORT;
    CHECK(ms110d_tx_create(&s, 10, &tx) == MS110D_ERR_UNSUPPORTED && tx == NULL);
    ms110d_tx_settings_default(&s);
    s.interleaver = 4;
    CHECK(ms110d_tx_create(&s, 10, &tx) == MS110D_ERR_INVALID_ARGUMENT);
    s.interleaver = -1;
    CHECK(ms110d_tx_create(&s, 10, &tx) == MS110D_ERR_INVALID_ARGUMENT);
    ms110d_tx_settings_default(&s);
    s.append_eom = 2;
    CHECK(ms110d_tx_create(&s, 10, &tx) == MS110D_ERR_INVALID_ARGUMENT);
    ms110d_tx_settings_default(&s);
    s.constraint_length = 8;
    CHECK(ms110d_tx_create(&s, 10, &tx) == MS110D_ERR_INVALID_ARGUMENT);
    ms110d_tx_settings_default(&s);
    s.roll_off = 2.0;
    CHECK(ms110d_tx_create(&s, 10, &tx) == MS110D_ERR_INVALID_ARGUMENT);
    ms110d_tx_settings_default(&s);
    s.struct_size = 4;
    CHECK(ms110d_tx_create(&s, 10, &tx) == MS110D_ERR_INVALID_ARGUMENT);
    CHECK(tx == NULL);
}

/* The receive functions in plain C: defaults, both ways to make a handle, a burst from the
 * transmitter decoded through the event queue, and the null and bad-value cases. */
static void test_receive(void) {
    enum { BITS = 300 };
    ms110d_rx_options o;
    ms110d_rx* rx = NULL;
    ms110d_rx* built = NULL;
    ms110d_tx_settings s;
    ms110d_tx* tx = NULL;
    ms110d_rx_event ev;
    static float audio[200000];
    static uint8_t bits[4096];
    static uint8_t got[8192];
    uint8_t payload[BITS];
    uint32_t i, n = 0, written = 0, used = 0, at = 0, total = 0, bytes = 0, alignment = 0;
    int32_t state = -1;
    int started = 0, ended = 0, reason = -1;
    int64_t payload_bits = -1;
    void* raw;
    uint8_t* aligned;

    CHECK(ms110d_rx_options_default(&o) == MS110D_OK);
    CHECK(o.struct_size == sizeof(ms110d_rx_options));
    CHECK(o.max_interleaver == MS110D_INTERLEAVER_LONG);
    CHECK(o.sync_threshold == 0.32);
    CHECK(ms110d_rx_options_default(NULL) == MS110D_ERR_INVALID_ARGUMENT);
    o.max_interleaver = MS110D_INTERLEAVER_SHORT;

    CHECK(ms110d_rx_create(NULL, &rx) == MS110D_ERR_INVALID_ARGUMENT);
    CHECK(ms110d_rx_create(&o, NULL) == MS110D_ERR_INVALID_ARGUMENT);
    o.reserved = 1;
    CHECK(ms110d_rx_create(&o, &rx) == MS110D_ERR_INVALID_ARGUMENT);
    o.reserved = 0;
    o.disable_turbo = 7;
    CHECK(ms110d_rx_create(&o, &rx) == MS110D_ERR_INVALID_ARGUMENT);
    o.disable_turbo = 0;
    CHECK(rx == NULL);
    CHECK(ms110d_rx_create(&o, &rx) == MS110D_OK);
    CHECK(rx != NULL);

    CHECK(ms110d_rx_memory_size(&o, &bytes, &alignment) == MS110D_OK);
    CHECK(ms110d_rx_memory_size(&o, NULL, &alignment) == MS110D_ERR_INVALID_ARGUMENT);
    raw = malloc((size_t)bytes + alignment);
    CHECK(raw != NULL);
    aligned = (uint8_t*)raw + (alignment - (uintptr_t)raw % alignment) % alignment;
    CHECK(ms110d_rx_init(aligned, bytes - 1, &o, &built) == MS110D_ERR_BAD_MEMORY);
    CHECK(ms110d_rx_init(aligned, bytes, &o, NULL) == MS110D_ERR_INVALID_ARGUMENT);
    CHECK(ms110d_rx_init(aligned, bytes, &o, &built) == MS110D_OK);

    CHECK(ms110d_tx_settings_default(&s) == MS110D_OK);
    s.waveform_number = 0;
    CHECK(ms110d_tx_create(&s, BITS, &tx) == MS110D_OK);
    for (i = 0; i < BITS; ++i) payload[i] = (uint8_t)((i * 5u / 3u) & 1u);
    CHECK(ms110d_tx_audio_length(tx, BITS, &n) == MS110D_OK);
    CHECK(n + 9600 <= 200000);
    CHECK(ms110d_tx_modulate(tx, payload, BITS, audio, n, &written) == MS110D_OK);
    ms110d_tx_destroy(tx);
    total = n + 9600;

    CHECK(ms110d_rx_process(NULL, audio, 1, &used) == MS110D_ERR_INVALID_ARGUMENT);
    CHECK(ms110d_rx_process(built, NULL, 1, &used) == MS110D_ERR_INVALID_ARGUMENT);
    CHECK(ms110d_rx_process(built, audio, 1, NULL) == MS110D_ERR_INVALID_ARGUMENT);
    ev.struct_size = 8;
    CHECK(ms110d_rx_next_event(built, &ev, bits, sizeof bits, NULL, 0) == MS110D_ERR_INVALID_ARGUMENT);
    CHECK(ms110d_rx_next_event(NULL, &ev, bits, sizeof bits, NULL, 0) == MS110D_ERR_INVALID_ARGUMENT);
    CHECK(ms110d_rx_state(NULL, &state) == MS110D_ERR_INVALID_ARGUMENT);
    CHECK(ms110d_rx_terminate(NULL) == MS110D_ERR_INVALID_ARGUMENT);
    CHECK(ms110d_rx_reset(NULL) == MS110D_ERR_INVALID_ARGUMENT);

    n = 0;
    while (at < total && !ended) {
        if (ms110d_rx_process(built, audio + at, total - at, &used) != MS110D_OK) {
            CHECK(0);
            break;
        }
        at += used;
        for (;;) {
            ev.struct_size = sizeof ev;
            if (ms110d_rx_next_event(built, &ev, bits, sizeof bits, NULL, 0) != MS110D_OK) {
                CHECK(0);
                break;
            }
            if (ev.kind == MS110D_RX_EVENT_NONE) break;
            if (ev.kind == MS110D_RX_EVENT_BURST_STARTED) {
                started = 1;
                CHECK(ev.waveform_number == 0);
                CHECK(ms110d_rx_state(built, &state) == MS110D_OK);
                CHECK(state == MS110D_RX_STATE_TRACKING);
            }
            if (ev.kind == MS110D_RX_EVENT_BLOCK_DECODED && n + ev.length <= sizeof got) {
                memcpy(got + n, bits, ev.length);
                n += ev.length;
            }
            if (ev.kind == MS110D_RX_EVENT_BURST_COMPLETED) {
                ended = 1;
                reason = ev.end_reason;
                payload_bits = ev.payload_bits;
            }
        }
    }
    CHECK(started && ended);
    CHECK(reason == MS110D_END_EOM);
    CHECK(payload_bits == BITS);
    CHECK(memcmp(got, payload, BITS) == 0);
    CHECK(ms110d_rx_state(built, &state) == MS110D_OK);
    CHECK(state == MS110D_RX_STATE_SEARCHING);
    CHECK(ms110d_rx_terminate(built) == MS110D_OK);
    CHECK(ms110d_rx_reset(built) == MS110D_OK);

    ms110d_rx_destroy(rx);
    ms110d_rx_destroy(NULL);
    free(raw);
}

int main(void) {
    int32_t major = -1, minor = -1, patch = -1;
    char text[64];
    int32_t precision = 0;
    int32_t keep = 12345;

    CHECK(ms110d_abi_version() == MS110D_ABI_VERSION);

    CHECK(ms110d_version(&major, &minor, &patch) == MS110D_OK);
    sprintf(text, "%d.%d.%d", (int)major, (int)minor, (int)patch);
    CHECK(strcmp(text, MS110D_EXPECTED_VERSION) == 0);

    /* A null output is rejected and nothing is written. */
    CHECK(ms110d_version(NULL, &minor, &patch) == MS110D_ERR_INVALID_ARGUMENT);
    CHECK(ms110d_version(&keep, NULL, &patch) == MS110D_ERR_INVALID_ARGUMENT);
    CHECK(ms110d_version(&keep, &minor, NULL) == MS110D_ERR_INVALID_ARGUMENT);
    CHECK(keep == 12345);

    CHECK(ms110d_precision_bits(&precision) == MS110D_OK);
#ifdef MS110D_EXPECT_SINGLE
    CHECK(precision == MS110D_PRECISION_SINGLE);
#else
    CHECK(precision == MS110D_PRECISION_DOUBLE);
#endif
    CHECK(ms110d_precision_bits(NULL) == MS110D_ERR_INVALID_ARGUMENT);

    CHECK(strcmp(ms110d_status_string(MS110D_OK), "ok") == 0);
    CHECK(strcmp(ms110d_status_string(MS110D_ERR_BUFFER_TOO_SMALL), "buffer too small") == 0);
    CHECK(strcmp(ms110d_status_string(MS110D_ERR_OUT_OF_MEMORY), "out of memory") == 0);
    CHECK(strcmp(ms110d_status_string(MS110D_ERR_BAD_MEMORY), "supplied memory too small or misaligned") == 0);
    CHECK(strcmp(ms110d_status_string(999), "unknown status") == 0);
    CHECK(strcmp(ms110d_status_string(-1), "unknown status") == 0);

    test_transmit();
    test_receive();

    if (failures != 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    return 0;
}
