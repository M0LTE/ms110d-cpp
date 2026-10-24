// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "equaliser.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <new>

// Floating point. The C# Dfe does its complex arithmetic in single precision (Complex32) and only
// its running sums in double: the trace of a Gram matrix, and the residual sums lin, quad, nrm,
// dt and sse. This port keeps exactly that split. Complex values and every float the C# declares
// float stay float in both builds, with the same operation order as Complex32 (the products are
// written out, and -ffp-contract=off keeps them unfused), so the double build tracks the C#
// bit for bit apart from libm (log). Where the C# says double the code says `real`: double in the
// default build, float in the single build. The single build therefore differs from the C# only
// in those sums.

namespace m0lte::ms110d {

#ifdef MS110D_SINGLE_PRECISION
inline namespace v_single {
#else
inline namespace v_double {
#endif

namespace {

using Cx = eq_detail::Cx;

inline Cx cx(float re, float im) noexcept { return Cx{re, im}; }
inline Cx cx(EqSample v) noexcept { return Cx{v.real(), v.imag()}; }
inline EqSample out(Cx v) noexcept { return EqSample{v.re, v.im}; }
inline Cx operator+(Cx a, Cx b) noexcept { return Cx{a.re + b.re, a.im + b.im}; }
inline Cx operator-(Cx a, Cx b) noexcept { return Cx{a.re - b.re, a.im - b.im}; }
inline Cx operator*(Cx a, Cx b) noexcept {
    return Cx{(a.re * b.re) - (a.im * b.im), (a.re * b.im) + (a.im * b.re)};
}
inline Cx operator*(Cx a, float s) noexcept { return Cx{a.re * s, a.im * s}; }
inline Cx& operator+=(Cx& a, Cx b) noexcept { return a = a + b; }
inline Cx& operator-=(Cx& a, Cx b) noexcept { return a = a - b; }
inline Cx& operator*=(Cx& a, Cx b) noexcept { return a = a * b; }
inline Cx conj(Cx a) noexcept { return Cx{a.re, -a.im}; }
inline float mag2(Cx a) noexcept { return (a.re * a.re) + (a.im * a.im); }
inline constexpr Cx zero{0.0f, 0.0f};

// max(0, v) that passes a NaN through, as Math.Max does in the C# (std::max would return 0 for a NaN
// and hide it from the callers' NaN checks).
inline real nonneg(real v) noexcept { return v != v ? v : (v > 0 ? v : static_cast<real>(0)); }


// Layout of the block, in elements, in order. Everything is 4 byte aligned (floats).
struct Layout {
    std::size_t cx_count;       // all Cx arrays
    std::size_t sample_count;   // the EqSample scratch
    std::size_t bytes;
};

constexpr std::size_t ssz(int v) noexcept { return static_cast<std::size_t>(v); }

Layout layout_for(int ff, int fb) noexcept {
    const std::size_t f = ssz(ff), b = ssz(fb), n = f + b;
    Layout l{};
    l.cx_count = f + b            // ff, fb
                 + n * n + n      // gram, rhs
                 + n * n + n      // chol_l, chol_y
                 + n              // solution
                 + n * n + n      // tir_gram (also seed gram), tir_rhs
                 + n + f          // tir_sol, tir_win
                 + 3 * f          // tir_cols
                 + n * n + n      // saved gram, rhs
                 + n * n + n;     // p, px
    l.sample_count = f + b;       // window and past scratch
    l.bytes = (l.cx_count + l.sample_count) * sizeof(Cx) + alignof(Cx) - 1;
    return l;
}

}  // namespace

std::size_t Dfe::storage_bytes(int ff_taps, int fb_taps) noexcept {
    if (ff_taps < 1 || fb_taps < 0 || ff_taps + fb_taps > eq_max_taps) return 0;
    return layout_for(ff_taps, fb_taps).bytes;
}

bool Dfe::configure(int ff_taps, int fb_taps, std::span<std::uint8_t> storage) noexcept {
    static_assert(sizeof(Cx) == sizeof(EqSample) && alignof(Cx) == alignof(EqSample));
    if (ff_taps < 1 || fb_taps < 0 || ff_taps + fb_taps > eq_max_taps) return false;
    const Layout l = layout_for(ff_taps, fb_taps);
    void* where = storage.data();
    std::size_t space = storage.size();
    if (std::align(alignof(Cx), (l.cx_count + l.sample_count) * sizeof(Cx), where, space) == nullptr) return false;
    std::memset(where, 0, (l.cx_count + l.sample_count) * sizeof(Cx));

    const std::size_t f = ssz(ff_taps), b = ssz(fb_taps), n = f + b;
    Cx* at = static_cast<Cx*>(where);
    auto take = [&at](std::size_t count) {
        Cx* p = at;
        at += count;
        return p;
    };
    ff_ = take(f);
    fb_ = take(b);
    gram_ = take(n * n);
    rhs_ = take(n);
    chol_l_ = take(n * n);
    chol_y_ = take(n);
    solution_ = take(n);
    tir_gram_ = take(n * n);
    tir_rhs_ = take(n);
    tir_sol_ = take(n);
    tir_win_ = take(f);
    tir_cols_ = take(3 * f);
    saved_gram_ = take(n * n);
    saved_rhs_ = take(n);
    p_ = take(n * n);
    px_ = take(n);
    win_ = reinterpret_cast<EqSample*>(take(f));
    past_ = reinterpret_cast<EqSample*>(take(b));
    nff_ = ff_taps;
    nfb_ = fb_taps;
    ld_ = ff_taps + fb_taps;
    gram_active_ = false;
    training_rows_ = 0;
    training_weight_sum_ = 0.0f;
    training_target_energy_ = 0.0f;
    has_saved_ = false;
    p_active_ = false;
    lambda_ = 1.0f;
    return true;
}

float Dfe::ff_energy() const noexcept {
    float e = 0.0f;
    for (int i = 0; i < nff_; ++i) e += mag2(ff_[i]);
    return e;
}

EqSample Dfe::equalise(std::span<const EqSample> window, std::span<const EqSample> past) const noexcept {
    Cx y = zero;
    for (int i = 0; i < nff_; ++i) y += ff_[i] * cx(window[ssz(i)]);
    for (int j = 0; j < nfb_; ++j) y += fb_[j] * cx(past[ssz(j)]);
    return out(y);
}

EqSample Dfe::nlms(std::span<const EqSample> window, std::span<const EqSample> past, EqSample desired,
                   float mu) noexcept {
    const Cx y = cx(equalise(window, past));
    const Cx error = cx(desired) - y;
    float norm = 1e-6f;
    for (int i = 0; i < nff_; ++i) norm += mag2(cx(window[ssz(i)]));
    for (int j = 0; j < nfb_; ++j) norm += mag2(cx(past[ssz(j)]));
    const float g = mu / norm;
    const Cx scaled = error * g;
    for (int i = 0; i < nff_; ++i) ff_[i] += scaled * conj(cx(window[ssz(i)]));
    for (int j = 0; j < nfb_; ++j) fb_[j] += scaled * conj(cx(past[ssz(j)]));
    return out(y);
}

void Dfe::snapshot_taps(std::span<EqSample> o) const noexcept {
    for (int i = 0; i < nff_; ++i) o[ssz(i)] = out(ff_[i]);
    for (int j = 0; j < nfb_; ++j) o[ssz(nff_ + j)] = out(fb_[j]);
}

void Dfe::load_taps(std::span<const EqSample> taps) noexcept {
    for (int i = 0; i < nff_; ++i) ff_[i] = cx(taps[ssz(i)]);
    for (int j = 0; j < nfb_; ++j) fb_[j] = cx(taps[ssz(nff_ + j)]);
}

void Dfe::load_interpolated_taps(std::span<const EqSample> a, std::span<const EqSample> b, float alpha) noexcept {
    const float inverse = 1 - alpha;
    for (int i = 0; i < nff_; ++i) ff_[i] = (cx(a[ssz(i)]) * inverse) + (cx(b[ssz(i)]) * alpha);
    for (int j = 0; j < nfb_; ++j) {
        const std::size_t i = ssz(nff_ + j);
        fb_[j] = (cx(a[i]) * inverse) + (cx(b[i]) * alpha);
    }
}

void Dfe::translate_taps(std::span<const EqSample> from, std::span<const EqSample> to) noexcept {
    for (int i = 0; i < nff_; ++i) ff_[i] += cx(to[ssz(i)]) - cx(from[ssz(i)]);
    for (int j = 0; j < nfb_; ++j) {
        const std::size_t i = ssz(nff_ + j);
        fb_[j] += cx(to[i]) - cx(from[i]);
    }
}

void Dfe::rotate_taps(EqSample rotor) noexcept {
    const Cx r = cx(rotor);
    for (int i = 0; i < nff_; ++i) ff_[i] *= r;
    for (int j = 0; j < nfb_; ++j) fb_[j] *= r;
}

void Dfe::begin_training() noexcept {
    const std::size_t n = ssz(ld_);
    std::fill_n(gram_, n * n, zero);
    std::fill_n(rhs_, n, zero);
    gram_active_ = true;
    training_rows_ = 0;
    training_weight_sum_ = 0.0f;
    training_target_energy_ = 0.0f;
}

void Dfe::snapshot_training() noexcept {
    const std::size_t n = ssz(ld_);
    std::copy_n(gram_, n * n, saved_gram_);
    std::copy_n(rhs_, n, saved_rhs_);
    has_saved_ = true;
    saved_rows_ = training_rows_;
    saved_weight_sum_ = training_weight_sum_;
    saved_target_energy_ = training_target_energy_;
}

bool Dfe::restore_training() noexcept {
    if (!has_saved_) return false;
    const std::size_t n = ssz(ld_);
    std::copy_n(saved_gram_, n * n, gram_);
    std::copy_n(saved_rhs_, n, rhs_);
    gram_active_ = true;
    training_rows_ = saved_rows_;
    training_weight_sum_ = saved_weight_sum_;
    training_target_energy_ = saved_target_energy_;
    return true;
}

bool Dfe::add_training_row(std::span<const EqSample> window, std::span<const EqSample> past, EqSample desired_in,
                           float weight) noexcept {
    if (!gram_active_) return false;
    const int n = ld_;
    const Cx desired = cx(desired_in);
    Cx row[eq_max_taps];
    for (int i = 0; i < nff_; ++i) row[i] = cx(window[ssz(i)]);
    for (int j = 0; j < nfb_; ++j) row[nff_ + j] = cx(past[ssz(j)]);
    for (int i = 0; i < n; ++i) {
        const Cx ci = conj(row[i]) * weight;
        for (int j = i; j < n; ++j) gram_[ssz(i) * ssz(n) + ssz(j)] += ci * row[j];
        rhs_[i] += ci * desired;
    }
    ++training_rows_;
    training_weight_sum_ += weight;
    training_target_energy_ += weight * mag2(desired);
    return true;
}

bool Dfe::add_training_row(std::span<const EqSample> window, std::span<const EqSample> past,
                           std::span<const float> past_variance, EqSample desired, float weight) noexcept {
    if (!add_training_row(window, past, desired, weight)) return false;
    for (int j = 0; j < nfb_; ++j) {
        const std::size_t d = ssz(nff_ + j);
        gram_[d * ssz(ld_) + d] += cx(weight * past_variance[ssz(j)], 0.0f);
    }
    return true;
}

ProbeStats Dfe::sweep_known(std::span<const EqSample> known, int first_row, WindowFill fill, void* context,
                            float weight, bool add_rows) noexcept {
    ProbeStats stats;
    if (first_row < nfb_ || (add_rows && !gram_active_)) return stats;
    Cx correlation = zero;
    real err = 0;
    const std::span<EqSample> window{win_, ssz(nff_)};
    const std::span<EqSample> past{past_, ssz(nfb_)};
    for (int i = first_row; i < static_cast<int>(known.size()); ++i) {
        fill(context, i, window);
        for (int j = 0; j < nfb_; ++j) past[ssz(j)] = known[ssz(i - 1 - j)];
        const Cx y = cx(equalise(window, past));
        const Cx k = cx(known[ssz(i)]);
        correlation += y * conj(k);
        err += static_cast<real>(mag2(y - k));
        if (add_rows) add_training_row(window, past, known[ssz(i)], weight);
        ++stats.rows;
    }
    stats.correlation = out(correlation);
    stats.error_energy = err;
    return stats;
}

ProbeStats Dfe::train_on_known(std::span<const EqSample> known, int first_row, WindowFill fill, void* context,
                               float weight) noexcept {
    return sweep_known(known, first_row, fill, context, weight, true);
}

ProbeStats Dfe::score_known(std::span<const EqSample> known, int first_row, WindowFill fill, void* context) const noexcept {
    // Reads the taps and writes only the window scratch, which is not part of the equaliser's state.
    return const_cast<Dfe*>(this)->sweep_known(known, first_row, fill, context, 0.0f, false);
}

bool Dfe::cholesky_factor(const Cx* a, int n) noexcept {
    Cx* l = chol_l_;
    const std::size_t ld = ssz(ld_);
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j <= i; ++j) {
            Cx sum = a[ssz(i) * ld + ssz(j)];
            for (int k = 0; k < j; ++k) sum -= l[ssz(i) * ld + ssz(k)] * conj(l[ssz(j) * ld + ssz(k)]);
            if (i == j) {
                if (!(sum.re > 0)) return false;
                l[ssz(i) * ld + ssz(j)] = cx(std::sqrt(sum.re), 0.0f);
            } else {
                const float inv = 1.0f / l[ssz(j) * ld + ssz(j)].re;
                l[ssz(i) * ld + ssz(j)] = sum * inv;
            }
        }
    }
    return true;
}

void Dfe::cholesky_substitute(const Cx* b, Cx* x, int n) noexcept {
    const Cx* l = chol_l_;
    Cx* y = chol_y_;
    const std::size_t ld = ssz(ld_);
    for (int i = 0; i < n; ++i) {
        Cx sum = b[i];
        for (int k = 0; k < i; ++k) sum -= l[ssz(i) * ld + ssz(k)] * y[k];
        y[i] = sum * (1.0f / l[ssz(i) * ld + ssz(i)].re);
    }
    for (int i = n - 1; i >= 0; --i) {
        Cx sum = y[i];
        for (int k = i + 1; k < n; ++k) sum -= conj(l[ssz(k) * ld + ssz(i)]) * x[k];
        x[i] = sum * (1.0f / l[ssz(i) * ld + ssz(i)].re);
    }
}

bool Dfe::solve_training(float regularization, bool anchor_to_current_taps, float ff_noise_power) noexcept {
    if (!gram_active_ || (!anchor_to_current_taps && training_rows_ < nff_ + nfb_) || training_rows_ == 0) {
        gram_active_ = false;
        return false;
    }
    const int n = ld_;
    const std::size_t ld = ssz(ld_);
    if (ff_noise_power > 0) {
        const float noise_diag = ff_noise_power * training_weight_sum_;
        for (int i = 0; i < nff_; ++i) gram_[ssz(i) * ld + ssz(i)] += cx(noise_diag, 0.0f);
    }
    real trace = 0;
    for (int i = 0; i < n; ++i) trace += static_cast<real>(gram_[ssz(i) * ld + ssz(i)].re);
    const float lambda = static_cast<float>(static_cast<real>(regularization) * trace / static_cast<real>(n)) + 1e-9f;
    for (int i = 0; i < n; ++i) {
        gram_[ssz(i) * ld + ssz(i)] += cx(lambda, 0.0f);
        if (anchor_to_current_taps) {
            const Cx current = i < nff_ ? ff_[i] : fb_[i - nff_];
            rhs_[i] += current * lambda;
        }
        for (int j = 0; j < i; ++j) gram_[ssz(i) * ld + ssz(j)] = conj(gram_[ssz(j) * ld + ssz(i)]);
    }
    if (!cholesky_factor(gram_, n)) {
        gram_active_ = false;
        return false;
    }
    cholesky_substitute(rhs_, solution_, n);
    // A NaN or infinity in the data reaches the solution through the right-hand side without upsetting
    // the factorisation; refuse it rather than install it. Finite data is unaffected.
    for (int i = 0; i < n; ++i) {
        if (!std::isfinite(solution_[i].re) || !std::isfinite(solution_[i].im)) {
            gram_active_ = false;
            return false;
        }
    }
    std::copy_n(solution_, nff_, ff_);
    std::copy_n(solution_ + nff_, nfb_, fb_);
    gram_active_ = false;
    return true;
}

float Dfe::solve_subset(std::span<const int> fb_indices, float regularization, float ff_noise_power, Cx* sol) noexcept {
    const int f = nff_;
    const int m = f + static_cast<int>(fb_indices.size());
    const std::size_t ld = ssz(ld_);
    const Cx* gram = gram_;
    const Cx* rhs = rhs_;
    Cx* tg = tir_gram_;

    auto map = [&](int i) { return i < f ? i : f + fb_indices[ssz(i - f)]; };
    for (int i = 0; i < m; ++i) {
        const int gi = map(i);
        for (int j = i; j < m; ++j) {
            const int gj = map(j);
            const Cx v = gram[ssz(gi) * ld + ssz(gj)];
            tg[ssz(i) * ld + ssz(j)] = v;
            if (j != i) tg[ssz(j) * ld + ssz(i)] = conj(v);
        }
    }

    const float noise_diag = ff_noise_power > 0 ? ff_noise_power * training_weight_sum_ : 0.0f;
    if (noise_diag > 0) {
        for (int i = 0; i < f; ++i) tg[ssz(i) * ld + ssz(i)] += cx(noise_diag, 0.0f);
    }
    real trace = 0;
    for (int i = 0; i < m; ++i) trace += static_cast<real>(tg[ssz(i) * ld + ssz(i)].re);
    const float lambda = static_cast<float>(static_cast<real>(regularization) * trace / static_cast<real>(m)) + 1e-9f;
    for (int k = 0; k < m; ++k) {
        const int gk = map(k);
        const Cx anchor = k < f ? ff_[k] : fb_[fb_indices[ssz(k - f)]];
        tg[ssz(k) * ld + ssz(k)] += cx(lambda, 0.0f);
        tir_rhs_[k] = rhs[gk] + (anchor * lambda);
    }
    if (!cholesky_factor(tg, m)) return std::nanf("");
    cholesky_substitute(tir_rhs_, sol, m);

    real lin = 0, quad = 0, nrm = 0, nrm_ff = 0;
    for (int i = 0; i < m; ++i) {
        const int gi = map(i);
        lin += static_cast<real>((conj(sol[i]) * rhs[gi]).re);
        Cx acc = zero;
        for (int j = 0; j < m; ++j) acc += tg[ssz(i) * ld + ssz(j)] * sol[j];
        quad += static_cast<real>((conj(sol[i]) * acc).re);
        nrm += static_cast<real>(mag2(sol[i]));
        if (i < f) nrm_ff += static_cast<real>(mag2(sol[i]));
    }
    const real sse = static_cast<real>(training_target_energy_) + quad - (2 * lin) -
                     (static_cast<real>(lambda) * nrm) - (static_cast<real>(noise_diag) * nrm_ff);
    return static_cast<float>(nonneg(sse));
}

bool Dfe::prepare_floating_tir(float regularization, float ff_noise_power) noexcept {
    const int f = nff_;
    const std::size_t ld = ssz(ld_);
    Cx* tg = tir_gram_;
    for (int i = 0; i < f; ++i) {
        for (int j = i; j < f; ++j) {
            const Cx v = gram_[ssz(i) * ld + ssz(j)];
            tg[ssz(i) * ld + ssz(j)] = v;
            if (j != i) tg[ssz(j) * ld + ssz(i)] = conj(v);
        }
    }
    const float noise_diag = ff_noise_power > 0 ? ff_noise_power * training_weight_sum_ : 0.0f;
    real trace = 0;
    for (int i = 0; i < f; ++i) {
        tg[ssz(i) * ld + ssz(i)] += cx(noise_diag, 0.0f);
        trace += static_cast<real>(tg[ssz(i) * ld + ssz(i)].re);
    }
    const float lambda = static_cast<float>(static_cast<real>(regularization) * trace / static_cast<real>(f)) + 1e-9f;
    for (int i = 0; i < f; ++i) tg[ssz(i) * ld + ssz(i)] += cx(lambda, 0.0f);
    if (!cholesky_factor(tg, f)) return false;
    for (int i = 0; i < f; ++i) tir_rhs_[i] = rhs_[i];
    cholesky_substitute(tir_rhs_, tir_cols_, f);
    return true;
}

float Dfe::solve_floating_candidate(std::span<const int> fb_indices, Cx* sol, Cx* t) noexcept {
    const int f = nff_;
    const int k = static_cast<int>(fb_indices.size());
    const int m = 1 + k;
    const std::size_t ld = ssz(ld_);
    const Cx* gram = gram_;
    const Cx* rhs = rhs_;
    Cx* cols = tir_cols_;  // column c at cols + c * f

    for (int j = 0; j < k; ++j) {
        const int gi = f + fb_indices[ssz(j)];
        for (int i = 0; i < f; ++i) tir_rhs_[i] = gram[ssz(i) * ld + ssz(gi)];
        cholesky_substitute(tir_rhs_, cols + ssz(1 + j) * ssz(f), f);
    }

    Cx d[9] = {}, r[9] = {};
    for (int a = 0; a < m; ++a) {
        for (int b = a; b < m; ++b) {
            Cx dv;
            if (a == 0 && b == 0) {
                dv = cx(training_target_energy_, 0.0f);
            } else if (a == 0) {
                dv = conj(rhs[f + fb_indices[ssz(b - 1)]]);
            } else {
                dv = gram[ssz(f + fb_indices[ssz(a - 1)]) * ld + ssz(f + fb_indices[ssz(b - 1)])];
            }
            Cx s = zero;
            const Cx* cb = cols + ssz(b) * ssz(f);
            for (int i = 0; i < f; ++i) {
                const Cx ba = a == 0 ? rhs[i] : gram[ssz(i) * ld + ssz(f + fb_indices[ssz(a - 1)])];
                s += conj(ba) * cb[i];
            }
            d[a * 3 + b] = dv;
            r[a * 3 + b] = dv - s;
            if (b != a) {
                d[b * 3 + a] = conj(dv);
                r[b * 3 + a] = conj(dv - s);
            }
        }
    }

    real tr = 0;
    for (int a = 0; a < m; ++a) tr += static_cast<real>(r[a * 3 + a].re);
    float eps = static_cast<float>(static_cast<real>(1e-6) * tr / static_cast<real>(m)) + 1e-12f;
    Cx l[9] = {}, y[3] = {}, v[3] = {};
    bool factored = false;
    for (int attempt = 0; attempt < 2 && !factored; attempt++, eps *= 100.0f) {
        factored = true;
        for (int i = 0; i < m && factored; ++i) {
            for (int j = 0; j <= i; ++j) {
                Cx sum = r[i * 3 + j];
                if (i == j) sum += cx(eps, 0.0f);
                for (int c = 0; c < j; ++c) sum -= l[i * 3 + c] * conj(l[j * 3 + c]);
                if (i == j) {
                    if (!(sum.re > 0)) {
                        factored = false;
                        break;
                    }
                    l[i * 3 + i] = cx(std::sqrt(sum.re), 0.0f);
                } else {
                    l[i * 3 + j] = sum * (1.0f / l[j * 3 + j].re);
                }
            }
        }
    }
    if (!factored) return std::nanf("");

    v[0] = cx(1.0f, 0.0f);
    for (int iter = 0; iter < 3; ++iter) {
        for (int i = 0; i < m; ++i) {
            Cx sum = v[i];
            for (int c = 0; c < i; ++c) sum -= l[i * 3 + c] * y[c];
            y[i] = sum * (1.0f / l[i * 3 + i].re);
        }
        for (int i = m - 1; i >= 0; --i) {
            Cx sum = y[i];
            for (int c = i + 1; c < m; ++c) sum -= conj(l[c * 3 + i]) * v[c];
            v[i] = sum * (1.0f / l[i * 3 + i].re);
        }
        float nrm = 0.0f;
        for (int i = 0; i < m; ++i) nrm += mag2(v[i]);
        if (nrm <= 0.0f || !std::isfinite(nrm)) return std::nanf("");
        const float inv = 1.0f / std::sqrt(nrm);
        for (int i = 0; i < m; ++i) v[i] = v[i] * inv;
    }

    int big = 0;
    for (int i = 1; i < m; ++i) {
        if (mag2(v[i]) > mag2(v[big])) big = i;
    }
    const Cx phase = v[big] * (1.0f / std::sqrt(mag2(v[big])));
    for (int i = 0; i < m; ++i) {
        v[i] *= conj(phase);
        t[i] = v[i];
    }

    for (int i = 0; i < f; ++i) {
        Cx acc = v[0] * cols[i];
        for (int j = 0; j < k; ++j) acc += v[1 + j] * cols[ssz(1 + j) * ssz(f) + ssz(i)];
        sol[i] = acc;
    }

    real dt = 0;
    for (int a = 0; a < m; ++a) {
        for (int b = 0; b < m; ++b) dt += static_cast<real>((conj(v[a]) * d[a * 3 + b] * v[b]).re);
    }
    real lin = 0;
    for (int i = 0; i < f; ++i) {
        Cx bt = rhs[i] * v[0];
        for (int j = 0; j < k; ++j) bt += gram[ssz(i) * ld + ssz(f + fb_indices[ssz(j)])] * v[1 + j];
        lin += static_cast<real>((conj(sol[i]) * bt).re);
    }
    real quad = 0;
    for (int i = 0; i < f; ++i) {
        Cx acc = zero;
        for (int j = 0; j < f; ++j) {
            acc += (j >= i ? gram[ssz(i) * ld + ssz(j)] : conj(gram[ssz(j) * ld + ssz(i)])) * sol[j];
        }
        quad += static_cast<real>((conj(sol[i]) * acc).re);
    }
    const real sse = dt - (2 * lin) + quad;
    return static_cast<float>(nonneg(sse));
}

TirSolve Dfe::solve_training_tir(float regularization, float ff_noise_power, int max_lag, bool allow_pair,
                                 int only_lag) noexcept {
    TirSolve fail;
    if (!gram_active_ || training_rows_ == 0) {
        gram_active_ = false;
        return fail;
    }
    const int f = nff_;
    max_lag = std::min(max_lag, nfb_);
    int one[1] = {0};
    int two[2] = {0, 0};
    float sse_null = solve_subset({}, regularization, ff_noise_power, tir_sol_);
    if (std::isnan(sse_null)) {
        gram_active_ = false;
        return fail;
    }
    std::copy_n(tir_sol_, f, tir_win_);
    int best_lag = 0;
    Cx best_b = zero;
    float best_sse = 3.402823466e+38f;  // float.MaxValue
    Cx t[3] = {};
    const int first_lag = only_lag > 0 ? only_lag : 1;
    const int last_lag = only_lag > 0 ? std::min(only_lag, max_lag) : max_lag;
    for (int lag = first_lag; lag <= last_lag; ++lag) {
        one[0] = lag - 1;
        const float sse = solve_subset(std::span<const int>{one, 1}, regularization, ff_noise_power, tir_sol_);
        if (!std::isnan(sse) && sse < best_sse) {
            best_sse = sse;
            best_lag = lag;
            best_b = tir_sol_[f];
            std::copy_n(tir_sol_, f, tir_win_);
        }
    }

    const float threshold = 4.0f * std::log(static_cast<float>(std::max(2, only_lag > 0 ? 1 : max_lag))) * sse_null /
                            static_cast<float>(training_rows_);
    const bool accept = best_lag > 0 && best_sse < sse_null - threshold;
    int lag2 = 0;
    Cx best_b2 = zero;
    if (accept && !allow_pair) {
        // Pair candidates suppressed: the accepted single-lag solve stands.
    } else if (accept) {
        float pair_gate = best_sse - (4.0f * sse_null / static_cast<float>(training_rows_));
        for (int side = -1; side <= 1; side += 2) {
            const int neighbor = best_lag + side;
            if (neighbor < 1 || neighbor > max_lag) continue;
            two[0] = std::min(best_lag, neighbor) - 1;
            two[1] = std::max(best_lag, neighbor) - 1;
            const float sse = solve_subset(std::span<const int>{two, 2}, regularization, ff_noise_power, tir_sol_);
            if (!std::isnan(sse) && sse < pair_gate) {
                pair_gate = sse;
                best_sse = sse;
                lag2 = neighbor;
                best_b = tir_sol_[f + (neighbor < best_lag ? 1 : 0)];
                best_b2 = tir_sol_[f + (neighbor < best_lag ? 0 : 1)];
                std::copy_n(tir_sol_, f, tir_win_);
            }
        }
    } else {
        sse_null = solve_subset({}, regularization, ff_noise_power, tir_win_);
        best_lag = 0;
        best_b = zero;
        best_sse = sse_null;
    }

    Cx coeff = zero;
    Cx coeff2 = zero;
    if (best_lag > 0) {
        coeff = cx(-best_b.re, -best_b.im);
        coeff2 = cx(-best_b2.re, -best_b2.im);
        if (prepare_floating_tir(regularization, ff_noise_power)) {
            std::span<const int> idx;
            if (lag2 > 0) {
                two[0] = std::min(best_lag, lag2) - 1;
                two[1] = std::max(best_lag, lag2) - 1;
                idx = std::span<const int>{two, 2};
            } else {
                one[0] = best_lag - 1;
                idx = std::span<const int>{one, 1};
            }
            const float fsse = solve_floating_candidate(idx, tir_sol_, t);
            if (!std::isnan(fsse)) {
                std::copy_n(tir_sol_, f, tir_win_);
                const Cx g = t[0];
                const Cx c1 = lag2 > 0 ? t[1 + (lag2 < best_lag ? 1 : 0)] : t[1];
                const Cx c2 = lag2 > 0 ? t[1 + (lag2 < best_lag ? 0 : 1)] : zero;
                const float g_n = mag2(g);
                if (g_n >= 1e-4f) {
                    const Cx inv_g = conj(g) * (1.0f / g_n);
                    coeff = c1 * inv_g;
                    coeff2 = c2 * inv_g;
                } else {
                    coeff = c1;
                    coeff2 = c2;
                }
            }
        }
    }

    std::copy_n(tir_win_, f, ff_);
    gram_active_ = false;
    TirSolve result;
    result.solved = true;
    result.lag = best_lag;
    result.coefficient = out(coeff);
    result.lag2 = lag2;
    result.coefficient2 = out(coeff2);
    result.sse_null = sse_null;
    result.sse_tir = best_sse;
    return result;
}

void Dfe::begin_rls(float lambda, float p_init) noexcept {
    const std::size_t n = ssz(ld_);
    std::fill_n(p_, n * n, zero);
    for (std::size_t i = 0; i < n; ++i) p_[i * n + i] = cx(p_init, 0.0f);
    lambda_ = lambda;
    p_active_ = true;
}

void Dfe::seed_rls_from_training(float regularization, float p_fallback, float ff_noise_power) noexcept {
    if (!gram_active_ || !p_active_) return;
    const int n = ld_;
    const std::size_t ld = ssz(ld_);
    Cx* gram = tir_gram_;
    std::copy_n(gram_, ld * ld, gram);
    if (ff_noise_power > 0) {
        const float noise_diag = ff_noise_power * training_weight_sum_;
        for (int i = 0; i < nff_; ++i) gram[ssz(i) * ld + ssz(i)] += cx(noise_diag, 0.0f);
    }
    real trace = 0;
    for (int i = 0; i < n; ++i) trace += static_cast<real>(gram[ssz(i) * ld + ssz(i)].re);
    const float ridge = static_cast<float>(static_cast<real>(regularization) * trace / static_cast<real>(n)) + 1e-9f;
    for (int i = 0; i < n; ++i) {
        gram[ssz(i) * ld + ssz(i)] += cx(ridge, 0.0f);
        for (int j = 0; j < i; ++j) gram[ssz(i) * ld + ssz(j)] = conj(gram[ssz(j) * ld + ssz(i)]);
    }
    const bool ok = cholesky_factor(gram, n);
    if (ok) {
        Cx* e = tir_rhs_;
        std::fill_n(e, n, zero);
        for (int col = 0; col < n; ++col) {
            e[col] = cx(1.0f, 0.0f);
            cholesky_substitute(e, solution_, n);
            for (int row = 0; row < n; ++row) p_[ssz(row) * ld + ssz(col)] = solution_[row];
            e[col] = zero;
        }
    } else {
        for (int i = 0; i < n; ++i) {
            for (int j = 0; j < n; ++j) p_[ssz(i) * ld + ssz(j)] = i == j ? cx(p_fallback, 0.0f) : zero;
        }
    }
}

EqSample Dfe::rls_update(std::span<const EqSample> window, std::span<const EqSample> past, EqSample desired,
                         float weight) noexcept {
    const Cx y = cx(equalise(window, past));
    if (!p_active_) return out(y);
    const int n = ld_;
    const std::size_t ld = ssz(ld_);
    Cx u[eq_max_taps];
    for (int i = 0; i < nff_; ++i) u[i] = cx(window[ssz(i)]);
    for (int j = 0; j < nfb_; ++j) u[nff_ + j] = cx(past[ssz(j)]);

    for (int i = 0; i < n; ++i) {
        Cx acc = zero;
        for (int j = 0; j < n; ++j) acc += p_[ssz(i) * ld + ssz(j)] * conj(u[j]);
        px_[i] = acc;
    }
    Cx u_px = zero;
    for (int j = 0; j < n; ++j) u_px += u[j] * px_[j];

    const float denom = lambda_ + (weight * u_px.re);
    if (!(denom > 1e-12f)) return out(y);
    const float inv_denom = 1.0f / denom;

    const Cx error = cx(desired) - y;
    const Cx scaled_error = error * (weight * inv_denom);
    for (int i = 0; i < nff_; ++i) ff_[i] += px_[i] * scaled_error;
    for (int j = 0; j < nfb_; ++j) fb_[j] += px_[nff_ + j] * scaled_error;

    const float inv_lambda = 1.0f / lambda_;
    const float k_scale = weight * inv_denom;
    for (int i = 0; i < n; ++i) {
        const Cx ki = px_[i] * k_scale;
        for (int j = 0; j < n; ++j) {
            p_[ssz(i) * ld + ssz(j)] = (p_[ssz(i) * ld + ssz(j)] - (ki * conj(px_[j]))) * inv_lambda;
        }
    }
    return out(y);
}

void Dfe::symmetrize_p(float p_max) noexcept {
    if (!p_active_) return;
    const int n = ld_;
    const std::size_t ld = ssz(ld_);
    for (int i = 0; i < n; ++i) {
        const float diag = std::min(p_[ssz(i) * ld + ssz(i)].re, p_max);
        p_[ssz(i) * ld + ssz(i)] = cx(diag, 0.0f);
        for (int j = i + 1; j < n; ++j) {
            const Cx avg = (p_[ssz(i) * ld + ssz(j)] + conj(p_[ssz(j) * ld + ssz(i)])) * 0.5f;
            p_[ssz(i) * ld + ssz(j)] = avg;
            p_[ssz(j) * ld + ssz(i)] = conj(avg);
        }
    }
}

}  // inline namespace (precision)

}  // namespace m0lte::ms110d
