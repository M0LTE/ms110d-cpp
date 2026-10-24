// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "framing.hpp"

#include "tables.hpp"

namespace m0lte::ms110d {

std::size_t framed_block_count(std::size_t payload_bits, bool append_eom, std::size_t input_bits) noexcept {
    if (input_bits == 0) return 0;
    const std::size_t used = payload_bits + (append_eom ? 32u : 0u);
    const std::size_t blocks = (used + input_bits - 1) / input_bits;
    return blocks < 1 ? 1 : blocks;
}

bool build_tx_bits(std::span<const std::uint8_t> payload_bits, bool append_eom, std::size_t input_bits,
                   std::span<std::uint8_t> out) noexcept {
    if (input_bits == 0) return false;
    if (out.size() != framed_block_count(payload_bits.size(), append_eom, input_bits) * input_bits) return false;

    std::size_t n = 0;
    for (const std::uint8_t b : payload_bits) out[n++] = b & 1u;
    if (append_eom) {
        // D.5.4.3: the left most bit of 4B65A5B2 is sent first.
        for (int i = 0; i < 32; ++i) out[n++] = static_cast<std::uint8_t>((tables::eom >> (31 - i)) & 1u);
    }
    while (n < out.size()) out[n++] = 0;
    return true;
}

bool build_tx_block(std::span<const std::uint8_t> packed, std::size_t payload_bits, bool append_eom,
                    std::size_t input_bits, std::size_t block_index, std::span<std::uint8_t> out) noexcept {
    if (input_bits == 0 || out.size() != input_bits || packed.size() < (payload_bits + 7) / 8) return false;
    if (block_index >= framed_block_count(payload_bits, append_eom, input_bits)) return false;
    const std::size_t payload = payload_bits;
    const std::size_t start = block_index * input_bits;
    for (std::size_t i = 0; i < input_bits; ++i) {
        const std::size_t g = start + i;
        std::uint8_t bit = 0;
        if (g < payload) {
            bit = static_cast<std::uint8_t>((static_cast<unsigned>(packed[g / 8]) >> (g % 8)) & 1u);
        } else if (append_eom && g < payload + 32) {
            // D.5.4.3: the left most bit of 4B65A5B2 is sent first.
            bit = static_cast<std::uint8_t>((tables::eom >> (31 - (g - payload))) & 1u);
        }
        out[i] = bit;
    }
    return true;
}

std::optional<std::array<std::uint8_t, 5>> encode_wid(int wn, InterleaverKind interleaver,
                                                      int constraint_length) noexcept {
    if (wn < 0 || wn > 13) return std::nullopt;
    if (constraint_length != 7 && constraint_length != 9) return std::nullopt;
    const int d9 = (wn >> 3) & 1, d8 = (wn >> 2) & 1, d7 = (wn >> 1) & 1, d6 = wn & 1;
    const int il = static_cast<int>(interleaver);
    const int d5 = (il >> 1) & 1, d4 = il & 1;
    const int d3 = constraint_length == 9 ? 1 : 0;
    const int d2 = d9 ^ d8 ^ d7;
    const int d1 = d7 ^ d6 ^ d5;
    const int d0 = d5 ^ d4 ^ d3;
    return std::array<std::uint8_t, 5>{
        static_cast<std::uint8_t>((d9 << 1) | d8), static_cast<std::uint8_t>((d7 << 1) | d6),
        static_cast<std::uint8_t>((d5 << 1) | d4), static_cast<std::uint8_t>((d3 << 1) | d2),
        static_cast<std::uint8_t>((d1 << 1) | d0)};
}

std::optional<WaveformId> decode_wid(const std::array<std::uint8_t, 5>& dibits) noexcept {
    for (const std::uint8_t d : dibits) {
        if (d > 3) return std::nullopt;
    }
    const int wn = (dibits[0] << 2) | dibits[1];
    const auto interleaver = static_cast<InterleaverKind>(dibits[2]);
    const int constraint_length = (dibits[3] & 2) != 0 ? 9 : 7;
    if (wn >= 14) return std::nullopt;
    const auto expected = encode_wid(wn, interleaver, constraint_length);
    if (!expected || dibits[3] != (*expected)[3] || dibits[4] != (*expected)[4]) return std::nullopt;
    return WaveformId{wn, interleaver, constraint_length};
}

std::optional<std::array<std::uint8_t, 4>> encode_count(int count) noexcept {
    if (count < 0 || count > 31) return std::nullopt;
    const int b0 = count & 1, b1 = (count >> 1) & 1, b2 = (count >> 2) & 1, b3 = (count >> 3) & 1,
              b4 = (count >> 4) & 1;
    const int b7 = b1 ^ b2 ^ b3;
    const int b6 = b2 ^ b3 ^ b4;
    const int b5 = b0 ^ b1 ^ b2;
    return std::array<std::uint8_t, 4>{
        static_cast<std::uint8_t>((b7 << 1) | b6), static_cast<std::uint8_t>((b5 << 1) | b4),
        static_cast<std::uint8_t>((b3 << 1) | b2), static_cast<std::uint8_t>((b1 << 1) | b0)};
}

std::optional<int> decode_count(const std::array<std::uint8_t, 4>& dibits) noexcept {
    for (const std::uint8_t d : dibits) {
        if (d > 3) return std::nullopt;
    }
    const int count = (((dibits[1] & 1) << 4) | (dibits[2] << 2) | dibits[3]) & 31;
    const auto expected = encode_count(count);
    if (!expected || dibits[0] != (*expected)[0] || (dibits[1] >> 1) != ((*expected)[1] >> 1)) return std::nullopt;
    return count;
}

}  // namespace m0lte::ms110d
