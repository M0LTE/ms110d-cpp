// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

// The receive front end of the C# Ms110dDemodulator.Process: 9600 Hz audio mixed down by the
// 1800 Hz carrier, the root-raised-cosine matched filter, and every second output kept, giving
// the 4800 Hz "T/2" stream (two samples per symbol). T/2 sample p is made from audio sample
// 2 p + 1. Single precision throughout, operation for operation as the C# (and as the preamble
// detector's own copy, which it equals to the bit), so a stage reading these samples sees the
// C#'s numbers.
//
// Internal (src/ is not installed). No allocation, no exceptions.

#include "dsp/complex_f.hpp"
#include "dsp/fir_filter.hpp"
#include "modulator.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>

namespace m0lte::ms110d {

class RxFrontEnd {
public:
    // Designs the matched filter for the roll-off and clears the state. False, unconfigured, if
    // the roll-off is not valid.
    bool configure(double roll_off) noexcept {
        taps_ = 0;
        std::array<float, max_pulse_taps> pulse{};
        const std::size_t taps = design_pulse(roll_off, pulse);
        if (taps == 0) return false;
        if (!filter_re_.configure({pulse.data(), taps}) || !filter_im_.configure({pulse.data(), taps})) return false;
        // 1800 Hz = 3/16 of a cycle per 9600 Hz sample: sixteenth-turn table, index step 3.
        for (int i = 0; i < 16; ++i) {
            mix_[static_cast<std::size_t>(i)] =
                dsp::from_phase(-static_cast<float>(2.0 * std::numbers::pi * i / 16.0));
        }
        taps_ = taps;
        reset();
        return true;
    }

    bool configured() const noexcept { return taps_ != 0; }

    void reset() noexcept {
        filter_re_.clear();
        filter_im_.clear();
        mix_phase_ = 0;
        decimate_toggle_ = false;
    }

    // Takes one audio sample; true, with the T/2 sample in `out`, on every second one.
    bool push(float sample, dsp::ComplexF& out) noexcept {
        const dsp::ComplexF phasor = mix_[static_cast<std::size_t>(mix_phase_)];
        mix_phase_ = (mix_phase_ + 3) & 15;
        filter_re_.push(phasor.re * sample);
        filter_im_.push(phasor.im * sample);
        decimate_toggle_ = !decimate_toggle_;
        if (decimate_toggle_) return false;  // the odd outputs are thrown away, so never computed
        out = {filter_re_.output(), filter_im_.output()};
        return true;
    }

private:
    dsp::FirFilter<max_pulse_taps> filter_re_;
    dsp::FirFilter<max_pulse_taps> filter_im_;
    std::array<dsp::ComplexF, 16> mix_{};
    std::size_t taps_ = 0;
    int mix_phase_ = 0;
    bool decimate_toggle_ = false;
};

}  // namespace m0lte::ms110d
