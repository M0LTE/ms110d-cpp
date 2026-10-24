#pragma once

// Waveform ID 0 Walsh data scrambler (D.5.1.4): a Trinomial (159, 31) bit shift register with
// the printed 159-bit initialisation, iterated 16 times per generated 8PSK scramble symbol.
// The output is (b2 << 2) | (b1 << 1) | b0 of the register. The sequence wraps at the
// 2048-symbol boundary, and is reset to the initialisation value at each interleaver boundary.
//
// The register is kept as a ring of 159 bits with a moving head, so one iteration is O(1)
// and the result is identical to the printed tri(), which shifts the whole array.

#include <cstdint>

namespace m0lte::ms110d {

class Trinomial159Scrambler {
public:
    static constexpr int register_length = 159;
    static constexpr int tap = 31;
    static constexpr int shifts_per_symbol = 16;
    static constexpr int wrap_symbols = 2048;

    Trinomial159Scrambler() noexcept { reset(); }

    // Back to the printed initialisation state; call at every interleaver boundary.
    void reset() noexcept;

    // The next 8PSK scramble symbol (0..7), wrapping after 2048 symbols.
    int next() noexcept;

private:
    // ring_[(head_ + i) % 159] is bitshift[i] of the printed code.
    std::uint8_t ring_[register_length];
    unsigned head_ = 0;
    int symbol_count_ = 0;
};

}  // namespace m0lte::ms110d
