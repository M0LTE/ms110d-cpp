// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later

#ifndef M0LTE_MS110D_TESTHOOKS_H
#define M0LTE_MS110D_TESTHOOKS_H

/* TEST ONLY. Plain C entry points over the library's internal stages, so the .NET tests can
 * run the same input through the C# reference and the C++ stage in one process (layer 1 of
 * docs/plan.md; see docs/dev/layer1.md). They live in their own shared library,
 * ms110d_testhooks, which is built only when the tests are, is never installed and is not
 * part of the public header or the API lock. They are not an API: they change with the
 * internals.
 *
 * They follow docs/dev/c-interface.md: int32_t status (the MS110D_* codes of ms110d.h),
 * fixed-width types only, caller-owned buffers with explicit lengths, nothing allocated.
 * Bits and symbols are one per byte. Output lengths are written to the *_len parameter. */

#include <m0lte/ms110d/ms110d.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#define MS110D_TH_API __declspec(dllexport)
#elif defined(__GNUC__)
#define MS110D_TH_API __attribute__((visibility("default")))
#else
#define MS110D_TH_API
#endif

/* Stage: framing (D.5.4.3): the payload bits, the 32 bit EOM when append_eom is non-zero, then
 * zeros to a whole number of blocks of input_bits (at least one). Writes
 * framed_block_count * input_bits bits; *out_len is set even on MS110D_ERR_BUFFER_TOO_SMALL. */
MS110D_TH_API int32_t ms110d_th_frame(const uint8_t* payload, uint32_t payload_len, int32_t append_eom,
                                      uint32_t input_bits, uint8_t* out, uint32_t out_capacity, uint32_t* out_len);

/* Stage: the preamble's 8PSK chips (D.5.2.1) for tlc_blocks (0 to 255) and superframes (1 to
 * 32). interleaver_kind is 0 ultra-short, 1 short, 2 medium, 3 long. */
MS110D_TH_API int32_t ms110d_th_preamble(int32_t tlc_blocks, int32_t superframes, int32_t wn,
                                         int32_t interleaver_kind, int32_t constraint_length, uint8_t* chips,
                                         uint32_t capacity, uint32_t* chips_len);

/* Stage: tail-biting convolutional encode. constraint_length is 7 or 9. Writes
 * 2 * info_len bits (T1 then T2 per input bit). */
MS110D_TH_API int32_t ms110d_th_encode(int32_t constraint_length, const uint8_t* info, uint32_t info_len,
                                       uint8_t* coded, uint32_t coded_capacity, uint32_t* coded_len);

/* Stage: Table D-L puncture or repetition for rate rate_num/rate_den (for example 3/4) of
 * the K=7 or K=9 code. MS110D_ERR_UNSUPPORTED if the table has no such rate. */
MS110D_TH_API int32_t ms110d_th_puncture(int32_t constraint_length, int32_t rate_num, int32_t rate_den,
                                         const uint8_t* coded, uint32_t coded_len, uint8_t* out,
                                         uint32_t out_capacity, uint32_t* out_len);

/* Stage: block interleaver of size_bits with the given load increment. in_len must equal
 * size_bits. Writes size_bits bits. */
MS110D_TH_API int32_t ms110d_th_interleave(uint32_t size_bits, uint32_t increment, const uint8_t* in,
                                           uint32_t in_len, uint8_t* out, uint32_t out_capacity);

/* Stage: transcoding of interleaved bits into data symbols for waveform number wn (1 to 8
 * and 13). The bits are taken bits_per_symbol at a time, first bit most significant;
 * in_len must be a multiple of bits_per_symbol. Writes in_len / bits_per_symbol symbols
 * (PSK 0..7, 16QAM 0..15). WN 0 has no transcode here (MS110D_ERR_UNSUPPORTED). */
MS110D_TH_API int32_t ms110d_th_transcode(int32_t wn, const uint8_t* bits, uint32_t in_len, uint8_t* symbols,
                                          uint32_t symbols_capacity, uint32_t* symbols_len);

/* Stage: Appendix D data scrambler (D.5.1.3) applied to transcoded symbols of waveform
 * number wn (1 to 8 and 13): modulo-8 addition for the PSK modes, XOR for 16QAM. The
 * register is reset to its initial value at the start and again every frame_symbols
 * symbols (a data frame). Writes symbols_len symbols. */
MS110D_TH_API int32_t ms110d_th_scramble(int32_t wn, uint32_t frame_symbols, const uint8_t* symbols,
                                         uint32_t symbols_len, uint8_t* out, uint32_t out_capacity);

/* Stage: waveform 0 Trinomial (159, 31) scrambler (D.5.1.4). Writes count scramble
 * symbols (0..7), resetting the register at the start and every block_symbols symbols (an
 * interleaver boundary). */
MS110D_TH_API int32_t ms110d_th_trinomial(uint32_t block_symbols, uint32_t count, uint8_t* out,
                                          uint32_t out_capacity);

#ifdef __cplusplus
}
#endif

#endif
