// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later
#include <m0lte/fec/convolutional.hpp>

#include <bit>

namespace m0lte::fec {

bool encode_tail_biting(const ConvolutionalCode& code,
                        std::span<const std::uint8_t> info,
                        std::span<std::uint8_t> coded) noexcept {
    const std::size_t n = info.size();
    if (code.k < 2 || code.k > 16 || n < static_cast<std::size_t>(code.k) || coded.size() != 2 * n) {
        return false;
    }

    const std::uint32_t mask = (std::uint32_t{1} << code.k) - 1u;
    const std::size_t k1 = static_cast<std::size_t>(code.k) - 1;

    std::uint32_t state = 0;
    for (std::size_t i = 0; i < k1; ++i) {
        state = ((state << 1) | (info[i] & 1u)) & mask;  // preload, no output
    }

    std::size_t o = 0;
    std::size_t idx = k1;  // index of the bit entering next, wrapping over the first k-1 bits
    for (std::size_t step = 0; step < n; ++step) {
        state = ((state << 1) | (info[idx] & 1u)) & mask;
        coded[o++] = static_cast<std::uint8_t>(std::popcount(state & code.poly_t1) & 1);  // T1 (b0) first
        coded[o++] = static_cast<std::uint8_t>(std::popcount(state & code.poly_t2) & 1);  // T2 (b1)
        if (++idx == n) {
            idx = 0;
        }
    }
    return true;
}

}  // namespace m0lte::fec
