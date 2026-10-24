// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later

/* Compiled as C99: the test-only hook header is valid C, and each hook rejects bad
 * arguments without writing, as docs/dev/c-interface.md requires. The comparisons that
 * matter are in tests/interop (against the C#) and tests/layer1_reference_test.cpp. */
#include "ms110d_testhooks.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define EXPECT(cond)                                                       \
    do {                                                                   \
        if (!(cond)) {                                                     \
            printf("FAILED line %d: %s\n", __LINE__, #cond);               \
            ++failures;                                                    \
        }                                                                  \
    } while (0)

int main(void) {
    uint8_t info[16];
    uint8_t coded[32];
    uint8_t out[64];
    uint32_t len = 99;
    for (int i = 0; i < 16; ++i) info[i] = (uint8_t)(i & 1);

    EXPECT(ms110d_th_encode(7, info, 16, coded, 32, &len) == MS110D_OK && len == 32);
    EXPECT(ms110d_th_encode(8, info, 16, coded, 32, &len) == MS110D_ERR_INVALID_ARGUMENT);
    EXPECT(ms110d_th_encode(7, NULL, 16, coded, 32, &len) == MS110D_ERR_INVALID_ARGUMENT);
    EXPECT(ms110d_th_encode(7, info, 16, coded, 31, &len) == MS110D_ERR_BUFFER_TOO_SMALL && len == 32);
    EXPECT(ms110d_th_encode(7, info, 3, coded, 32, &len) == MS110D_ERR_INVALID_ARGUMENT);

    EXPECT(ms110d_th_puncture(7, 1, 2, coded, 32, out, 64, &len) == MS110D_OK && len == 32);
    EXPECT(ms110d_th_puncture(7, 5, 7, coded, 32, out, 64, &len) == MS110D_ERR_UNSUPPORTED);
    EXPECT(ms110d_th_puncture(7, 1, 2, coded, 32, out, 8, &len) == MS110D_ERR_BUFFER_TOO_SMALL);
    EXPECT(ms110d_th_puncture(7, 0, 2, coded, 32, out, 64, &len) == MS110D_ERR_INVALID_ARGUMENT);

    {
        const uint8_t payload[3] = {1, 0, 1};
        EXPECT(ms110d_th_frame(payload, 3, 0, 8, out, 64, &len) == MS110D_OK && len == 8);
        EXPECT(out[0] == 1 && out[1] == 0 && out[2] == 1 && out[3] == 0);
        EXPECT(ms110d_th_frame(payload, 3, 1, 8, out, 64, &len) == MS110D_OK && len == 40);
        EXPECT(ms110d_th_frame(payload, 3, 1, 8, out, 8, &len) == MS110D_ERR_BUFFER_TOO_SMALL && len == 40);
        EXPECT(ms110d_th_frame(payload, 3, 0, 0, out, 64, &len) == MS110D_ERR_INVALID_ARGUMENT);
    }

    {
        uint8_t chips[2048];
        EXPECT(ms110d_th_preamble(0, 1, 6, 1, 7, chips, 2048, &len) == MS110D_OK && len == 320);
        EXPECT(ms110d_th_preamble(0, 1, 6, 1, 7, chips, 10, &len) == MS110D_ERR_BUFFER_TOO_SMALL && len == 320);
        EXPECT(ms110d_th_preamble(0, 0, 6, 1, 7, chips, 2048, &len) == MS110D_ERR_INVALID_ARGUMENT);
        EXPECT(ms110d_th_preamble(0, 1, 6, 4, 7, chips, 2048, &len) == MS110D_ERR_INVALID_ARGUMENT);
    }

    /* Increment 3 on 8 bits: bit n lands at (3n) mod 8. */
    {
        const uint8_t in[8] = {0, 1, 2, 3, 4, 5, 6, 7};
        const uint8_t want[8] = {0, 3, 6, 1, 4, 7, 2, 5};
        EXPECT(ms110d_th_interleave(8, 3, in, 8, out, 64) == MS110D_OK);
        EXPECT(memcmp(out, want, 8) == 0);
        EXPECT(ms110d_th_interleave(8, 4, in, 8, out, 64) == MS110D_ERR_INVALID_ARGUMENT);
        EXPECT(ms110d_th_interleave(8, 3, in, 7, out, 64) == MS110D_ERR_INVALID_ARGUMENT);
        EXPECT(ms110d_th_interleave(8, 3, in, 8, out, 7) == MS110D_ERR_BUFFER_TOO_SMALL);
    }

    {
        const uint8_t bits[4] = {1, 0, 0, 1};
        EXPECT(ms110d_th_transcode(6, bits, 4, out, 64, &len) == MS110D_OK && len == 2);
        EXPECT(ms110d_th_transcode(0, bits, 4, out, 64, &len) == MS110D_ERR_UNSUPPORTED);
        EXPECT(ms110d_th_transcode(7, bits, 4, out, 64, &len) == MS110D_ERR_INVALID_ARGUMENT);
        EXPECT(ms110d_th_transcode(6, bits, 4, out, 1, &len) == MS110D_ERR_BUFFER_TOO_SMALL);
    }

    {
        /* The first symbol of a frame is scrambled by the initial register (1): 8PSK 0 -> 1. */
        const uint8_t zeros[4] = {0, 0, 0, 0};
        EXPECT(ms110d_th_scramble(7, 4, zeros, 4, out, 64) == MS110D_OK && out[0] == 1);
        EXPECT(ms110d_th_scramble(0, 4, zeros, 4, out, 64) == MS110D_ERR_UNSUPPORTED);
        EXPECT(ms110d_th_scramble(7, 0, zeros, 4, out, 64) == MS110D_ERR_INVALID_ARGUMENT);
        EXPECT(ms110d_th_scramble(7, 4, zeros, 4, out, 3) == MS110D_ERR_BUFFER_TOO_SMALL);
    }

    {
        uint8_t a[40];
        uint8_t b[40];
        EXPECT(ms110d_th_trinomial(10, 40, a, 40) == MS110D_OK);
        EXPECT(ms110d_th_trinomial(10, 10, b, 40) == MS110D_OK);
        EXPECT(memcmp(a, b, 10) == 0 && memcmp(a + 10, b, 10) == 0 && memcmp(a + 30, b, 10) == 0);
        EXPECT(ms110d_th_trinomial(0, 4, a, 40) == MS110D_ERR_INVALID_ARGUMENT);
        EXPECT(ms110d_th_trinomial(10, 41, a, 40) == MS110D_ERR_BUFFER_TOO_SMALL);
    }

    if (failures != 0) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("ok\n");
    return 0;
}
