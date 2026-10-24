// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "convolutional.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace m0lte::fec {

// Soft-decision Viterbi decoder for the tail-biting rate 1/2 mother codes (the C#
// M0LTE.Fec TailBitingViterbiDecoder).
//
// LLR convention: positive means bit 0. Branch metrics are max-log (plus or minus LLR
// sums), so a punctured position carries LLR 0 and contributes nothing: depuncturing
// needs no special case here.
//
// Tail-biting is handled by circular wrap-extension: the trellis runs over the last W
// steps, the full block and the first W steps with equal initial metrics, traceback
// starts from the best end state, and only the middle N decisions are kept, with
// W = min(6 * k, N). Decision memory is bit-packed per step.
//
// Arithmetic is float, as in the C#, in both precision builds: the module is
// independent of the modem's `real` type. The metrics are sums of a few LLRs, nothing
// here needs more than float, and double would only double the decision memory traffic.
//
// All memory is sized in the constructor from the longest block; decode() never
// allocates and never throws.
class TailBitingViterbiDecoder {
public:
    // `max_block_bits` is the longest block (info bits) decode() will be given.
    TailBitingViterbiDecoder(const ConvolutionalCode& code, std::size_t max_block_bits);

    // False if the code is not one with 2 <= k <= 16 (decode() then always fails).
    bool valid() const noexcept { return valid_; }

    // Decodes one block. `mother_llrs` is the full rate 1/2 lattice, two LLRs per
    // decoded bit (T1 then T2, 0 where punctured); `info` receives mother_llrs.size() / 2
    // hard bits, one per byte. Returns false, writing nothing, if the sizes disagree,
    // the block is shorter than k bits or longer than the constructor's maximum.
    bool decode(std::span<const float> mother_llrs, std::span<std::uint8_t> info) noexcept;

private:
    ConvolutionalCode code_;
    bool valid_;
    std::size_t max_n_;
    std::vector<std::uint8_t> outputs_;  // per k-bit register value v: (T1 << 1) | T2
    std::vector<float> metric_a_;
    std::vector<float> metric_b_;
    std::vector<std::uint64_t> decisions_;
};

}  // namespace m0lte::fec
