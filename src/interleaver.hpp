#pragma once

// Appendix D block interleaver (D.5.3.3.1 to .3): one 1-D array of "Interleaver Size in
// Bits". The transmitter loads punctured bit B(n) at (n * increment) mod size and fetches
// linearly 0, 1, 2, ...; the receiver inverts that. Sizes and increments for 3 kHz are in
// Tables D-XXXVII and D-LI (ultra-short, short, medium, long for each waveform number).
//
// The load location is computed on the fly (no permutation table), so an Interleaver is two
// integers (the increment is reduced modulo the size) and needs no buffer: the caller owns the input and output arrays, each exactly
// `size_bits` long. The increment must be coprime with the size for the load map to be a
// permutation (every pair in the standard is; see coprime()). Input and output must not
// overlap. No allocation, no exceptions.

#include <stdint.h>  // not <cstdint>: the Cortex-M7 CI image has no C++ standard library

namespace m0lte::ms110d {

class Interleaver {
public:
    constexpr Interleaver(uint32_t size_bits, uint32_t increment) noexcept
        : size_(size_bits), increment_(size_bits == 0 ? 0 : increment % size_bits) {}

    constexpr uint32_t size_bits() const noexcept { return size_; }
    constexpr uint32_t increment() const noexcept { return increment_; }

    // True when the load map is a permutation of 0..size-1.
    constexpr bool valid() const noexcept { return size_ > 0 && gcd(size_, increment_) == 1; }

    // Load location of punctured bit B(n): (n * increment) mod size.
    constexpr uint32_t load_location(uint32_t n) const noexcept {
        return static_cast<uint32_t>((static_cast<uint64_t>(n) * increment_) % size_);
    }

    // TX: out[load_location(n)] = in[n]; reading `out` in order is the linear fetch.
    template <typename T>
    constexpr void interleave(const T* in, T* out) const noexcept {
        uint32_t loc = 0;
        for (uint32_t n = 0; n < size_; ++n) {
            out[loc] = in[n];
            loc += increment_;
            if (loc >= size_) {
                loc -= size_;
            }
        }
    }

    // RX: out[n] = in[load_location(n)], recovering punctured-order values (bits or LLRs)
    // from wire-order ones.
    template <typename T>
    constexpr void deinterleave(const T* in, T* out) const noexcept {
        uint32_t loc = 0;
        for (uint32_t n = 0; n < size_; ++n) {
            out[n] = in[loc];
            loc += increment_;
            if (loc >= size_) {
                loc -= size_;
            }
        }
    }

private:
    static constexpr uint32_t gcd(uint32_t a, uint32_t b) noexcept {
        while (b != 0) {
            const uint32_t t = a % b;
            a = b;
            b = t;
        }
        return a;
    }

    uint32_t size_;
    uint32_t increment_;
};

}  // namespace m0lte::ms110d
