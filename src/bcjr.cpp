// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "bcjr.hpp"

#include <algorithm>
#include <cmath>

namespace m0lte::ms110d {

#ifdef MS110D_SINGLE_PRECISION
inline namespace v_single {
#else
inline namespace v_double {
#endif

namespace {

// The C# sentinel, -1e30f.
template <class T>
constexpr T neg_inf_t = static_cast<T>(-1e30f);
constexpr real neg_inf = neg_inf_t<real>;

template <class T>
struct CxT {
    T re;
    T im;
};
using Cx = CxT<real>;

template <class T, typename U>
inline CxT<T> load_t(const std::complex<U>& c) noexcept {
    return {static_cast<T>(c.real()), static_cast<T>(c.imag())};
}
template <typename U>
inline Cx load(const std::complex<U>& c) noexcept {
    return load_t<real>(c);
}
template <class T>
inline CxT<T> add(CxT<T> a, CxT<T> b) noexcept { return {a.re + b.re, a.im + b.im}; }
template <class T>
inline CxT<T> sub(CxT<T> a, CxT<T> b) noexcept { return {a.re - b.re, a.im - b.im}; }
template <class T>
inline CxT<T> scale(CxT<T> a, T s) noexcept { return {a.re * s, a.im * s}; }
// (a.Re * b.Re) - (a.Im * b.Im), (a.Re * b.Im) + (a.Im * b.Re), as the C# Complex32.
template <class T>
inline CxT<T> mul(CxT<T> a, CxT<T> b) noexcept { return {(a.re * b.re) - (a.im * b.im), (a.re * b.im) + (a.im * b.re)}; }
template <class T>
inline T mag2(CxT<T> a) noexcept { return (a.re * a.re) + (a.im * a.im); }

// a + log(1 + e^(b - a)) for a >= b; the guard skips the exp once the difference is beyond precision.
template <class T>
inline T log_sum_exp(T a, T b) noexcept {
    if (a < b) {
        const T t = a;
        a = b;
        b = t;
    }
    const T diff = b - a;
    return diff < static_cast<T>(-20.0f) ? a : a + std::log(static_cast<T>(1.0f) + std::exp(diff));
}

// Branch metric of the shift register trellis: -|rx - (h1 xN + h2 xDelayed)|^2 / (2 noise).
inline real gamma_for(Cx rx, Cx h1, Cx h2, real xn, real xd, real inv_two_sigma2) noexcept {
    const Cx y_exp = add(scale(h1, xn), scale(h2, xd));
    const Cx diff = sub(rx, y_exp);
    return -mag2(diff) * inv_two_sigma2;
}

// The delayed symbol x[n - delay] is at bit (delay - 1) of the state (state bit i is x[n - 1 - i]).
inline real delayed_symbol(std::size_t state, int delay) noexcept {
    const std::size_t bit = (state >> (delay - 1)) & 1u;
    return bit == 0 ? static_cast<real>(1.0f) : static_cast<real>(-1.0f);
}

}  // namespace

Bcjr::Bcjr(std::size_t max_symbols, int max_delay)
    : valid_(max_delay >= 1 && max_delay <= 24), max_n_(max_symbols), max_delay_(max_delay) {
    if (!valid_) {
        return;
    }
    const std::size_t total = (max_n_ + 1) * (std::size_t{1} << max_delay);
    alpha_.resize(total);
    beta_.resize(total);
}

bool Bcjr::equalize(std::span<const std::complex<real>> rx,
                    std::span<const std::complex<real>> h1,
                    std::span<const std::complex<real>> h2,
                    int delay,
                    real noise_var,
                    std::span<real> llrs) noexcept {
    const std::size_t n = rx.size();
    if (!valid_ || delay < 1 || delay > max_delay_ || n > max_n_ || h1.size() != n || h2.size() != n ||
        llrs.size() != n || !(noise_var > 0)) {
        return false;
    }

    const std::size_t n_states = std::size_t{1} << delay;
    const real inv_two_sigma2 = static_cast<real>(1.0f) / (static_cast<real>(2.0f) * noise_var);
    const std::size_t total = (n + 1) * n_states;
    real* log_alpha = alpha_.data();
    real* log_beta = beta_.data();

    // Forward recursion (log domain), starting in state 0.
    std::fill_n(log_alpha, total, neg_inf);
    log_alpha[0] = 0;
    for (std::size_t t = 0; t < n; ++t) {
        const std::size_t base_cur = t * n_states;
        const std::size_t base_next = (t + 1) * n_states;
        const Cx r = load(rx[t]), a1 = load(h1[t]), a2 = load(h2[t]);
        for (std::size_t s = 0; s < n_states; ++s) {
            const real alpha = log_alpha[base_cur + s];
            if (alpha < neg_inf * static_cast<real>(0.5f)) {
                continue;
            }
            for (std::size_t b = 0; b < 2; ++b) {
                const real xn = b == 0 ? static_cast<real>(1.0f) : static_cast<real>(-1.0f);
                const real gamma = gamma_for(r, a1, a2, xn, delayed_symbol(s, delay), inv_two_sigma2);
                const std::size_t next_state = ((s << 1) | b) & (n_states - 1);
                const real val = alpha + gamma;
                if (val > log_alpha[base_next + next_state]) {
                    log_alpha[base_next + next_state] = val;
                }
            }
        }
    }

    // Backward recursion (log domain), free end state.
    std::fill_n(log_beta, total, neg_inf);
    for (std::size_t s = 0; s < n_states; ++s) {
        log_beta[n * n_states + s] = 0;
    }
    for (std::size_t t = n; t-- > 0;) {
        const std::size_t base_cur = t * n_states;
        const std::size_t base_next = (t + 1) * n_states;
        const Cx r = load(rx[t]), a1 = load(h1[t]), a2 = load(h2[t]);
        for (std::size_t s = 0; s < n_states; ++s) {
            real best = neg_inf;
            for (std::size_t b = 0; b < 2; ++b) {
                const real xn = b == 0 ? static_cast<real>(1.0f) : static_cast<real>(-1.0f);
                const real gamma = gamma_for(r, a1, a2, xn, delayed_symbol(s, delay), inv_two_sigma2);
                const std::size_t next_state = ((s << 1) | b) & (n_states - 1);
                const real val = log_beta[base_next + next_state] + gamma;
                if (val > best) {
                    best = val;
                }
            }
            log_beta[base_cur + s] = best;
        }
    }

    // LLRs: best path metric through bit 0 minus through bit 1.
    for (std::size_t t = 0; t < n; ++t) {
        const std::size_t base_cur = t * n_states;
        const std::size_t base_next = (t + 1) * n_states;
        const Cx r = load(rx[t]), a1 = load(h1[t]), a2 = load(h2[t]);
        real log_p0 = neg_inf, log_p1 = neg_inf;
        for (std::size_t s = 0; s < n_states; ++s) {
            const real alpha = log_alpha[base_cur + s];
            if (alpha < neg_inf * static_cast<real>(0.5f)) {
                continue;
            }
            for (std::size_t b = 0; b < 2; ++b) {
                const real xn = b == 0 ? static_cast<real>(1.0f) : static_cast<real>(-1.0f);
                const real gamma = gamma_for(r, a1, a2, xn, delayed_symbol(s, delay), inv_two_sigma2);
                const std::size_t next_state = ((s << 1) | b) & (n_states - 1);
                const real val = alpha + gamma + log_beta[base_next + next_state];
                if (b == 0) {
                    if (val > log_p0) {
                        log_p0 = val;
                    }
                } else if (val > log_p1) {
                    log_p1 = val;
                }
            }
        }
        llrs[t] = log_p0 - log_p1;
    }
    return true;
}

template <class T>
BasicChainBcjr<T>::BasicChainBcjr(std::size_t max_symbols, std::size_t max_constellation)
    : max_n_(max_symbols), max_m_(max_constellation) {
    inv2_.resize(std::max<std::size_t>(1, max_n_));
    alpha_.resize(max_n_ * max_m_);
    beta_.resize(2 * max_m_);
    post_.resize(max_m_);
}

template <class T>
bool BasicChainBcjr<T>::equalize(const Input& in, std::span<T> llrs) noexcept {
    using C = CxT<T>;
    constexpr T ninf = neg_inf_t<T>;
    const std::size_t n = in.rx.size();
    const std::size_t m = in.constellation.size();
    if (n > max_n_ || m == 0 || m > max_m_ || in.h1.size() != n || in.h2.size() != n || in.delay < 1 ||
        in.bits_per_symbol < 1 || in.bits_per_symbol > 8 ||
        llrs.size() != n * static_cast<std::size_t>(in.bits_per_symbol) ||
        (!in.bit_labels.empty() && in.bit_labels.size() != m) ||
        (!in.preceding.empty() && in.preceding.size() < static_cast<std::size_t>(in.delay)) ||
        (!in.symbol_log_priors.empty() && in.symbol_log_priors.size() != n * m) ||
        (in.noise_var_per_symbol.size() != n && !(in.noise_var > 0))) {
        return false;
    }

    const std::size_t delay = static_cast<std::size_t>(in.delay);
    const std::size_t bps = static_cast<std::size_t>(in.bits_per_symbol);
    const bool has_priors = !in.symbol_log_priors.empty();
    const bool has_preceding = !in.preceding.empty();
    T* inv2 = inv2_.data();
    if (in.noise_var_per_symbol.size() == n && n > 0) {
        for (std::size_t t = 0; t < n; ++t) {
            inv2[t] = static_cast<T>(1.0f) / (static_cast<T>(2.0f) * in.noise_var_per_symbol[t]);
        }
    } else {
        std::fill_n(inv2, n, static_cast<T>(1.0f) / (static_cast<T>(2.0f) * in.noise_var));
    }

    T* alpha = alpha_.data();
    T* beta = beta_.data();
    T* post = post_.data();

    for (std::size_t c = 0; c < std::min(delay, n); ++c) {
        const std::size_t len = ((n - c) + delay - 1) / delay;

        // Forward: alpha[k][s] includes gamma at step k, so the posterior is alpha + beta (exclusive).
        for (std::size_t k = 0; k < len; ++k) {
            const std::size_t t = c + k * delay;
            const std::size_t base_cur = k * m;
            const C r = load_t<T>(in.rx[t]), a1 = load_t<T>(in.h1[t]), a2 = load_t<T>(in.h2[t]);
            for (std::size_t s = 0; s < m; ++s) {
                const T prior = has_priors ? in.symbol_log_priors[t * m + s] : 0;
                const C main = mul(a1, load_t<T>(in.constellation[s]));
                if (k == 0) {
                    const C expected = has_preceding ? add(main, mul(a2, load_t<T>(in.preceding[c]))) : main;
                    alpha[base_cur + s] = (-mag2(sub(r, expected)) * inv2[t]) + prior;
                    continue;
                }

                const std::size_t base_prev = (k - 1) * m;
                T acc = ninf;
                for (std::size_t q = 0; q < m; ++q) {
                    const C expected = add(main, mul(a2, load_t<T>(in.constellation[q])));
                    const T val = alpha[base_prev + q] - (mag2(sub(r, expected)) * inv2[t]);
                    acc = log_sum_exp(acc, val);
                }
                alpha[base_cur + s] = acc + prior;
            }
        }

        // Backward with on the fly posteriors and LLRs, beta ping-ponging between two columns.
        std::size_t cur = 0;
        for (std::size_t s = 0; s < m; ++s) {
            beta[s] = 0;
        }
        for (std::size_t k = len; k-- > 0;) {
            const std::size_t t = c + k * delay;
            const std::size_t base_cur = k * m;
            for (std::size_t s = 0; s < m; ++s) {
                post[s] = alpha[base_cur + s] + beta[cur * m + s];
            }

            for (std::size_t b = 0; b < bps; ++b) {
                T log0 = ninf, log1 = ninf;
                for (std::size_t s = 0; s < m; ++s) {
                    const std::size_t label = in.bit_labels.empty() ? s : in.bit_labels[s];
                    if (((label >> (bps - 1 - b)) & 1u) == 0) {
                        log0 = log_sum_exp(log0, post[s]);
                    } else {
                        log1 = log_sum_exp(log1, post[s]);
                    }
                }
                llrs[t * bps + b] = log0 - log1;
            }

            if (k == 0) {
                break;
            }

            // beta[k-1][q] = logsumexp_s(beta[k][s] + gamma_k(q, s)); gamma carries symbol s's prior.
            const std::size_t next = 1 - cur;
            const C r = load_t<T>(in.rx[t]), a1 = load_t<T>(in.h1[t]), a2 = load_t<T>(in.h2[t]);
            for (std::size_t q = 0; q < m; ++q) {
                const C echo = mul(a2, load_t<T>(in.constellation[q]));
                T acc = ninf;
                for (std::size_t s = 0; s < m; ++s) {
                    const C expected = add(mul(a1, load_t<T>(in.constellation[s])), echo);
                    const T val = beta[cur * m + s] - (mag2(sub(r, expected)) * inv2[t]) +
                                     (has_priors ? in.symbol_log_priors[t * m + s] : 0);
                    acc = log_sum_exp(acc, val);
                }
                beta[next * m + q] = acc;
            }
            cur = next;
        }
    }
    return true;
}

template class BasicChainBcjr<float>;
template class BasicChainBcjr<double>;

}  // inline namespace

}  // namespace m0lte::ms110d
