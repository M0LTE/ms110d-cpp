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

/* Receiver options: 4 byte fields, then the two doubles at 48 and 56 on every ABI. */
_Static_assert(sizeof(ms110d_rx_options) == 64, "ms110d_rx_options size");
_Static_assert(offsetof(ms110d_rx_options, struct_size) == 0, "struct_size is first");
_Static_assert(offsetof(ms110d_rx_options, max_input_data_blocks) == 4, "max_input_data_blocks offset");
_Static_assert(offsetof(ms110d_rx_options, disable_turbo) == 8, "disable_turbo offset");
_Static_assert(offsetof(ms110d_rx_options, presence_release_off) == 12, "presence_release_off offset");
_Static_assert(offsetof(ms110d_rx_options, turbo_noise_pricing) == 16, "turbo_noise_pricing offset");
_Static_assert(offsetof(ms110d_rx_options, has_rls_forgetting_factor) == 20, "has_rls_forgetting_factor offset");
_Static_assert(offsetof(ms110d_rx_options, rls_forgetting_factor) == 24, "rls_forgetting_factor offset");
_Static_assert(offsetof(ms110d_rx_options, has_track_ridge) == 28, "has_track_ridge offset");
_Static_assert(offsetof(ms110d_rx_options, track_ridge) == 32, "track_ridge offset");
_Static_assert(offsetof(ms110d_rx_options, max_interleaver) == 36, "max_interleaver offset");
_Static_assert(offsetof(ms110d_rx_options, soft_decisions) == 40, "soft_decisions offset");
_Static_assert(offsetof(ms110d_rx_options, reserved) == 44, "reserved offset");
_Static_assert(offsetof(ms110d_rx_options, sync_threshold) == 48, "sync_threshold offset");
_Static_assert(offsetof(ms110d_rx_options, roll_off) == 56, "roll_off offset");

/* Receive event: 64 bytes, the 64 bit fields at 8, 40 and 56 on every ABI. */
_Static_assert(sizeof(ms110d_rx_event) == 64, "ms110d_rx_event size");
_Static_assert(offsetof(ms110d_rx_event, struct_size) == 0, "struct_size is first");
_Static_assert(offsetof(ms110d_rx_event, kind) == 4, "kind offset");
_Static_assert(offsetof(ms110d_rx_event, sample) == 8, "sample offset");
_Static_assert(offsetof(ms110d_rx_event, waveform_number) == 16, "waveform_number offset");
_Static_assert(offsetof(ms110d_rx_event, interleaver) == 20, "interleaver offset");
_Static_assert(offsetof(ms110d_rx_event, constraint_length) == 24, "constraint_length offset");
_Static_assert(offsetof(ms110d_rx_event, block_index) == 28, "block_index offset");
_Static_assert(offsetof(ms110d_rx_event, end_reason) == 32, "end_reason offset");
_Static_assert(offsetof(ms110d_rx_event, blocks) == 36, "blocks offset");
_Static_assert(offsetof(ms110d_rx_event, payload_bits) == 40, "payload_bits offset");
_Static_assert(offsetof(ms110d_rx_event, length) == 48, "length offset");
_Static_assert(offsetof(ms110d_rx_event, reserved) == 52, "reserved offset");
_Static_assert(offsetof(ms110d_rx_event, cfo_hz) == 56, "cfo_hz offset");
_Static_assert(sizeof(int64_t) == 8, "int64_t");
_Static_assert(MS110D_RX_EVENT_NONE == 0 && MS110D_RX_EVENT_BURST_STARTED == 1 && MS110D_RX_EVENT_BLOCK_DECODED == 2 &&
                   MS110D_RX_EVENT_BURST_COMPLETED == 3 && MS110D_RX_EVENT_FIRST_PASS_SOFT == 4 &&
                   MS110D_RX_EVENT_TURBO_SOFT == 5,
               "event kinds");
_Static_assert(MS110D_END_EOM == 0 && MS110D_END_MAX_INPUT_DATA_BLOCKS == 1 && MS110D_END_TERMINATED == 2 &&
                   MS110D_END_SIGNAL_LOST == 3 && MS110D_END_SIGNAL_ABSENT == 4,
               "end reasons");
_Static_assert(MS110D_RX_STATE_SEARCHING == 0 && MS110D_RX_STATE_READING_PREAMBLE == 1 && MS110D_RX_STATE_TRACKING == 2,
               "states");
_Static_assert(MS110D_NOISE_PRICING_SPIKE_UP == 0 && MS110D_NOISE_PRICING_OFF == 1 && MS110D_NOISE_PRICING_SPIKE_2S == 2,
               "noise pricing");
