// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later

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


/* Transmit settings: all 4 or 8 byte fields at fixed offsets, the same on every ABI. */
_Static_assert(sizeof(ms110d_tx_settings) == 48, "ms110d_tx_settings size");
_Static_assert(offsetof(ms110d_tx_settings, struct_size) == 0, "struct_size is first");
_Static_assert(offsetof(ms110d_tx_settings, waveform_number) == 4, "waveform_number offset");
_Static_assert(offsetof(ms110d_tx_settings, interleaver) == 8, "interleaver offset");
_Static_assert(offsetof(ms110d_tx_settings, constraint_length) == 12, "constraint_length offset");
_Static_assert(offsetof(ms110d_tx_settings, preamble_superframes) == 16, "preamble_superframes offset");
_Static_assert(offsetof(ms110d_tx_settings, tlc_blocks) == 20, "tlc_blocks offset");
_Static_assert(offsetof(ms110d_tx_settings, append_eom) == 24, "append_eom offset");
_Static_assert(offsetof(ms110d_tx_settings, append_eot) == 28, "append_eot offset");
_Static_assert(offsetof(ms110d_tx_settings, amplitude) == 32, "amplitude offset");
_Static_assert(offsetof(ms110d_tx_settings, reserved) == 36, "reserved offset");
_Static_assert(offsetof(ms110d_tx_settings, roll_off) == 40, "roll_off offset");
_Static_assert(sizeof(float) == 4 && sizeof(double) == 8, "audio is float, roll_off double");
_Static_assert(MS110D_INTERLEAVER_ULTRA_SHORT == 0 && MS110D_INTERLEAVER_SHORT == 1 &&
                   MS110D_INTERLEAVER_MEDIUM == 2 && MS110D_INTERLEAVER_LONG == 3,
               "interleaver values");
