// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

// Forward error correction primitives. This module knows nothing of MS110D: it is
// laid out so it can move to its own library when a second modem needs it.
//
// Everything here is integer only, never allocates and never throws.
namespace m0lte::fec {

// A rate 1/2 feed-forward convolutional mother code.
//
// A generator polynomial is held with the oldest register tap (the highest power of
// x, x^(k-1)) as the most significant bit and the newest input bit (x^0) as the
// least significant. Octal 133 is therefore 0b1011011. The register holds the k most
// recent input bits, newest in bit 0, and an output bit is the parity of the
// register ANDed with the generator. For each input bit two bits are produced, the
// one from poly_t1 (the upper output, b0) first.
struct ConvolutionalCode {
    int k;                  // constraint length
    std::uint32_t poly_t1;  // generator of the first output bit per input bit
    std::uint32_t poly_t2;  // generator of the second output bit per input bit

    // Trellis state count, 2^(k-1).
    constexpr std::uint32_t states() const noexcept { return std::uint32_t{1} << (k - 1); }

    friend constexpr bool operator==(const ConvolutionalCode&, const ConvolutionalCode&) = default;
};

// Constraint length 7: T1 = x^6+x^4+x^3+x+1 (octal 133), T2 = x^6+x^5+x^4+x^3+1 (octal 171).
inline constexpr ConvolutionalCode k7_133_171{7, 0b1011011u, 0b1111001u};

// Constraint length 9: T1 = x^8+x^6+x^5+x^4+1 (octal 561), T2 = x^8+x^7+x^6+x^5+x^3+x+1 (octal 753).
inline constexpr ConvolutionalCode k9_561_753{9, 0b101110001u, 0b111101011u};

// Full tail-biting encode of one block. `info` holds one bit per byte (0 or 1; only
// the low bit is used). `coded` receives exactly 2 * info.size() bits, one per byte.
//
// The register is preloaded with the first k-1 input bits with no output; the first
// output pair is produced as bit k-1 shifts in; after the last input bit the first
// k-1 bits are shifted in again, earliest first, and their pairs are the last outputs.
// The encoder therefore ends in the state it started in. Output pair i is the pair
// produced when input bit (i + k - 1) mod n enters the register, T1 then T2.
//
// Returns false, writing nothing, if the block is shorter than k bits, the code is
// not one with 2 <= k <= 16, or `coded` is not exactly twice as long as `info`.
bool encode_tail_biting(const ConvolutionalCode& code,
                        std::span<const std::uint8_t> info,
                        std::span<std::uint8_t> coded) noexcept;

}  // namespace m0lte::fec
