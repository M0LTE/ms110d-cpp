// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

// The probe-trained decision-feedback equaliser: the C++ counterpart of the C# Dfe (v0.2.1).
// Internal: not installed and not part of the locked public API; the demodulator that drives it
// is a later story. See docs/dev/equaliser.md for the design, the floating point choices, the
// memory needed per mode and how it is tested against the C#.
//
// A fractionally spaced (T/2) feed-forward filter plus a symbol-spaced feedback filter over past
// decisions. Taps start from a regularised least-squares solve over known symbols (a preamble
// tail and the first probe), are re-solved at every mini-probe (anchored to the current taps),
// and between probes follow the data by recursive least squares (or NLMS). The C# also has a
// target-impulse-response (channel shortening) solve, used by the turbo pass, and a soft-label
// training row; both are here.
//
// Probe interface. The equaliser never decides what a probe contains. The caller hands it the
// known symbols (the probe and the symbols before it, as many as the feedback filter needs) and
// a function that fills the T/2 window for a given symbol index (train_on_known), or adds rows
// one at a time with the desired symbol (add_training_row). Which probe applies at a given
// site, including a probe whose pattern was detected rather than assumed, is the caller's
// business.
//
// Memory. The object allocates nothing, ever, and throws nothing. All its working memory is one
// block that the caller supplies to configure(); storage_bytes() says how big for the tap
// counts. The object itself is a handful of pointers and counters. Total taps (feed-forward
// plus feedback) are at most max_taps.
//
// Conventions as the C#: the feed-forward window holds T/2 input samples newest first
// (window[i] = x[2n + lead - i]); the feedback window holds prior decisions (past[j] is the
// decision j + 1 symbols back). The output is y = sum ff[i] * window[i] + sum fb[j] * past[j].

#include <m0lte/ms110d/config.hpp>

#include "tables.hpp"

#include <complex>
#include <cstddef>
#include <cstdint>
#include <span>

namespace m0lte::ms110d {

// A complex sample, single precision as in the C# (Complex32).
using EqSample = tables::Complex32;

// Tap counts and ridges per mini-probe length, from the C# demodulator's InitializeDfe: the feed
// forward and feedback tap counts, the lead (T/2 samples ahead of the cursor in the window), the
// ridge of the initial solve and of the per-probe tracking solve.
struct EqParams {
    int ff_taps;
    int fb_taps;
    int lead;
    float init_ridge;
    float track_ridge;
};

// The C# values for a probe length K: 48 gives 32 FF / 22 FB, 24 gives 16 / 6, anything else
// (the 3 kHz K = 32, and 36) gives 24 / 12.
constexpr EqParams eq_params_for_probe_length(int k) noexcept {
    if (k == 48) return {32, 22, 16, 1.0f, 8.0f};
    if (k == 24) return {16, 6, 8, 1e-3f, 0.15f};
    return {24, 12, 13, 1e-3f, 0.15f};
}

// The most taps (feed-forward plus feedback) one equaliser can have. The C# fixes its scratch at
// 64 too (it throws for more). The largest in use is 54.
inline constexpr int eq_max_taps = 64;

namespace eq_detail {
// Storage element for taps and matrices: two floats, laid out like std::complex<float>. The
// arithmetic is in equaliser.cpp (it follows the C# Complex32 operation by operation).
struct Cx {
    float re;
    float im;
};
}  // namespace eq_detail

// Result of Dfe::solve_training_tir. lag == 0 means the full-inversion (null) candidate won:
// feed-forward taps installed, no designed echo. lag > 0 means the shortened solve was accepted:
// the post-feed-forward response is proportional to x[u] + coefficient * x[u - lag]
// (+ coefficient2 * x[u - lag2] when lag2 > 0). sse_null and sse_tir are the exact (unridged)
// residual sums, for diagnostics.
struct TirSolve {
    bool solved = false;
    int lag = 0;
    EqSample coefficient{};
    int lag2 = 0;
    EqSample coefficient2{};
    float sse_null = 0.0f;
    float sse_tir = 0.0f;
};

// Fills the T/2 window for the known symbol at `index` (see train_on_known). No allocation, no
// exceptions.
using WindowFill = void (*)(void* context, int index, std::span<EqSample> window) noexcept;

#ifdef MS110D_SINGLE_PRECISION
inline namespace v_single {
#else
inline namespace v_double {
#endif

// What train_on_known measured while it added rows.
struct ProbeStats {
    EqSample correlation{};   // sum of y * conj(known), y the output before the row is added
    real error_energy = 0;    // sum of |y - known|^2, accumulated in `real` as the C# does in double
    int rows = 0;             // rows added
};

class Dfe {
public:
    Dfe() noexcept = default;
    Dfe(const Dfe&) = delete;
    Dfe& operator=(const Dfe&) = delete;
    // Not movable: the object points into the caller's block, and two objects must never share one.
    Dfe(Dfe&&) = delete;
    Dfe& operator=(Dfe&&) = delete;

    // Bytes of storage configure() needs for these tap counts, or 0 if they are out of range
    // (ff_taps at least 1, fb_taps at least 0, together at most eq_max_taps).
    static std::size_t storage_bytes(int ff_taps, int fb_taps) noexcept;

    // Builds the equaliser in `storage` (any alignment; at least storage_bytes() bytes, kept in
    // place and unused by anything else while this object is used). Taps are zero. False, with
    // the object unchanged, if the tap counts are out of range or the storage is too small.
    bool configure(int ff_taps, int fb_taps, std::span<std::uint8_t> storage) noexcept;
    bool configured() const noexcept { return ff_ != nullptr; }

    int ff_taps() const noexcept { return nff_; }
    int fb_taps() const noexcept { return nfb_; }

    // Diagnostic: sum |ff|^2 of the feed-forward taps, the equaliser's white-noise power gain.
    float ff_energy() const noexcept;

    // Equalises one symbol: `window` holds ff_taps() samples, `past` fb_taps() decisions.
    EqSample equalise(std::span<const EqSample> window, std::span<const EqSample> past) const noexcept;

    // One normalised-LMS update toward `desired`; returns the output before the update.
    EqSample nlms(std::span<const EqSample> window, std::span<const EqSample> past, EqSample desired,
                  float mu) noexcept;

    // Taps, feed-forward then feedback (ff_taps() + fb_taps() entries).
    void snapshot_taps(std::span<EqSample> out) const noexcept;
    void load_taps(std::span<const EqSample> taps) noexcept;
    // (1 - alpha) * a + alpha * b.
    void load_interpolated_taps(std::span<const EqSample> a, std::span<const EqSample> b, float alpha) noexcept;
    // Adds (to - from) to the current taps.
    void translate_taps(std::span<const EqSample> from, std::span<const EqSample> to) noexcept;
    // Multiplies every tap by `rotor` (a unit phasor).
    void rotate_taps(EqSample rotor) noexcept;

    // Least-squares training. begin_training() clears the accumulation. add_training_row() adds
    // one row: the window and known past symbols observed when `desired` was current, with a
    // weight (known probe symbols authoritative, decision-directed rows advisory). The overload
    // with `past_variance` is the soft-label row (past entries are expectations; the variance is
    // added on the feedback diagonal). Both return false, adding nothing, unless training was
    // begun (the C# throws).
    void begin_training() noexcept;
    bool add_training_row(std::span<const EqSample> window, std::span<const EqSample> past, EqSample desired,
                          float weight = 1.0f) noexcept;
    bool add_training_row(std::span<const EqSample> window, std::span<const EqSample> past,
                          std::span<const float> past_variance, EqSample desired, float weight = 1.0f) noexcept;

    // Saves and restores the in-progress accumulation, so a nested re-training pass can run
    // without destroying the rows gathered for the next probe solve. restore_training() is false
    // (and does nothing) if nothing was saved (the C# throws).
    void snapshot_training() noexcept;
    bool restore_training() noexcept;

    // Adds one row per known symbol index in [first_row, known.size()): fill(context, index,
    // window) provides the window, the past is known[index - 1 - j], the output is taken (for the
    // returned statistics) and the row is added with `weight` and desired known[index]. This is
    // the C# probe-site loop with the symbols supplied by the caller. Needs first_row >= the
    // feedback tap count and training begun; otherwise adds nothing and returns rows == 0.
    ProbeStats train_on_known(std::span<const EqSample> known, int first_row, WindowFill fill, void* context,
                              float weight) noexcept;
    // The same sweep without adding rows or touching the training state: equalises each known
    // symbol with the current taps and returns the statistics (correlation sum of y * conj(known)
    // and error energy). For scoring a trial probe pattern against the taps without training on it.
    // Does not need training to have begun. Rows == 0 if first_row is below the feedback tap count.
    ProbeStats score_known(std::span<const EqSample> known, int first_row, WindowFill fill, void* context) const noexcept;

    // Solves the accumulated regularised normal equations and installs the taps; false (taps
    // unchanged) if degenerate or too few rows. The accumulation is consumed either way.
    // anchor_to_current_taps pulls the ridge toward the current taps instead of zero;
    // ff_noise_power adds sigma^2 * sum(weight) to the feed-forward diagonal (a channel-truth
    // genie in the C#; 0 in normal use).
    bool solve_training(float regularization = 1e-3f, bool anchor_to_current_taps = false,
                        float ff_noise_power = 0.0f) noexcept;

    // The channel-shortening variant used by the turbo pass: see the C# SolveTrainingTir. The
    // accumulation is consumed either way.
    TirSolve solve_training_tir(float regularization, float ff_noise_power, int max_lag, bool allow_pair = true,
                                int only_lag = 0) noexcept;

    // Recursive least squares. begin_rls() sets P to p_init * I. seed_rls_from_training() sets P
    // from the inverse of the accumulated Gram (identity times p_fallback if it is degenerate);
    // call it before solve_training(), which consumes the accumulation. rls_update() returns the
    // output before the update. symmetrize_p() enforces Hermitian symmetry and caps the diagonal.
    void begin_rls(float lambda, float p_init = 1.0f) noexcept;
    void seed_rls_from_training(float regularization, float p_fallback = 1.0f, float ff_noise_power = 0.0f) noexcept;
    EqSample rls_update(std::span<const EqSample> window, std::span<const EqSample> past, EqSample desired,
                        float weight = 1.0f) noexcept;
    void symmetrize_p(float p_max = 10.0f) noexcept;

private:
    using Cx = eq_detail::Cx;

    ProbeStats sweep_known(std::span<const EqSample> known, int first_row, WindowFill fill, void* context,
                           float weight, bool add_rows) noexcept;
    bool cholesky_factor(const Cx* a, int n) noexcept;
    void cholesky_substitute(const Cx* b, Cx* x, int n) noexcept;
    float solve_subset(std::span<const int> fb_indices, float regularization, float ff_noise_power, Cx* sol) noexcept;
    bool prepare_floating_tir(float regularization, float ff_noise_power) noexcept;
    float solve_floating_candidate(std::span<const int> fb_indices, Cx* sol, Cx* t) noexcept;

    int nff_ = 0;
    int nfb_ = 0;
    int ld_ = 0;  // nff_ + nfb_: the row length of every square matrix

    // Pointers into the caller's block (see equaliser.cpp for the layout).
    Cx* ff_ = nullptr;
    Cx* fb_ = nullptr;
    Cx* gram_ = nullptr;        // accumulated Gram (upper triangle), ld_ x ld_
    Cx* rhs_ = nullptr;
    Cx* chol_l_ = nullptr;      // Cholesky factor scratch
    Cx* chol_y_ = nullptr;
    Cx* solution_ = nullptr;
    Cx* tir_gram_ = nullptr;    // subset Gram scratch; also the seed Gram of seed_rls_from_training
    Cx* tir_rhs_ = nullptr;
    Cx* tir_sol_ = nullptr;
    Cx* tir_win_ = nullptr;
    Cx* tir_cols_ = nullptr;    // three columns of nff_
    Cx* saved_gram_ = nullptr;
    Cx* saved_rhs_ = nullptr;
    Cx* p_ = nullptr;           // RLS inverse correlation, ld_ x ld_
    Cx* px_ = nullptr;
    EqSample* win_ = nullptr;   // window and past scratch for train_on_known
    EqSample* past_ = nullptr;

    // Training state (the C# nullable _gram/_rhs is gram_active_).
    bool gram_active_ = false;
    int training_rows_ = 0;
    float training_weight_sum_ = 0.0f;
    float training_target_energy_ = 0.0f;
    bool has_saved_ = false;
    int saved_rows_ = 0;
    float saved_weight_sum_ = 0.0f;
    float saved_target_energy_ = 0.0f;
    bool p_active_ = false;
    float lambda_ = 1.0f;
};

}  // inline namespace (precision)

}  // namespace m0lte::ms110d
