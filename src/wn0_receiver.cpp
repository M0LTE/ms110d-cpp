// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "wn0_receiver.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace m0lte::ms110d {

#ifdef MS110D_SINGLE_PRECISION
inline namespace v_single {
#else
inline namespace v_double {
#endif

namespace {

// The C# Complex32 arithmetic, in the same operation order, in the build's precision.
inline Cx add(Cx a, Cx b) noexcept { return {a.re + b.re, a.im + b.im}; }
inline Cx sub(Cx a, Cx b) noexcept { return {a.re - b.re, a.im - b.im}; }
inline Cx mul(Cx a, Cx b) noexcept { return {(a.re * b.re) - (a.im * b.im), (a.re * b.im) + (a.im * b.re)}; }
inline Cx scale(Cx a, real s) noexcept { return {a.re * s, a.im * s}; }
inline Cx conj(Cx a) noexcept { return {a.re, -a.im}; }
inline real mag2(Cx a) noexcept { return (a.re * a.re) + (a.im * a.im); }
inline real mag(Cx a) noexcept { return std::sqrt((a.re * a.re) + (a.im * a.im)); }
inline Cx from_phase(real phase) noexcept { return {std::cos(phase), std::sin(phase)}; }

// Single-precision constants of the C# (float literals), widened to the build's precision, so
// the double build uses the very numbers the C# does.
constexpr float k_gain_alpha_f = 1.0f / 6;   // about 80 ms one-pole, inside the 1 Hz fade coherence time
constexpr float k_gain_scale_f = 1.0f / 32;  // 1 / ChipsPerSymbol
constexpr int k_warmup_symbols = 8;          // noncoherent selection while the DD gains are cold
constexpr int k_interp_half = 4;             // 8-tap interpolator
constexpr int k_weak_limit = 90;             // about 1.2 s of weak symbols ends the burst

// Walsh candidate s is +-q[0] +-q[1] +-q[2] +-q[3]: sign of quarter-phase j is + when walsh[s][j]
// is 0 (a chip of 0 or 4 in 8PSK, so +1 or -1).
inline Cx walsh_sum(const std::array<Cx, 4>& q, std::size_t s) noexcept {
    Cx acc{};
    for (std::size_t j = 0; j < 4; ++j) acc = tables::walsh[s][j] == 0 ? add(acc, q[j]) : sub(acc, q[j]);
    return acc;
}

}  // namespace

bool Wn0Receiver::configure(double roll_off) noexcept {
    taps_ = 0;
    active_ = false;
    const std::size_t taps = design_pulse(roll_off, pulse_);
    if (taps == 0) return false;
    taps_ = taps;
    // 1800 Hz = 3/16 of a cycle per 9600 Hz sample: sixteenth-turn table, index step 3. The C#
    // takes MathF.Cos/Sin of the float-rounded phase; the same here (computed in double, rounded
    // once), so the entries at the quarter turns are the C#'s tiny non-zeros, not exact 0 and 1.
    for (std::size_t i = 0; i < 16; ++i) {
        const float phase = static_cast<float>(-(2.0 * std::numbers::pi * static_cast<double>(i) / 16.0));
        mix_[i] = {static_cast<real>(static_cast<float>(std::cos(static_cast<double>(phase)))),
                   static_cast<real>(static_cast<float>(std::sin(static_cast<double>(phase))))};
    }
    reset();
    return true;
}

void Wn0Receiver::reset() noexcept {
    hist_re_.fill(0);
    hist_im_.fill(0);
    ring_.fill(Cx{});
    hist_pos_ = 0;
    mix_phase_ = 0;
    decimate_toggle_ = false;
    written_ = 0;
    active_ = false;
    last_ = Wn0Symbol{};
}

void Wn0Receiver::push_audio(std::span<const float> audio) noexcept {
    if (taps_ == 0) return;
    for (const float sample : audio) {
        // Mix to baseband (the table is the 1800 Hz carrier, conjugated), then the matched filter.
        const real s = static_cast<real>(sample);
        const Cx mixed = scale(mix_[mix_phase_], s);
        mix_phase_ = (mix_phase_ + 3) & 15;
        hist_re_[hist_pos_] = mixed.re;
        hist_im_[hist_pos_] = mixed.im;
        const std::size_t newest = hist_pos_;
        if (++hist_pos_ == taps_) hist_pos_ = 0;

        // The C# filters every sample and keeps every second; the skipped outputs are never
        // used, so they are not computed (the history is still updated above).
        decimate_toggle_ = !decimate_toggle_;
        if (decimate_toggle_) continue;

        // FirFilter.Next: newest sample first, plain scalar accumulation.
        real acc_re = 0;
        real acc_im = 0;
        std::size_t index = newest;
        for (std::size_t k = 0; k < taps_; ++k) {
            const real c = static_cast<real>(pulse_[k]);
            acc_re += c * hist_re_[index];
            acc_im += c * hist_im_[index];
            index = index == 0 ? taps_ - 1 : index - 1;
        }
        ring_[static_cast<std::size_t>(written_) & ring_mask] = Cx{acc_re, acc_im};
        ++written_;
    }
}

Cx Wn0Receiver::interpolate(double pos) const noexcept {
    // 8-tap windowed-sinc interpolation, the weights in double and applied in the build's
    // precision, as the C#. Positions outside what has been written (or no longer held)
    // contribute nothing.
    const std::int64_t i0 = static_cast<std::int64_t>(std::floor(pos));
    const double frac = pos - static_cast<double>(i0);
    Cx acc{};
    for (int j = -k_interp_half + 1; j <= k_interp_half; ++j) {
        const double u = static_cast<double>(j) - frac;
        double w;
        if (std::fabs(u) < 1e-9) {
            w = 1;
        } else {
            w = std::sin(std::numbers::pi * u) / (std::numbers::pi * u) *
                (0.5 + (0.5 * std::cos(std::numbers::pi * u / k_interp_half)));
        }
        const std::int64_t idx = i0 + j;
        if (idx >= 0 && idx < written_ && idx + static_cast<std::int64_t>(ring_size) > written_) {
            acc = add(acc, scale(ring_[static_cast<std::size_t>(idx) & ring_mask], static_cast<real>(w)));
        }
    }
    return acc;
}

Cx Wn0Receiver::read_chip(std::int64_t chip) const noexcept {
    const double half_chips = 2.0 * static_cast<double>(chip);
    const double pos = chip0_ + half_chips + tau_;
    const Cx value = interpolate(pos);
    const double theta = theta_base_ + (omega_ * (pos - chip0_));
    return mul(value, from_phase(static_cast<real>(-theta)));
}

bool Wn0Receiver::have_samples_for_chip(double chip) const noexcept {
    const double pos = chip0_ + (2.0 * chip) + tau_;
    return written_ > static_cast<std::int64_t>(std::ceil(pos)) + k_interp_half + 1;
}

bool Wn0Receiver::start(const Wn0Start& s, std::span<real> block_llrs) noexcept {
    if (taps_ == 0) return false;
    const std::uint32_t bits = s.interleaver.size_bits();
    if (bits == 0 || bits % 2 != 0 || !s.interleaver.valid() || block_llrs.size() < bits) return false;
    if (s.data_start_chip < neg_fingers) return false;
    // The oldest half-chip sample the first symbol reads must still be held.
    const double first = s.chip0 + (2.0 * static_cast<double>(s.data_start_chip - neg_fingers)) + s.tau;
    if (static_cast<std::int64_t>(std::floor(first)) - k_interp_half + static_cast<std::int64_t>(ring_size) <= written_)
        return false;

    chip0_ = s.chip0;
    tau_ = s.tau;
    omega_ = s.omega;
    theta_base_ = s.theta_base;
    symbol_chip_ = s.data_start_chip;
    symbol_in_block_ = 0;
    weak_symbols_ = 0;
    phase_acc_ = Cx{};
    phase_count_ = 0;
    interleaver_ = s.interleaver;
    block_ = block_llrs.first(bits);
    scrambler_.reset();
    gains_.fill(Cx{});
    symbols_seen_ = 0;
    last_ = Wn0Symbol{};
    active_ = true;
    return true;
}

Wn0Step Wn0Receiver::step() noexcept {
    if (!active_) return Wn0Step::Inactive;

    // Forward availability: the 32 chips of the symbol plus the finger reach, 38 chips ahead of
    // the symbol boundary, and the interpolator's tail (the anti-causal fingers read history).
    constexpr double forward_chips = rake_chips - neg_fingers;
    if (!have_samples_for_chip(static_cast<double>(symbol_chip_) + forward_chips + 2)) return Wn0Step::NeedMoreAudio;

    std::array<Cx, rake_chips> chips;
    for (std::size_t i = 0; i < rake_chips; ++i)
        chips[i] = read_chip(symbol_chip_ - neg_fingers + static_cast<std::int64_t>(i));

    // One scramble draw per symbol, applied to every finger's window (rotate by the conjugate).
    std::array<Cx, chips_per_symbol> rot;
    for (std::size_t i = 0; i < chips_per_symbol; ++i) {
        const tables::Complex32 p = tables::psk8[static_cast<std::size_t>(scrambler_.next())];
        rot[i] = conj(Cx{static_cast<real>(p.real()), static_cast<real>(p.imag())});
    }

    // Per-finger candidate correlations via quarter-phase sums: the four Walsh rows only depend
    // on i mod 4, so q[j] is the sum over i = j (mod 4) of the descrambled chips, and every
    // candidate is +-q[0] +-q[1] +-q[2] +-q[3].
    std::array<Cx, fingers * 4> corr;
    for (std::size_t k = 0; k < fingers; ++k) {
        std::array<Cx, 4> q{};
        for (std::size_t i = 0; i < chips_per_symbol; ++i) q[i & 3] = add(q[i & 3], mul(chips[k + i], rot[i]));
        for (std::size_t s = 0; s < 4; ++s) corr[(k * 4) + s] = walsh_sum(q, s);
    }

    // MRC decision statistic per candidate; during warm-up (cold gains) the choice is made
    // noncoherently on total finger power instead.
    std::array<real, 4> d;
    std::array<double, 4> power;
    for (std::size_t s = 0; s < 4; ++s) {
        real acc = 0;
        double pow = 0;
        for (std::size_t k = 0; k < fingers; ++k) {
            const Cx c = corr[(k * 4) + s];
            acc += mul(conj(gains_[k]), c).re;
            pow += static_cast<double>(mag2(c));
        }
        d[s] = acc;
        power[s] = pow;
    }

    const bool warmup = symbols_seen_ < k_warmup_symbols;
    std::size_t best = 0;
    for (std::size_t s = 1; s < 4; ++s) {
        const bool wins = warmup ? power[s] > power[best] : d[s] > d[best];
        if (wins) best = s;
    }

    // LLRs always from the MRC statistic: near zero during warm-up (self-erasure, 8 symbols per
    // block, which the code absorbs). Positive means bit 0; [0] is the di-bit's MSB.
    const real llr0 = std::max(d[0], d[1]) - std::max(d[2], d[3]);
    const real llr1 = std::max(d[0], d[2]) - std::max(d[1], d[3]);

    Cx combined{};
    double max_finger_abs = 0;
    for (std::size_t k = 0; k < fingers; ++k) {
        const Cx c = corr[(k * 4) + best];
        combined = add(combined, mul(conj(gains_[k]), c));
        const double a = static_cast<double>(mag(c));
        if (a > max_finger_abs) max_finger_abs = a;
        // The one-pole DD gain update, after the gain has been used for this symbol.
        gains_[k] = add(gains_[k], scale(sub(scale(c, static_cast<real>(k_gain_scale_f)), gains_[k]),
                                         static_cast<real>(k_gain_alpha_f)));
    }
    ++symbols_seen_;

    block_[2 * static_cast<std::size_t>(symbol_in_block_)] = llr0;
    block_[(2 * static_cast<std::size_t>(symbol_in_block_)) + 1] = llr1;
    last_.dibit = static_cast<int>(best);
    last_.llr[0] = llr0;
    last_.llr[1] = llr1;
    last_.combined = combined;
    last_.max_finger_abs = max_finger_abs;
    last_.index = symbol_in_block_;

    // Decision-directed carrier: the MRC-combined winner statistic should be real and positive;
    // its argument is the residual common phase error (the fingers absorb per-path phase).
    // Averaged over 8 channel symbols before it is applied, because the per-symbol estimate at
    // the -6 dB operating point is too noisy to drive the frequency integrator directly.
    phase_acc_ = add(phase_acc_, combined);
    if (++phase_count_ == 8) {
        if (static_cast<double>(mag2(phase_acc_)) > 1e-12) {
            const double err = static_cast<double>(std::atan2(phase_acc_.im, phase_acc_.re));
            // Re-anchor the phase model at this symbol: an omega change must not re-rotate history.
            const double href = (2.0 * static_cast<double>(symbol_chip_ + 16)) + tau_;
            const double d_theta = 0.4 * err;
            const double d_omega = 0.06 * err / 512.0;
            omega_ += d_omega;
            theta_base_ += d_theta - (d_omega * href);
        }
        phase_acc_ = Cx{};
        phase_count_ = 0;
    }

    // Signal-lost discriminator: the winning correlation against the chip energy is about 0.5 at
    // the -6 dB mask point but about 0.23 on noise alone. Weak only when ALL fingers are weak.
    double sum_mag = 0;
    for (std::size_t i = 0; i < chips_per_symbol; ++i) sum_mag += static_cast<double>(mag(chips[neg_fingers + i]));
    last_.chip_sum = sum_mag;
    if (max_finger_abs < 0.35 * sum_mag) {
        if (++weak_symbols_ >= k_weak_limit) {
            active_ = false;
            return Wn0Step::SignalLost;
        }
    } else {
        weak_symbols_ = 0;
    }

    symbol_chip_ += chips_per_symbol;
    ++symbol_in_block_;
    if (2 * symbol_in_block_ == interleaver_.size_bits()) {
        symbol_in_block_ = 0;
        scrambler_.reset();  // the scramble sequence restarts at the interleaver boundary
        return Wn0Step::BlockComplete;
    }
    return Wn0Step::Symbol;
}

Wn0Step Wn0Receiver::poll() noexcept {
    for (;;) {
        const Wn0Step r = step();
        if (r != Wn0Step::Symbol) return r;
    }
}

bool Wn0Receiver::deinterleaved_block(std::span<real> out) const noexcept {
    if (block_.empty()) return false;
    if (out.size() != block_.size()) return false;
    interleaver_.deinterleave(block_.data(), out.data());
    return true;
}

bool Wn0Receiver::decode_block(fec::TailBitingViterbiDecoder& decoder, std::span<std::uint8_t> info) noexcept {
    const std::size_t n = block_.size();
    if (n == 0 || n > max_block_bits || info.size() != n / 2) return false;
    // Fetched order to code order, as Interleaver::deinterleave, converting as it goes.
    std::uint32_t loc = 0;
    const std::uint32_t size = interleaver_.size_bits();
    for (std::size_t i = 0; i < n; ++i) {
        soft_[i] = static_cast<float>(block_[loc]);
        loc += interleaver_.increment();
        if (loc >= size) loc -= size;
    }
    return decoder.decode(std::span<const float>(soft_.data(), n), info);
}

real Wn0Receiver::gain_magnitude(int k) const noexcept {
    if (k < 0 || k >= fingers) return 0;
    return mag(gains_[static_cast<std::size_t>(k)]);
}

}  // inline namespace

}  // namespace m0lte::ms110d
