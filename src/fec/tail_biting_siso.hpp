// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "convolutional.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace m0lte::fec {

// Soft-in soft-out (log-MAP BCJR) decoder for the tail-biting rate 1/2 mother codes (the
// C# TailBitingSisoDecoder): the outer half of the soft-feedback turbo. Where the Viterbi
// returns hard info bits, this returns a posterior LLR for every coded (mother lattice)
// position. LLR convention as the Viterbi: positive means bit 0; punctured positions carry
// LLR 0 on input, and their outputs are then the code's own opinion of the erased bit.
//
// Trellis convention (matches the encoder exactly): the k-bit branch register v holds the
// oldest bit at the MSB and the newest at the LSB, so a branch runs from state v >> 1 to
// state v & (2^(k-1) - 1) and emits parity(v & poly_t1), parity(v & poly_t2).
//
// Tail-biting is handled as in the Viterbi: circular wrap-extension with
// W = min(6 * k, N) warm-up stages on each end and equal starting metrics, keeping only
// the middle N stages' outputs. max* uses a 128 entry log(1 + e^-d) table (step 1/16,
// clamped at 8: worst case error under 0.04).
//
// Arithmetic is float in both precision builds, as in the C#. The table quantisation
// (0.04) is far coarser than float rounding, so double would buy nothing, and the alpha
// store (N * 2^(k-1) metrics) would double. The module does not use the modem's `real`.
//
// All memory is sized in the constructor from the longest block (alpha is
// max_block_bits * 2^(k-1) floats: about 0.5 MB for k = 9 and 1000 bits); decode() never
// allocates and never throws.
class TailBitingSisoDecoder {
public:
    TailBitingSisoDecoder(const ConvolutionalCode& code, std::size_t max_block_bits);

    // False if the code is not one with 2 <= k <= 16 (decode() then always fails).
    bool valid() const noexcept { return valid_; }

    // Decodes one block. `mother_llrs` is the full rate 1/2 lattice (two LLRs per info
    // bit, 0 where punctured); `mother_posteriors`, the same length, receives the posterior
    // LLR of every coded position given the whole block and the code constraint. Returns
    // false, writing nothing, if the lengths differ or are odd, the block is shorter than
    // k bits or longer than the constructor's maximum.
    bool decode(std::span<const float> mother_llrs, std::span<float> mother_posteriors) noexcept;

private:
    static constexpr std::size_t corr_entries = 128;

    float max_star(float a, float b) const noexcept;

    ConvolutionalCode code_;
    bool valid_;
    std::size_t max_n_;
    std::vector<std::uint8_t> outputs_;
    std::vector<float> alpha_;
    std::vector<float> beta_a_;
    std::vector<float> beta_b_;
    std::array<float, corr_entries> corr_;
};

}  // namespace m0lte::fec
