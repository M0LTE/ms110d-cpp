// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <m0lte/fec/convolutional.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace m0lte::ms110d {

// One Table D-L puncture or repetition rule of MIL-STD-188-110D Appendix D.
//
// The two mask rows are applied column by column to the pair stream, 1 = keep.
// Rates below 1/2 first repeat each (T1,T2) pair adjacently `repeat` times and then
// mask, so the mask column of the p-th repeated pair is p mod columns.
// Bit c of keep_t1 and keep_t2 is column c (bit 0 is the first column).
struct PunctureSpec {
    std::uint8_t rate_num;   // code rate numerator, as printed in the table
    std::uint8_t rate_den;   // code rate denominator
    std::uint8_t columns;    // mask length, 1 to 9
    std::uint8_t repeat;     // pair repetition factor, 1 for none
    std::uint16_t keep_t1;   // mask row for T1 (b0) bits
    std::uint16_t keep_t2;   // mask row for T2 (b1) bits
};

// Table D-L entry for `code` at `rate_num`/`rate_den` (for example 3/4, or 1/8), or
// nullptr if the table has no such rate or the code is not K=7 or K=9. The K=7 and
// K=9 masks differ for some rates. The returned pointer is to static storage.
const PunctureSpec* find_puncture(const fec::ConvolutionalCode& code,
                                  unsigned rate_num,
                                  unsigned rate_den) noexcept;

// Punctured length for an `info_bits` block, or nullopt if the block is not a whole
// number of mask cycles (info_bits * repeat must be a multiple of columns).
std::optional<std::size_t> punctured_length(const PunctureSpec& spec, std::size_t info_bits) noexcept;

// Repeats and punctures the mother stream `coded` (T1,T2 pairs, one bit per byte)
// into `punctured`, and returns the number of bits written. Returns nullopt, with
// the output partly written, if `punctured` is too small. A trailing odd bit of
// `coded` is ignored.
std::optional<std::size_t> apply_puncture(const PunctureSpec& spec,
                                          std::span<const std::uint8_t> coded,
                                          std::span<std::uint8_t> punctured) noexcept;

}  // namespace m0lte::ms110d
