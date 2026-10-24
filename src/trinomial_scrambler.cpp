// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "trinomial_scrambler.hpp"

namespace m0lte::ms110d {

namespace {

// int bitshift[159] as printed in D.5.1.4.
constexpr uint8_t k_init[Trinomial159Scrambler::register_length] = {
    0, 0, 0, 1, 0, 0, 1, 1, 0, 1, 1, 0, 0, 1, 0, 1,
    1, 1, 1, 1, 0, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0,
    1, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 1, 0,
    1, 1, 1, 0, 0, 0, 1, 1, 0, 0, 0, 1, 0, 0, 0, 0,
    1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 1, 0, 1,
    0, 1, 1, 1, 1, 0, 1, 0, 1, 0, 0, 0, 1, 1, 1, 1,
    1, 1, 0, 0, 1, 1, 0, 1, 0, 1, 1, 1, 1, 1, 0, 1,
    1, 1, 1, 0, 0, 0, 1, 1, 0, 0, 0, 1, 1, 0, 1, 0,
    1, 1, 1, 0, 0, 1, 1, 1, 0, 0, 0, 1, 1, 0, 0, 0,
    1, 0, 0, 1, 0, 0, 0, 1, 1, 0, 1, 0, 0, 1, 1,
};

constexpr unsigned wrap(unsigned i) noexcept {
    return i % static_cast<unsigned>(Trinomial159Scrambler::register_length);
}

}  // namespace

void Trinomial159Scrambler::reset() noexcept {
    for (int i = 0; i < register_length; ++i) {
        ring_[i] = k_init[i];
    }
    head_ = 0;
    symbol_count_ = 0;
}

int Trinomial159Scrambler::next() noexcept {
    if (symbol_count_ == wrap_symbols) {
        // "the sequences are continuously wrapped around the 2048 symbol boundary".
        reset();
    }

    for (int j = 0; j < shifts_per_symbol; ++j) {
        // bitout = bitshift[158], bittap = bitshift[31]; shifting up by one makes the old
        // bitshift[158] slot the new bitshift[0] slot, which receives bitout ^ bittap.
        const uint8_t bitout = ring_[wrap(head_ + (register_length - 1))];
        const uint8_t bittap = ring_[wrap(head_ + tap)];
        head_ = wrap(head_ + (register_length - 1));
        ring_[head_] = static_cast<uint8_t>(bitout ^ bittap);
    }

    ++symbol_count_;
    const uint8_t b0 = ring_[head_];
    const uint8_t b1 = ring_[wrap(head_ + 1)];
    const uint8_t b2 = ring_[wrap(head_ + 2)];
    return (b2 << 2) + (b1 << 1) + b0;
}

}  // namespace m0lte::ms110d
