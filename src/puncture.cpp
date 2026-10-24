// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "puncture.hpp"

#include <array>

namespace m0lte::ms110d {

namespace {

// A mask row written as the string printed in the table, leftmost column first,
// folded into a bit mask with the first column in bit 0.
constexpr std::uint16_t row(const char* s) {
    std::uint16_t v = 0;
    for (unsigned i = 0; s[i] != '\0'; ++i) {
        if (s[i] == '1') {
            v = static_cast<std::uint16_t>(v | (1u << i));
        }
    }
    return v;
}

constexpr std::uint8_t width(const char* s) {
    std::uint8_t n = 0;
    while (s[n] != '\0') {
        ++n;
    }
    return n;
}

constexpr PunctureSpec spec(std::uint8_t num, std::uint8_t den, const char* t1, const char* t2, std::uint8_t repeat) {
    return PunctureSpec{num, den, width(t1), repeat, row(t1), row(t2)};
}

// Table D-L, K=7 column. Rates above 1/2 are the masks verbatim; rates below 1/2
// repeat the rate 1/2 pair stream (1/3 is "2/3 repeated 2x") and then mask.
constexpr std::array<PunctureSpec, 17> k7_table{{
    spec(9, 10, "111101110", "100010001", 1),
    spec(8, 9, "11110100", "10001011", 1),
    spec(5, 6, "11010", "10101", 1),
    spec(4, 5, "1111", "1000", 1),
    spec(3, 4, "110", "101", 1),
    spec(2, 3, "11", "10", 1),
    spec(4, 7, "1111", "0111", 1),
    spec(9, 16, "111101111", "111111011", 1),
    spec(1, 2, "1", "1", 1),
    spec(2, 5, "1110", "1010", 2),
    spec(1, 3, "11", "10", 2),
    spec(2, 7, "1111", "0111", 2),
    spec(1, 4, "1", "1", 2),
    spec(1, 6, "1", "1", 3),
    spec(1, 8, "1", "1", 4),
    spec(1, 12, "1", "1", 6),
    spec(1, 16, "1", "1", 8),
}};

// Table D-L, K=9 column; it differs from K=7 for 9/10, 8/9, 5/6, 4/5 and 3/4.
constexpr std::array<PunctureSpec, 17> k9_table{{
    spec(9, 10, "111000101", "100111010", 1),
    spec(8, 9, "11100000", "10011111", 1),
    spec(5, 6, "10110", "11001", 1),
    spec(4, 5, "1101", "1010", 1),
    spec(3, 4, "111", "100", 1),
    spec(2, 3, "11", "10", 1),
    spec(4, 7, "1111", "0111", 1),
    spec(9, 16, "111101111", "111111011", 1),
    spec(1, 2, "1", "1", 1),
    spec(2, 5, "1110", "1010", 2),
    spec(1, 3, "11", "10", 2),
    spec(2, 7, "1111", "0111", 2),
    spec(1, 4, "1", "1", 2),
    spec(1, 6, "1", "1", 3),
    spec(1, 8, "1", "1", 4),
    spec(1, 12, "1", "1", 6),
    spec(1, 16, "1", "1", 8),
}};

}  // namespace

const PunctureSpec* find_puncture(const fec::ConvolutionalCode& code,
                                  unsigned rate_num,
                                  unsigned rate_den) noexcept {
    const std::array<PunctureSpec, 17>* table = nullptr;
    if (code.k == 7) {
        table = &k7_table;
    } else if (code.k == 9) {
        table = &k9_table;
    } else {
        return nullptr;
    }
    for (const PunctureSpec& s : *table) {
        if (s.rate_num == rate_num && s.rate_den == rate_den) {
            return &s;
        }
    }
    return nullptr;
}

std::optional<std::size_t> punctured_length(const PunctureSpec& spec, std::size_t info_bits) noexcept {
    const std::size_t pairs = info_bits * spec.repeat;
    if (spec.columns == 0 || pairs % spec.columns != 0) {
        return std::nullopt;
    }
    std::size_t per_cycle = 0;
    for (unsigned c = 0; c < spec.columns; ++c) {
        per_cycle += ((spec.keep_t1 >> c) & 1u) + ((spec.keep_t2 >> c) & 1u);
    }
    return pairs / spec.columns * per_cycle;
}

std::optional<std::size_t> apply_puncture(const PunctureSpec& spec,
                                          std::span<const std::uint8_t> coded,
                                          std::span<std::uint8_t> punctured) noexcept {
    if (spec.columns == 0) {
        return std::nullopt;
    }
    const std::size_t base_pairs = coded.size() / 2;
    std::size_t o = 0;
    std::size_t col = 0;  // p mod columns, kept incrementally
    for (std::size_t n = 0; n < base_pairs; ++n) {
        for (unsigned r = 0; r < spec.repeat; ++r) {
            if ((spec.keep_t1 >> col) & 1u) {
                if (o >= punctured.size()) {
                    return std::nullopt;
                }
                punctured[o++] = coded[2 * n];
            }
            if ((spec.keep_t2 >> col) & 1u) {
                if (o >= punctured.size()) {
                    return std::nullopt;
                }
                punctured[o++] = coded[2 * n + 1];
            }
            if (++col == spec.columns) {
                col = 0;
            }
        }
    }
    return o;
}

bool depuncture_soft(const PunctureSpec& spec, std::span<const float> rx, std::span<float> mother) noexcept {
    for (float& v : mother) v = 0.0f;
    if (spec.columns == 0 || spec.repeat == 0) return false;
    const std::size_t pairs = mother.size() / 2 * spec.repeat;
    std::size_t j = 0;
    for (std::size_t p = 0; p < pairs; ++p) {
        const std::size_t n = p / spec.repeat;
        const std::size_t col = p % spec.columns;
        if ((spec.keep_t1 >> col) & 1u) {
            if (j >= rx.size()) return false;
            mother[2 * n] += rx[j++];
        }
        if ((spec.keep_t2 >> col) & 1u) {
            if (j >= rx.size()) return false;
            mother[(2 * n) + 1] += rx[j++];
        }
    }
    return j == rx.size();
}

}  // namespace m0lte::ms110d
