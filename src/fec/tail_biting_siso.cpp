// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "tail_biting_siso.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cmath>
#include <limits>
#include <utility>

namespace m0lte::fec {

namespace {

constexpr float corr_step = 1.0f / 16.0f;
constexpr float float_min = std::numeric_limits<float>::lowest();  // C# float.MinValue

// log P(pair) up to a constant: gamma = 0.5 * (+-L1 +-L2), + for bit 0, indexed by the
// (t1 << 1) | t2 output code.
inline void branch_metrics(const float* llrs, std::size_t p, float* g) noexcept {
    const float l1 = 0.5f * llrs[2 * p];
    const float l2 = 0.5f * llrs[2 * p + 1];
    g[0] = l1 + l2;
    g[1] = l1 - l2;
    g[2] = -l1 + l2;
    g[3] = -l1 - l2;
}

}  // namespace

namespace {

struct SisoLayout {
    std::size_t alpha = 0, beta_a = 0, beta_b = 0, outputs = 0, total = 0;
};

SisoLayout siso_layout(const ConvolutionalCode& code, std::size_t max_n) noexcept {
    SisoLayout l;
    const std::size_t states = code.states();
    std::size_t at = max_n * states * sizeof(float);
    l.beta_a = at;
    at += states * sizeof(float);
    l.beta_b = at;
    at += states * sizeof(float);
    l.outputs = at;
    at += std::size_t{1} << code.k;
    l.total = at;
    return l;
}

}  // namespace

std::size_t TailBitingSisoDecoder::storage_bytes(const ConvolutionalCode& code, std::size_t max_block_bits) noexcept {
    if (code.k < 2 || code.k > 16) return 0;
    return siso_layout(code, max_block_bits).total + alignof(float);
}

bool TailBitingSisoDecoder::configure(const ConvolutionalCode& code, std::size_t max_block_bits,
                                      std::span<std::uint8_t> storage) noexcept {
    valid_ = false;
    const std::size_t need = storage_bytes(code, max_block_bits);
    if (need == 0 || storage.size() < need) return false;
    const auto addr = reinterpret_cast<std::uintptr_t>(storage.data());
    std::uint8_t* base = storage.data() + ((alignof(float) - (addr % alignof(float))) % alignof(float));
    const SisoLayout l = siso_layout(code, max_block_bits);
    code_ = code;
    max_n_ = max_block_bits;
    alpha_ = reinterpret_cast<float*>(base + l.alpha);
    beta_a_ = reinterpret_cast<float*>(base + l.beta_a);
    beta_b_ = reinterpret_cast<float*>(base + l.beta_b);
    outputs_ = base + l.outputs;
    for (std::uint32_t v = 0; v < (std::uint32_t{1} << code.k); ++v) {
        const unsigned t1 = static_cast<unsigned>(std::popcount(v & code.poly_t1)) & 1u;
        const unsigned t2 = static_cast<unsigned>(std::popcount(v & code.poly_t2)) & 1u;
        outputs_[v] = static_cast<std::uint8_t>((t1 << 1) | t2);
    }
    for (std::size_t i = 0; i < corr_entries; ++i) {
        corr_[i] = std::log(1.0f + std::exp(-(static_cast<float>(i) * corr_step)));
    }
    valid_ = true;
    return true;
}

TailBitingSisoDecoder::TailBitingSisoDecoder(const ConvolutionalCode& code, std::size_t max_block_bits)
    : code_(code), max_n_(max_block_bits) {
    const std::size_t bytes = storage_bytes(code, max_block_bits);
    if (bytes == 0) return;
    owned_.resize((bytes + sizeof(float) - 1) / sizeof(float));
    configure(code, max_block_bits, {reinterpret_cast<std::uint8_t*>(owned_.data()), owned_.size() * sizeof(float)});
}

float TailBitingSisoDecoder::max_star(float a, float b) const noexcept {
    const float m = a > b ? a : b;
    const float d = a > b ? a - b : b - a;
    if (std::isinf(m) && m < 0.0f) {
        return m;
    }
    // Not (d < 8) rather than (d >= 8): an infinite or NaN d must not reach the table index.
    if (!(d < static_cast<float>(corr_entries) * corr_step)) {
        return m;
    }
    return m + corr_[static_cast<std::size_t>(d * (1.0f / corr_step))];
}

bool TailBitingSisoDecoder::decode(std::span<const float> mother_llrs, std::span<float> mother_posteriors) noexcept {
    if (!valid_ || mother_posteriors.size() != mother_llrs.size() || (mother_llrs.size() & 1) != 0) {
        return false;
    }
    const std::size_t n = mother_llrs.size() / 2;
    const std::size_t k = static_cast<std::size_t>(code_.k);
    if (n < k || n > max_n_) {
        return false;
    }

    const std::size_t states = code_.states();
    const std::size_t half = states >> 1;
    const std::size_t wrap = std::min<std::size_t>(6 * k, n);
    const float* llrs = mother_llrs.data();
    const std::uint8_t* outputs = outputs_;
    float* alpha_store = alpha_;
    float g[4];

    // Forward: alpha over extended stages [-W, N); alpha before branch p is stored at alpha_[p * states].
    float* cur = beta_a_;
    float* next = beta_b_;
    std::fill_n(cur, states, 0.0f);
    for (std::ptrdiff_t i = -static_cast<std::ptrdiff_t>(wrap); i < static_cast<std::ptrdiff_t>(n); ++i) {
        const std::size_t p = i < 0 ? static_cast<std::size_t>(i + static_cast<std::ptrdiff_t>(n)) : static_cast<std::size_t>(i);
        if (i >= 0) {
            std::copy_n(cur, states, alpha_store + p * states);
        }

        branch_metrics(llrs, p, g);
        float norm = float_min;
        for (std::size_t s = 0; s < states; ++s) {
            const float a = cur[s >> 1] + g[outputs[s]];
            const float b = cur[(s >> 1) + half] + g[outputs[s | states]];
            const float m = max_star(a, b);
            next[s] = m;
            if (m > norm) {
                norm = m;
            }
        }
        for (std::size_t s = 0; s < states; ++s) {
            next[s] -= norm;
        }
        std::swap(cur, next);
    }

    // Backward: beta over extended stages [N + W - 1, 0], emitting posteriors for stages < N.
    // beta_after[s] is the metric at the boundary after the current branch.
    float* beta_after = cur;
    float* beta_before = next;
    std::fill_n(beta_after, states, 0.0f);
    for (std::size_t i = n + wrap; i-- > 0;) {
        const std::size_t p = i >= n ? i - n : i;
        branch_metrics(llrs, p, g);
        if (i < n) {
            const float* alpha = alpha_store + p * states;
            float t10 = float_min, t11 = float_min;
            float t20 = float_min, t21 = float_min;
            for (std::size_t v = 0; v < 2 * states; ++v) {
                const unsigned output = outputs[v];
                const float m = alpha[v >> 1] + g[output] + beta_after[v & (states - 1)];
                if ((output & 2u) == 0) {
                    t10 = max_star(t10, m);
                } else {
                    t11 = max_star(t11, m);
                }
                if ((output & 1u) == 0) {
                    t20 = max_star(t20, m);
                } else {
                    t21 = max_star(t21, m);
                }
            }
            mother_posteriors[2 * p] = t10 - t11;
            mother_posteriors[2 * p + 1] = t20 - t21;
        }

        float norm = float_min;
        for (std::size_t s = 0; s < states; ++s) {
            const std::size_t v0 = s << 1;
            const float a = g[outputs[v0]] + beta_after[v0 & (states - 1)];
            const float b = g[outputs[v0 | 1]] + beta_after[(v0 | 1) & (states - 1)];
            const float m = max_star(a, b);
            beta_before[s] = m;
            if (m > norm) {
                norm = m;
            }
        }
        for (std::size_t s = 0; s < states; ++s) {
            beta_before[s] -= norm;
        }
        std::swap(beta_after, beta_before);
    }
    return true;
}

}  // namespace m0lte::fec
