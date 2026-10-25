// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

// The two BCJR equalisers of the C# package (Ms110dBcjr and Ms110dChainBcjr): soft symbol
// detection on the two path channel y[t] = h1[t] x[t] + h2[t] x[t - d] plus noise, giving
// per-bit LLRs for the turbo loop. Internal, like the rest of the receive side: not
// installed and not part of the locked public API.
//
// Floating point (docs/dev/soft-decoders.md): the C# is float throughout. Here the metrics,
// channel gains, samples and LLRs are `real`, so the equaliser runs at the precision of the
// samples it is fed: float in the single build (the same arithmetic as the C#), double in the
// default build, where the smooth log-sum-exp chain BCJR then differs from the C# only by
// the float rounding the C# carries. The constellation is the float table (tables::Complex32)
// as the C# holds it, widened on use. The sentinel for an impossible path is the C# -1e30f.
//
// All memory is sized in the constructor; equalize() never allocates and never throws. The
// complex products are written out on the real and imaginary parts, as the C# Complex32
// does, so no library NaN fix up runs and the sums are in the C# order.

#include <m0lte/ms110d/config.hpp>

#include "tables.hpp"

#include <complex>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace m0lte::ms110d {

#ifdef MS110D_SINGLE_PRECISION
inline namespace v_single {
#else
inline namespace v_double {
#endif

// Max-log BCJR for BPSK on a two path channel: forward-backward recursions on a 2^L state
// trellis (L = echo delay in symbols) with max in place of log-sum-exp throughout. This is
// the max-log approximation, not exact MAP: LLRs are best path metric differences, well
// calibrated when the noise variance is honest and slightly over-confident where competing
// paths run close. Positive LLR means bit 0, symbol +1.
class Bcjr {
public:
    // `max_symbols` is the longest block and `max_delay` the largest echo delay (1 to 24,
    // the trellis has 2^delay states) that equalize() will be given. Scratch is
    // 2 * (max_symbols + 1) * 2^max_delay reals.
    Bcjr(std::size_t max_symbols, int max_delay);

    // False if max_delay was out of range (equalize() then always fails).
    bool valid() const noexcept { return valid_; }

    // rx, h1 and h2 are one entry per symbol, and `llrs` must be the same length. `delay`
    // is 1 to max_delay; `noise_var` is the noise variance per complex dimension and must
    // be positive. Returns false, writing nothing, on any size or range violation.
    bool equalize(std::span<const std::complex<real>> rx,
                  std::span<const std::complex<real>> h1,
                  std::span<const std::complex<real>> h2,
                  int delay,
                  real noise_var,
                  std::span<real> llrs) noexcept;

private:
    bool valid_;
    std::size_t max_n_;
    int max_delay_;
    std::vector<real> alpha_;
    std::vector<real> beta_;
};

// Chain decomposed exact BCJR for the same channel, in single or double precision (T).
// Because y[t] couples x[t] only to x[t - d], the symbol dependency graph splits into d independent memory 1 chains
// {c, c + d, c + 2d, ...}; each chain's BCJR carries M states (M = constellation size)
// instead of M^delay, so the cost is O(n M^2) whatever the delay. Exact MAP under the two
// tap model: true log-sum-exp. The d symbols before the block enter as known constants in
// each chain's first branch metric; data symbols in the final d positions get no echo
// observation from beyond the block.
template <class T>
class BasicChainBcjr {
public:
    // `max_symbols` is the longest block and `max_constellation` the largest M that
    // equalize() will be given. Scratch is (max_symbols * M + 3 * M + max_symbols) reals.
    BasicChainBcjr(std::size_t max_symbols, std::size_t max_constellation);
    // Unconfigured (equalize() fails) until configure(), which sets it up in caller storage
    // (storage_bytes() of it, any alignment, kept in place) and allocates nothing.
    BasicChainBcjr() noexcept = default;
    BasicChainBcjr(const BasicChainBcjr&) = delete;
    BasicChainBcjr& operator=(const BasicChainBcjr&) = delete;
    static std::size_t storage_bytes(std::size_t max_symbols, std::size_t max_constellation) noexcept;
    bool configure(std::size_t max_symbols, std::size_t max_constellation, std::span<std::uint8_t> storage) noexcept;

    struct Input {
        // One entry per symbol (rx, h1, h2): received samples in the equalised and
        // descrambled domain, main tap gain, echo tap gain.
        std::span<const std::complex<T>> rx;
        std::span<const std::complex<T>> h1;
        std::span<const std::complex<T>> h2;
        int delay = 1;       // echo delay in symbols, d >= 1
        T noise_var = 0;     // noise variance per complex dimension
        // The M candidate symbols (descrambled domain).
        std::span<const tables::Complex32> constellation;
        // Per symbol index bit label, MSB first (bit b of symbol s is
        // (bit_labels[s] >> (bits_per_symbol - 1 - b)) & 1); empty means label = index.
        std::span<const std::uint8_t> bit_labels;
        int bits_per_symbol = 1;
        // The d known symbols before the block in natural time order (preceding[k] =
        // x[k - d]); empty treats the pre block echo as absent. Otherwise at least d long.
        std::span<const tables::Complex32> preceding;
        // Optional per symbol log priors, log P(x[t] = constellation[s]) at [t * M + s] up
        // to a per symbol constant; empty runs prior free. Each prior enters its branch
        // metric once, so the outputs are full posteriors.
        std::span<const T> symbol_log_priors;
        // Optional per position noise variance (length rx.size()) overriding noise_var;
        // any other length (including empty) broadcasts the scalar, as the C# does.
        std::span<const T> noise_var_per_symbol;
    };

    // Writes rx.size() * bits_per_symbol LLRs to `llrs`, bit b of symbol t at
    // [t * bits_per_symbol + b], positive meaning bit 0. Returns false, writing nothing,
    // on any size or range violation (including more symbols or a larger constellation
    // than the constructor was given).
    bool equalize(const Input& in, std::span<T> llrs) noexcept;

private:
    std::size_t max_n_ = 0;
    std::size_t max_m_ = 0;
    T* inv2_ = nullptr;
    T* alpha_ = nullptr;
    T* beta_ = nullptr;
    T* post_ = nullptr;
    std::vector<T> owned_;  // the allocating constructor's storage
};

// The chain BCJR in the build's precision (docs/dev/soft-decoders.md). The receiver's turbo stage
// uses BasicChainBcjr<float>, the C#'s own precision, in both builds (docs/dev/dfe-receiver.md).
using ChainBcjr = BasicChainBcjr<real>;
extern template class BasicChainBcjr<float>;
extern template class BasicChainBcjr<double>;

}  // inline namespace

}  // namespace m0lte::ms110d
