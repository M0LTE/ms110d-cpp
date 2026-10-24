// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

// Appendix D data scrambler (D.5.1.3): generator x^9 + x^4 + 1, 9-bit register initialised
// to 1 at the start of each data frame. PSK symbols are scrambled by modulo-8 addition of the
// rightmost three register bits, 2^N-QAM symbols by XOR with the rightmost N bits. The
// register iterates after use, so the first symbol of a frame is scrambled by the
// initialisation value 000000001.
//
// Register convention, as in the C# reference: bit i of the integer is b[i], one iteration is
// b0' = b8 xor b4 with everything else shifting up, and the rightmost bits are b[2..0]
// (b2 the most significant). Integer only, no state beyond one word, no allocation.

#include <stdint.h>  // not <cstdint>: the Cortex-M7 CI image has no C++ standard library

namespace m0lte::ms110d {

class Scrambler {
public:
    constexpr Scrambler() noexcept = default;

    // Back to the D.5.1.3 initialisation value; call at the start of every data frame.
    constexpr void reset() noexcept { sr_ = 1; }

    // Scrambles a transcoded PSK symbol number (0..7): (symbol + rightmost 3 bits) mod 8,
    // then iterates 3 times. BPSK and QPSK are transcoded onto the 8PSK ring first.
    constexpr int next_psk(int transcoded) noexcept {
        const int scrambled = (transcoded + static_cast<int>(sr_ & 7u)) & 7;
        iterate(3);
        return scrambled;
    }

    // Scrambles a 2^bits-QAM symbol number (bits 1..8) by XOR with the rightmost `bits`
    // register bits, then iterates `bits` times (16QAM: 4, 32QAM: 5, 64QAM: 6, 256QAM: 8).
    constexpr int next_qam(int symbol, int bits) noexcept {
        const int scrambled = symbol ^ static_cast<int>(sr_ & ((1u << bits) - 1u));
        iterate(bits);
        return scrambled;
    }

    // The 9-bit register, for tests.
    constexpr uint32_t state() const noexcept { return sr_; }

private:
    constexpr void iterate(int times) noexcept {
        for (int i = 0; i < times; ++i) {
            const uint32_t feedback = ((sr_ >> 8) ^ (sr_ >> 4)) & 1u;
            sr_ = ((sr_ << 1) | feedback) & 0x1FFu;
        }
    }

    uint32_t sr_ = 1;
};

}  // namespace m0lte::ms110d
