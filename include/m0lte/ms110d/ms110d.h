// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later

#ifndef M0LTE_MS110D_MS110D_H
#define M0LTE_MS110D_MS110D_H

// C interface to the MS110D modem, for callers that are not C++ (.NET, Python,
// C firmware). The conventions every entry point follows are in
// docs/dev/c-interface.md. In short:
//   - every function is named ms110d_*, returns a status, and never throws;
//   - only fixed-width integers, pointers and plain structs cross the boundary;
//   - objects are opaque handles made and freed by the library, with all memory
//     sized when the handle is created and none allocated afterwards;
//   - the caller owns every input and output buffer and passes its length;
//   - ms110d_abi_version() changes whenever anything here changes shape.
// Public headers use // comments only (see docs/dev/api-lock.md).

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Symbol export. The shared library is built with hidden visibility, so only
// functions marked MS110D_API are visible to a loader. Define
// MS110D_C_STATIC when linking a static build of the C interface on Windows.
#if defined(_WIN32) && !defined(MS110D_C_STATIC)
#if defined(MS110D_C_BUILDING)
#define MS110D_API __declspec(dllexport)
#else
#define MS110D_API __declspec(dllimport)
#endif
#elif defined(__GNUC__)
#define MS110D_API __attribute__((visibility("default")))
#else
#define MS110D_API
#endif

// The shape of the C interface this header describes. Bumped on any change that
// could break a caller that was built against an older header.
#define MS110D_ABI_VERSION 1u

// Only fixed-width integers cross the boundary: no enum, bool or size_t types
// in signatures or structs, because their size is not the same on every ABI
// (arm-none-eabi makes enums one byte). The names below are plain constants.
//
// A status is an int32_t: zero is success, anything else is an error.
typedef int32_t ms110d_status;
enum {
    MS110D_OK = 0,
    MS110D_ERR_INVALID_ARGUMENT = 1,
    MS110D_ERR_BUFFER_TOO_SMALL = 2,
    MS110D_ERR_UNSUPPORTED = 3,
    MS110D_ERR_STATE = 4,
    MS110D_ERR_INTERNAL = 5,
    // The library could not allocate (create only; nothing allocates afterwards).
    MS110D_ERR_OUT_OF_MEMORY = 6,
    // Caller-supplied memory for an init function is too small or misaligned.
    MS110D_ERR_BAD_MEMORY = 7
};

// The floating point type the library was built with, as the width in bits.
enum {
    MS110D_PRECISION_SINGLE = 32,
    MS110D_PRECISION_DOUBLE = 64
};

// The ABI version the loaded library implements. Compare it with
// MS110D_ABI_VERSION before calling anything else. Always safe to call.
MS110D_API uint32_t ms110d_abi_version(void);

// The library's own version (the CMake project version). Each output pointer
// must be non-null, else MS110D_ERR_INVALID_ARGUMENT and nothing is written.
MS110D_API ms110d_status ms110d_version(int32_t* major, int32_t* minor, int32_t* patch);

// The precision the library was built with, so a caller can check it matches
// the one it expects. Fails with MS110D_ERR_INVALID_ARGUMENT on a null pointer.
MS110D_API ms110d_status ms110d_precision_bits(int32_t* precision);

// A fixed, static, human-readable name for a status. Never null, never freed by
// the caller. A value from a newer library, or a bad one, gives "unknown status".
MS110D_API const char* ms110d_status_string(ms110d_status status);

// ---------------------------------------------------------------------------
// Transmit: payload bits in, 9600 Hz audio out, one burst at a time.
//
//   ms110d_tx_settings settings;
//   ms110d_tx_settings_default(&settings);          // the C# Ms110dTxSettings defaults
//   settings.waveform_number = 6;                    // change what you need
//   ms110d_tx* tx;
//   ms110d_tx_create(&settings, max_payload_bits, &tx);
//   ms110d_tx_push_bits(tx, bits, count);            // one bit (0 or 1) per byte
//   ms110d_tx_end_payload(tx);
//   ms110d_tx_audio_length(tx, count, &samples);     // the total length, known up front
//   do { ms110d_tx_read(tx, buf, capacity, &got); /* play buf[0..got) */ } while (got != 0);
//   ms110d_tx_begin(tx);                             // the next burst
//   ms110d_tx_destroy(tx);
//
// The handle is sized when it is made, from the settings and the largest payload, and
// allocates nothing afterwards. Audio is 32 bit float at 9600 Hz, whatever the precision the
// library was built with. A handle is used from one thread at a time.
// ---------------------------------------------------------------------------

typedef struct ms110d_tx ms110d_tx;

// Burst settings. Fill it with ms110d_tx_settings_default() first, then change fields; the
// caller sets struct_size (the default function does) and reserved must be 0. A struct larger
// than the library knows is accepted only if every byte past the known size is zero. Flags are
// 0 or 1.
//   waveform_number       0 to 8 or 13 (default 6)
//   interleaver           0 ultra short, 1 short, 2 medium, 3 long (default 1)
//   constraint_length     7 or 9 (default 7)
//   preamble_superframes  1 to 32 (default 3)
//   tlc_blocks            0 to 255 (default 0)
//   append_eom, append_eot  1 to send the end of message word / the end of transmission marker
//   amplitude             linear scale of the symbols, finite (default 0.5)
//   roll_off              pulse shaping roll-off, 0.05 to 1 (default 0.35)
enum {
    MS110D_INTERLEAVER_ULTRA_SHORT = 0,
    MS110D_INTERLEAVER_SHORT = 1,
    MS110D_INTERLEAVER_MEDIUM = 2,
    MS110D_INTERLEAVER_LONG = 3
};

typedef struct ms110d_tx_settings {
    uint32_t struct_size;
    int32_t waveform_number;
    int32_t interleaver;
    int32_t constraint_length;
    int32_t preamble_superframes;
    int32_t tlc_blocks;
    int32_t append_eom;
    int32_t append_eot;
    float amplitude;
    uint32_t reserved;
    double roll_off;
} ms110d_tx_settings;

// Largest maximum payload, in bits.
#define MS110D_TX_MAX_PAYLOAD_BITS 16777216u

// Sample rate of the audio, in Hz.
#define MS110D_TX_SAMPLE_RATE 9600u

// Writes the defaults, and struct_size, into *settings.
MS110D_API ms110d_status ms110d_tx_settings_default(ms110d_tx_settings* settings);

// Makes a transmitter for these settings and payloads of up to max_payload_bits bits
// (at most MS110D_TX_MAX_PAYLOAD_BITS). MS110D_ERR_INVALID_ARGUMENT for a setting out of range,
// MS110D_ERR_UNSUPPORTED for a waveform and interleaver the standard has no row for,
// MS110D_ERR_OUT_OF_MEMORY if it cannot allocate. *tx is set only on success.
MS110D_API ms110d_status ms110d_tx_create(const ms110d_tx_settings* settings, uint32_t max_payload_bits, ms110d_tx** tx);

// Frees a handle from ms110d_tx_create. Accepts null. Not for one from ms110d_tx_init.
MS110D_API void ms110d_tx_destroy(ms110d_tx* tx);

// The same without a heap: how much memory ms110d_tx_init needs, and its alignment.
MS110D_API ms110d_status ms110d_tx_memory_size(const ms110d_tx_settings* settings, uint32_t max_payload_bits,
                                               uint32_t* bytes, uint32_t* alignment);

// Builds a transmitter in memory the caller owns, which must stay valid and untouched while the
// handle is used. MS110D_ERR_BAD_MEMORY if it is smaller than ms110d_tx_memory_size says or not
// aligned to that. No destroy is needed.
MS110D_API ms110d_status ms110d_tx_init(void* memory, uint32_t bytes, const ms110d_tx_settings* settings,
                                        uint32_t max_payload_bits, ms110d_tx** tx);

// The number of audio samples a payload of payload_bits bits comes to (the burst and the pulse
// tail). It does not depend on the bits.
MS110D_API ms110d_status ms110d_tx_audio_length(const ms110d_tx* tx, uint32_t payload_bits, uint32_t* samples);

// Starts a new burst, forgetting any payload and unread audio.
MS110D_API ms110d_status ms110d_tx_begin(ms110d_tx* tx);

// Appends payload bits, one per byte (the low bit is used). MS110D_ERR_BUFFER_TOO_SMALL, taking
// none of them, if the payload would pass the maximum; MS110D_ERR_STATE after end_payload.
MS110D_API ms110d_status ms110d_tx_push_bits(ms110d_tx* tx, const uint8_t* bits, uint32_t count);

// The payload is complete; the audio can now be read. MS110D_ERR_STATE if already called.
MS110D_API ms110d_status ms110d_tx_end_payload(ms110d_tx* tx);

// Writes up to capacity samples into audio and sets *written to how many; 0 means the burst is
// finished, and only that. A capacity of 0 or a null audio is MS110D_ERR_INVALID_ARGUMENT.
// MS110D_ERR_STATE before end_payload. Any other capacity works, down to 1.
//
// The whole burst in one call is ms110d_tx_modulate, below.
MS110D_API ms110d_status ms110d_tx_read(ms110d_tx* tx, float* audio, uint32_t capacity, uint32_t* written);

// The whole burst at once, like the C# Modulate(bits): begin, push_bits, end_payload and every
// read, into audio. *written is the burst's length in samples. If capacity is shorter the result
// is MS110D_ERR_BUFFER_TOO_SMALL, nothing is written to audio and *written is the length needed
// (so a caller can call with capacity 0 to ask). More bits than the maximum payload is
// MS110D_ERR_BUFFER_TOO_SMALL with *written 0.
MS110D_API ms110d_status ms110d_tx_modulate(ms110d_tx* tx, const uint8_t* bits, uint32_t bit_count, float* audio,
                                            uint32_t capacity, uint32_t* written);

// Samples still to be read (0 before end_payload, and when the burst is finished).
MS110D_API ms110d_status ms110d_tx_remaining(const ms110d_tx* tx, uint32_t* samples);

#ifdef __cplusplus
}
#endif

#endif
