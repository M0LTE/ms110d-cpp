// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "tail_biting_viterbi.hpp"

#include <algorithm>
#include <bit>
#include <utility>

namespace m0lte::fec {

TailBitingViterbiDecoder::TailBitingViterbiDecoder(const ConvolutionalCode& code, std::size_t max_block_bits)
    : code_(code), valid_(code.k >= 2 && code.k <= 16), max_n_(max_block_bits) {
    if (!valid_) {
        return;
    }
    const std::size_t k = static_cast<std::size_t>(code.k);
    const std::size_t states = code.states();
    outputs_.resize(std::size_t{1} << k);
    for (std::uint32_t v = 0; v < outputs_.size(); ++v) {
        const unsigned t1 = static_cast<unsigned>(std::popcount(v & code.poly_t1)) & 1u;
        const unsigned t2 = static_cast<unsigned>(std::popcount(v & code.poly_t2)) & 1u;
        outputs_[v] = static_cast<std::uint8_t>((t1 << 1) | t2);
    }
    metric_a_.resize(states);
    metric_b_.resize(states);
    const std::size_t w = std::min<std::size_t>(6 * k, max_n_);
    decisions_.resize((max_n_ + 2 * w) * ((states + 63) >> 6));
}

bool TailBitingViterbiDecoder::decode(std::span<const float> mother_llrs, std::span<std::uint8_t> info) noexcept {
    const std::size_t n = info.size();
    const std::size_t k = static_cast<std::size_t>(code_.k);
    if (!valid_ || mother_llrs.size() != 2 * n || n < k || n > max_n_) {
        return false;
    }

    const std::size_t states = code_.states();
    const std::size_t w = std::min<std::size_t>(6 * k, n);
    const std::size_t steps = n + 2 * w;
    const std::size_t words = (states + 63) >> 6;
    const std::size_t half = states >> 1;

    std::fill_n(metric_a_.begin(), static_cast<std::ptrdiff_t>(states), 0.0f);
    float* cur = metric_a_.data();
    float* next = metric_b_.data();
    float bm[4];

    for (std::size_t e = 0; e < steps; ++e) {
        // Extended step e covers block position (e - w) mod n.
        std::size_t t = e;
        if (t < w) {
            t = t + n - w;
        } else {
            t -= w;
            if (t >= n) {
                t -= n;
            }
        }

        const float l1 = mother_llrs[2 * t];
        const float l2 = mother_llrs[2 * t + 1];
        bm[0] = l1 + l2;
        bm[1] = l1 - l2;
        bm[2] = -l1 + l2;
        bm[3] = -l1 - l2;

        std::uint64_t* dec = decisions_.data() + e * words;
        for (std::size_t wd = 0; wd < words; ++wd) {
            dec[wd] = 0;
        }

        for (std::size_t s = 0; s < states; ++s) {
            // Register value v = (prevState << 1) | inputBit; new state s = v & (states - 1).
            // The two candidates differ in v's dropped MSB: v0 = s, v1 = s | states.
            const std::size_t v1 = s | states;
            const float m0 = cur[s >> 1] + bm[outputs_[s]];
            const float m1 = cur[(s >> 1) + half] + bm[outputs_[v1]];
            if (m1 > m0) {
                next[s] = m1;
                dec[s >> 6] |= std::uint64_t{1} << (s & 63);
            } else {
                next[s] = m0;
            }
        }

        std::swap(cur, next);

        if ((e & 1023) == 1023) {
            float max = cur[0];
            for (std::size_t s = 1; s < states; ++s) {
                if (cur[s] > max) {
                    max = cur[s];
                }
            }
            for (std::size_t s = 0; s < states; ++s) {
                cur[s] -= max;
            }
        }
    }

    std::size_t best = 0;
    for (std::size_t s = 1; s < states; ++s) {
        if (cur[s] > cur[best]) {
            best = s;
        }
    }

    std::size_t state = best;
    for (std::size_t e = steps; e-- > 0;) {
        if (e >= w && e < w + n) {
            // Input bit = LSB of the post-step state. The encoder's tail-biting wrap makes output
            // pair o carry info[(o + k - 1) mod n] as its newest bit, so map the step back.
            info[(e - w + k - 1) % n] = static_cast<std::uint8_t>(state & 1u);
        }
        const std::uint64_t d = (decisions_[e * words + (state >> 6)] >> (state & 63)) & 1u;
        const std::size_t v = state | (static_cast<std::size_t>(d) << (k - 1));
        state = v >> 1;
    }
    return true;
}

}  // namespace m0lte::fec
