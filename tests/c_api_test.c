/* Compiled as C99, not C++: proves the header is valid C and every entry point links. */
#include <m0lte/ms110d/ms110d.h>

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                          \
        }                                                                        \
    } while (0)

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

    if (failures != 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    return 0;
}
