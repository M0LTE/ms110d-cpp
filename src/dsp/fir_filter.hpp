// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

// Streaming FIR filter, the C++ counterpart of M0LTE.Dsp FirFilter: one coefficient set, a
// circular history, plain scalar multiply-accumulates in single precision, newest sample first.
// Like filter_design.hpp this module knows nothing of MS110D and is laid out so it can move to
// its own library. The history is a fixed array (MaxTaps floats), so nothing is allocated and
// nothing throws.
//
// One addition over the C# class: push() and output() are separate, so a caller that throws
// away every second output (decimation by two) only pays for the outputs it keeps. next() is
// push() followed by output(), the C# call, and gives the same value.
//
// The multiply-accumulate order is the C# one (coefficient 0 against the newest sample,
// accumulating in float from zero), so with -ffp-contract=off the output is the C# float to the
// last bit.

#include <array>
#include <cstddef>
#include <span>

namespace m0lte::dsp {

template <std::size_t MaxTaps>
class FirFilter {
public:
    // Sets the coefficients and clears the history. Returns false, leaving the filter
    // unconfigured, if there are none or more than MaxTaps.
    bool configure(std::span<const float> coefficients) noexcept {
        taps_ = 0;
        if (coefficients.empty() || coefficients.size() > MaxTaps) return false;
        for (std::size_t i = 0; i < coefficients.size(); ++i) coefficients_[i] = coefficients[i];
        taps_ = coefficients.size();
        clear();
        return true;
    }

    // Clears the history, keeping the coefficients.
    void clear() noexcept {
        history_.fill(0.0f);
        position_ = 0;
        newest_ = 0;
    }

    std::size_t taps() const noexcept { return taps_; }

    // Adds a sample to the history without computing an output.
    void push(float sample) noexcept {
        newest_ = position_;
        history_[position_] = sample;
        if (++position_ == taps_) position_ = 0;
    }

    // The filter output for the history as it stands (the newest pushed sample against
    // coefficient 0).
    float output() const noexcept {
        float acc = 0.0f;
        std::size_t index = newest_;
        for (std::size_t k = 0; k < taps_; ++k) {
            acc += coefficients_[k] * history_[index];
            index = index == 0 ? taps_ - 1 : index - 1;
        }
        return acc;
    }

    // Filters one sample, as the C# Next.
    float next(float sample) noexcept {
        push(sample);
        return output();
    }

private:
    std::array<float, MaxTaps> coefficients_{};
    std::array<float, MaxTaps> history_{};
    std::size_t taps_ = 0;
    std::size_t position_ = 0;  // where the next sample goes
    std::size_t newest_ = 0;    // where the newest sample is
};

}  // namespace m0lte::dsp
