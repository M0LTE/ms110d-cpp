// Copyright (C) 2026 Thomas Fanning
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

// The transmit signal path from burst symbols to 9600 Hz audio: pulse shaping and mixing to the
// 1800 Hz sub-carrier (the C# Ms110dModulator from symbols onward). Internal for now: not
// installed and not part of the locked public API; the public modulator comes when the whole
// transmit path exists.
//
// Floating point (see docs/dev/modulator.md): the pulse is designed in double and rounded to
// float exactly as the C# does; symbol scaling, the shaping sums and the mix then run in
// `real` (so float in the single build, double in the default build).

#include <m0lte/ms110d/config.hpp>

#include "tables.hpp"
#include "tx_settings.hpp"

#include <array>
#include <complex>
#include <cstddef>
#include <span>

namespace m0lte::ms110d {

// Native DSP rate: 9600 Hz, 4 samples per symbol at 2400 Bd.
inline constexpr int native_rate = 9600;
inline constexpr int samples_per_symbol = 4;
// The roll-off the standard recommends (D.5.1.5), and the default.
inline constexpr double default_roll_off = 0.35;
// Narrowest roll-off accepted.
inline constexpr double min_roll_off = 0.05;
// Longest pulse: 64 symbols (the cap in pulse_span_symbols) at 4 samples per symbol, plus one.
inline constexpr int max_pulse_symbols = 64;
inline constexpr std::size_t max_pulse_taps = static_cast<std::size_t>(max_pulse_symbols * samples_per_symbol + 1);

// Whether roll_off is in [min_roll_off, 1]. False for NaN.
bool valid_roll_off(double roll_off) noexcept;

// Pulse span in symbols for a roll-off: 16 at 0.35 and wider, growing in proportion (capped at
// 64) for narrower roll-offs. 0 if the roll-off is not valid.
int pulse_span_symbols(double roll_off) noexcept;

// Designs the unit-energy root-raised-cosine pulse at 4 samples per symbol into `out` and returns
// the tap count (span * 4 + 1), or 0 if the roll-off is invalid or `out` is too small.
std::size_t design_pulse(double roll_off, std::span<float> out) noexcept;

// Number of audio samples for a burst of `symbols` symbols with a pulse of `taps` taps.
constexpr std::size_t audio_length(std::size_t symbols, std::size_t taps) noexcept {
    return symbols * static_cast<std::size_t>(samples_per_symbol) + taps;
}

#ifdef MS110D_SINGLE_PRECISION
inline namespace v_single {
#else
inline namespace v_double {
#endif

// Streaming pulse shaper and up-converter. All state is in the object (about 1.5 KB): nothing is
// allocated after construction, whatever the burst length, and nothing throws.
//
// Use: configure(); begin(); then for each burst symbol push_symbol() followed by read() until
// pending() is 0 (each symbol makes 4 samples available, and a sample depends only on symbols
// up to its own, so there is no delay); after the last symbol call end_of_symbols() and keep
// calling read() until done(), which drains the pulse tail (taps() samples). The audio can be
// read in chunks of any size, down to one sample, for example to fill a DMA buffer.
class Modulator {
public:
    // Applies the settings: validates them and designs the pulse. Returns false, leaving the
    // modulator unconfigured, if the waveform number is not implemented, the constraint length is
    // not 7 or 9, or the roll-off is outside [0.05, 1]. Only the amplitude and roll-off shape the
    // signal here; the other fields are validated because the C# modulator validates them.
    bool configure(const TxSettings& settings) noexcept;
    bool configured() const noexcept { return taps_ != 0; }

    // The pulse in use (empty before configure) and its tap count.
    std::span<const float> pulse() const noexcept { return {pulse_.data(), taps_}; }
    std::size_t taps() const noexcept { return taps_; }

    // Starts a new burst: clears the shaping history and the carrier phase.
    void begin() noexcept;

    // Adds the next burst symbol (a unit-magnitude constellation or probe point). Requires
    // configured(), pending() == 0 and no end_of_symbols() yet; otherwise returns false and does
    // nothing. The symbol is scaled by the amplitude.
    bool push_symbol(const tables::Complex32& symbol) noexcept;

    // No more symbols: from now on read() produces the pulse tail.
    void end_of_symbols() noexcept;

    // Samples ready to read for the symbol most recently pushed (0 to 4).
    std::size_t pending() const noexcept { return pending_; }

    // True once end_of_symbols() has been called and every sample has been read.
    bool done() const noexcept { return ended_ && pending_ == 0 && tail_left_ == 0; }

    // Writes up to audio.size() samples of 1800 Hz audio and returns how many: no more than
    // pending() before end_of_symbols(), and as many as asked for (until the tail is drained)
    // after it.
    std::size_t read(std::span<real> audio) noexcept;

    // The same, also writing the baseband (before mixing) for each sample. baseband must be at
    // least audio.size() long.
    std::size_t read(std::span<real> audio, std::span<std::complex<real>> baseband) noexcept;

private:
    void shift_in(real re, real im) noexcept;
    std::size_t read_impl(std::span<real> audio, std::complex<real>* baseband) noexcept;

    static constexpr std::size_t ring_size = max_pulse_taps / samples_per_symbol + 1;  // 65 symbols

    std::array<float, max_pulse_taps> pulse_{};
    std::array<real, 16> carrier_cos_{};
    std::array<real, 16> carrier_sin_{};
    std::array<real, ring_size> ring_re_{};
    std::array<real, ring_size> ring_im_{};
    std::size_t taps_ = 0;
    std::size_t newest_ = 0;     // ring index of the newest symbol
    std::size_t pending_ = 0;    // samples ready
    std::size_t position_ = 0;   // which of the 4 samples of the newest symbol is next
    std::size_t tail_left_ = 0;  // tail samples not yet scheduled
    std::size_t phase_ = 0;      // sample index modulo 16
    real amplitude_ = 0;
    bool ended_ = false;
};

}  // inline namespace

}  // namespace m0lte::ms110d
