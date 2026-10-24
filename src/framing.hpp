// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

// Appendix D framing, the bit-level part of the transmit side: payload, EOM and zero fill
// (D.5.4.3), and the two small coded fields of the preamble, the downcount (D.5.2.1.3.1) and
// the waveform ID (D.5.2.1.3.2) with their parity checks. Ported from the C# Ms110dFraming
// and PreambleGenerator. Integer only; caller-owned buffers; no allocation, no exceptions.

#include <m0lte/ms110d/mode.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace m0lte::ms110d {

// Interleaver input blocks needed for a payload: the payload plus the 32 bit EOM (when
// appended), rounded up to whole blocks, and at least one. A payload whose EOM ends exactly on
// a block end gets no extra block. A block size of zero gives 0 blocks.
std::size_t framed_block_count(std::size_t payload_bits, bool append_eom, std::size_t input_bits) noexcept;

// Writes the framed transmit bits (one bit per byte, 0 or 1): the payload, the EOM word
// leftmost bit first when appended, then zeros up to a whole number of blocks. `out` must be
// exactly framed_block_count(...) * input_bits long; returns false, writing nothing, if
// input_bits is zero or `out` has another size. Payload bytes are taken as their low bit.
bool build_tx_bits(std::span<const std::uint8_t> payload_bits, bool append_eom, std::size_t input_bits,
                   std::span<std::uint8_t> out) noexcept;

// The five waveform ID di-bits w4..w0 (w4 first). d9d8d7d6 is the waveform number, d5d4 the
// interleaver option (Table D-XVI), d3 is 0 for K = 7 and 1 for K = 9, and d2d1d0 a parity
// check over d9..d3 (D.5.2.1.3.2). Like the C#, any waveform number 0 to 13 encodes (a
// receiver decides what it implements); nothing is returned for another number or for a
// constraint length other than 7 or 9.
std::optional<std::array<std::uint8_t, 5>> encode_wid(int wn, InterleaverKind interleaver,
                                                      int constraint_length) noexcept;

struct WaveformId {
    int wn;
    InterleaverKind interleaver;
    int constraint_length;
    bool operator==(const WaveformId&) const = default;
};

// Reads the five di-bits back. Nothing is returned if a di-bit is above 3, the parity check
// fails or the waveform number is reserved (14 and 15).
std::optional<WaveformId> decode_wid(const std::array<std::uint8_t, 5>& dibits) noexcept;

// The four downcount di-bits c3..c0 (c3 first): the 5 bit count b4..b0 plus parity bits
// b7 = b1^b2^b3, b6 = b2^b3^b4, b5 = b0^b1^b2 (D.5.2.1.3.1). Nothing is returned for a count
// outside 0 to 31.
std::optional<std::array<std::uint8_t, 4>> encode_count(int count) noexcept;

// Reads the four di-bits back; nothing is returned if a di-bit is above 3 or parity fails.
std::optional<int> decode_count(const std::array<std::uint8_t, 4>& dibits) noexcept;

}  // namespace m0lte::ms110d
