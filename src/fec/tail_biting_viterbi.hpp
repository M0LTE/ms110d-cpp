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
// All memory is sized up front from the longest block: either allocated by the
// constructor, or carved from caller storage by configure() (storage_bytes() says how much),
// which allocates nothing. decode() never allocates and never throws.
class TailBitingViterbiDecoder {
public:
    // `max_block_bits` is the longest block (info bits) decode() will be given.
    TailBitingViterbiDecoder(const ConvolutionalCode& code, std::size_t max_block_bits);
    // Unconfigured (valid() false) until configure().
    TailBitingViterbiDecoder() noexcept = default;
    TailBitingViterbiDecoder(const TailBitingViterbiDecoder&) = delete;
    TailBitingViterbiDecoder& operator=(const TailBitingViterbiDecoder&) = delete;

    // Bytes configure() needs for this code and longest block (0 for a code it cannot decode).
    // Larger codes need more, so storage for k = 9 also fits k = 7.
    static std::size_t storage_bytes(const ConvolutionalCode& code, std::size_t max_block_bits) noexcept;
    // Sets the decoder up in `storage` (any alignment, kept in place), allocating nothing. False,
    // unconfigured, if the code is not valid or the storage too small. May be called again, for
    // another code, on the same storage.
    bool configure(const ConvolutionalCode& code, std::size_t max_block_bits, std::span<std::uint8_t> storage) noexcept;

    // False if the code is not one with 2 <= k <= 16 (decode() then always fails).
    bool valid() const noexcept { return valid_; }

    // Decodes one block. `mother_llrs` is the full rate 1/2 lattice, two LLRs per
    // decoded bit (T1 then T2, 0 where punctured); `info` receives mother_llrs.size() / 2
    // hard bits, one per byte. Returns false, writing nothing, if the sizes disagree,
    // the block is shorter than k bits or longer than the constructor's maximum.
    bool decode(std::span<const float> mother_llrs, std::span<std::uint8_t> info) noexcept;

private:
    ConvolutionalCode code_{};
    bool valid_ = false;
    std::size_t max_n_ = 0;
    std::uint8_t* outputs_ = nullptr;  // per k-bit register value v: (T1 << 1) | T2
    float* metric_a_ = nullptr;
    float* metric_b_ = nullptr;
    std::uint64_t* decisions_ = nullptr;
    std::vector<std::uint64_t> owned_;  // the allocating constructor's storage
};

}  // namespace m0lte::fec
