// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "modulator.hpp"

#include "dsp/filter_design.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace m0lte::ms110d {

bool valid_roll_off(double roll_off) noexcept {
    return roll_off >= min_roll_off && roll_off <= 1.0;  // false for NaN
}

int pulse_span_symbols(double roll_off) noexcept {
    if (!valid_roll_off(roll_off)) return 0;
    if (roll_off >= default_roll_off) return 16;
    int span = static_cast<int>(std::ceil(16 * default_roll_off / roll_off));
    span += span & 1;
    return std::min(span, max_pulse_symbols);
}

// The C# DesignPulse, step for step: float taps from a double design, the energy summed in double
// from float products, the normaliser rounded to float and applied in float.
std::size_t design_pulse(double roll_off, std::span<float> out) noexcept {
    const int span = pulse_span_symbols(roll_off);
    if (span == 0) return 0;
    const std::size_t taps = static_cast<std::size_t>(span * samples_per_symbol + 1);
    if (out.size() < taps) return 0;

    const double centre = static_cast<double>(taps - 1) / 2.0;
    double energy = 0;
    for (std::size_t i = 0; i < taps; ++i) {
        const double t = (static_cast<double>(i) - centre) / samples_per_symbol;
        const float tap = static_cast<float>(dsp::root_raised_cosine(t, roll_off));
        out[i] = tap;
        energy += static_cast<double>(tap * tap);  // a float product, as in the C#
    }
    const float norm = static_cast<float>(1.0 / std::sqrt(energy));
    for (std::size_t i = 0; i < taps; ++i) out[i] *= norm;
    return taps;
}

#ifdef MS110D_SINGLE_PRECISION
inline namespace v_single {
#else
inline namespace v_double {
#endif

bool Modulator::configure(const TxSettings& s) noexcept {
    taps_ = 0;
    if (!mode_3k(s.waveform_number).has_value()) return false;
    if (s.constraint_length != 7 && s.constraint_length != 9) return false;
    const std::size_t taps = design_pulse(s.roll_off, pulse_);
    if (taps == 0) return false;
    taps_ = taps;
    amplitude_ = static_cast<real>(s.amplitude);

    // 1800 Hz at 9600 Hz is 3/16 of a cycle per sample, so the carrier repeats every 16 samples.
    // The C# evaluates the phase 2 pi 3 i / 16 afresh in double for every sample; the sixteen
    // values here are the same numbers (the phase is exact for the first 16 and the C# error
    // after that is far below float resolution).
    for (std::size_t m = 0; m < 16; ++m) {
        const double phase = 2.0 * std::numbers::pi * 3.0 * static_cast<double>(m) / 16.0;
        carrier_cos_[m] = static_cast<real>(std::cos(phase));
        carrier_sin_[m] = static_cast<real>(std::sin(phase));
    }
    begin();
    return true;
}

void Modulator::begin() noexcept {
    ring_re_.fill(0);
    ring_im_.fill(0);
    newest_ = 0;
    pending_ = 0;
    position_ = 0;
    tail_left_ = 0;
    phase_ = 0;
    ended_ = false;
}

void Modulator::shift_in(real re, real im) noexcept {
    newest_ = newest_ + 1 == ring_size ? 0 : newest_ + 1;
    ring_re_[newest_] = re;
    ring_im_[newest_] = im;
    position_ = 0;
}

bool Modulator::push_symbol(const tables::Complex32& symbol) noexcept {
    if (taps_ == 0 || pending_ != 0 || ended_) return false;
    // Scaling happens in `real`, from the float constellation value, like the C# Complex32 * float
    // (which is a float product in the single build).
    shift_in(static_cast<real>(symbol.real()) * amplitude_, static_cast<real>(symbol.imag()) * amplitude_);
    pending_ = samples_per_symbol;
    return true;
}

void Modulator::end_of_symbols() noexcept {
    if (taps_ == 0 || ended_) return;
    ended_ = true;
    tail_left_ = taps_;
}

std::size_t Modulator::read(std::span<real> audio) noexcept {
    return read_impl(audio, nullptr);
}

std::size_t Modulator::read(std::span<real> audio, std::span<std::complex<real>> baseband) noexcept {
    if (baseband.size() < audio.size()) audio = audio.first(baseband.size());
    return read_impl(audio, baseband.data());
}

std::size_t Modulator::read_impl(std::span<real> audio, std::complex<real>* baseband) noexcept {
    std::size_t produced = 0;
    while (produced < audio.size()) {
        if (pending_ == 0) {
            // Once the symbols have ended, the tail is the pulses of the last symbols running out:
            // the same as pushing zero symbols, 4 samples at a time.
            if (!ended_ || tail_left_ == 0) break;
            shift_in(0, 0);
            pending_ = std::min<std::size_t>(samples_per_symbol, tail_left_);
            tail_left_ -= pending_;
        }

        // Sample p of the newest symbol n is the sum over symbols n - j of s(n - j) * pulse[p + 4 j],
        // added oldest first like the C#. Slots before the burst hold zero.
        const std::size_t p = position_;
        const std::size_t jmax = (taps_ - 1 - p) / samples_per_symbol;
        std::size_t idx = newest_ >= jmax ? newest_ - jmax : newest_ + ring_size - jmax;
        real re = 0;
        real im = 0;
        for (std::size_t j = jmax + 1; j-- > 0;) {
            const real tap = static_cast<real>(pulse_[p + samples_per_symbol * j]);
            re += ring_re_[idx] * tap;
            im += ring_im_[idx] * tap;
            idx = idx + 1 == ring_size ? 0 : idx + 1;
        }

        audio[produced] = re * carrier_cos_[phase_] - im * carrier_sin_[phase_];
        if (baseband != nullptr) baseband[produced] = std::complex<real>(re, im);
        phase_ = (phase_ + 1) & 15u;
        ++position_;
        --pending_;
        ++produced;
    }
    return produced;
}

}  // inline namespace

}  // namespace m0lte::ms110d
