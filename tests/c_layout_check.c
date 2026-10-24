/* Compile-time checks of the size of every type that crosses the C boundary.
 * Compiled as C11 for the host and, in CI, for the Cortex-M7 (where enums are one
 * byte), so a type that is not fixed-width fails the build there. Add each new
 * boundary struct and its size and offsets here. */
#include <m0lte/ms110d/ms110d.h>

#include <stddef.h>

_Static_assert(sizeof(ms110d_status) == 4, "ms110d_status must be 4 bytes");
_Static_assert(sizeof(int32_t) == 4, "int32_t");
_Static_assert(sizeof(uint32_t) == 4, "uint32_t");
_Static_assert(sizeof(MS110D_ABI_VERSION) == sizeof(int), "constants are plain ints");
_Static_assert(MS110D_PRECISION_SINGLE == 32 && MS110D_PRECISION_DOUBLE == 64, "precision values");

